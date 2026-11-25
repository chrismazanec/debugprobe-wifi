#include <stdio.h>
#include <string.h>
#include "netcfg.h"

void netcfg_defaults(netcfg_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->hostname, "dapprobe");
    strcpy(c->country, "XX");
    c->auth = NETCFG_AUTH_WPA2;
    c->net_enabled = 1;
    c->port = 4441;
    c->allow_net = 0;
    c->allow_mask = 0;
    netcfg_seal(c);
}

uint32_t netcfg_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void netcfg_seal(netcfg_t *c)
{
    c->magic = NETCFG_MAGIC;
    c->version = NETCFG_VERSION;
    c->size = sizeof(netcfg_t);
    c->crc32 = netcfg_crc32(c, offsetof(netcfg_t, crc32));
}

static bool str_terminated(const char *s, size_t n)
{
    return memchr(s, 0, n) != NULL;
}

bool netcfg_valid(const netcfg_t *c)
{
    if (c->magic != NETCFG_MAGIC || c->version != NETCFG_VERSION || c->size != sizeof(netcfg_t))
        return false;
    if (c->crc32 != netcfg_crc32(c, offsetof(netcfg_t, crc32)))
        return false;
    if (!str_terminated(c->ssid, sizeof c->ssid) || !str_terminated(c->psk, sizeof c->psk) ||
        !str_terminated(c->hostname, sizeof c->hostname) || !str_terminated(c->country, sizeof c->country))
        return false;
    if (c->auth > NETCFG_AUTH_OPEN || c->port == 0)
        return false;
    return true;
}

bool netcfg_has_wifi(const netcfg_t *c)
{
    return c->net_enabled && c->ssid[0] != 0;
}

bool netcfg_peer_allowed(const netcfg_t *c, uint32_t ipv4)
{
    return (ipv4 & c->allow_mask) == (c->allow_net & c->allow_mask);
}

bool netcfg_parse_ipv4(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t octet = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            octet = octet * 10u + (uint32_t)(*s++ - '0');
            if (++digits > 3 || octet > 255)
                return false;
        }
        v = (v << 8) | octet;
        if (i < 3) {
            if (*s++ != '.')
                return false;
        }
    }
    if (*s != 0)
        return false;
    *ip = v;
    return true;
}

bool netcfg_parse_cidr(const char *s, uint32_t *net, uint32_t *mask)
{
    if (strcmp(s, "any") == 0) {
        *net = 0;
        *mask = 0;
        return true;
    }
    char buf[24];
    size_t n = strlen(s);
    if (n >= sizeof buf)
        return false;
    memcpy(buf, s, n + 1);
    int bits = 32;
    char *slash = strchr(buf, '/');
    if (slash) {
        *slash++ = 0;
        if (*slash == 0)
            return false;
        bits = 0;
        for (const char *p = slash; *p; p++) {
            if (*p < '0' || *p > '9')
                return false;
            bits = bits * 10 + (*p - '0');
            if (bits > 32)
                return false;
        }
    }
    uint32_t ip;
    if (!netcfg_parse_ipv4(buf, &ip))
        return false;
    uint32_t m = bits == 0 ? 0 : (0xFFFFFFFFu << (32 - bits));
    *mask = m;
    *net = ip & m;
    return true;
}

void netcfg_format_ipv4(uint32_t ip, char *out)
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
             (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
}

int netcfg_mask_bits(uint32_t mask)
{
    int n = 0;
    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}
