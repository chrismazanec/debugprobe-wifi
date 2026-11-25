/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/*
 * Network configuration of the WiFi probe. Lives only in the last 4 KiB
 * sector of the Pico's own flash; it is written over USB (config console,
 * see netcfg_console.c) and is never part of any source file or UF2.
 */
#ifndef NETCFG_H_
#define NETCFG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETCFG_MAGIC    0x4746434Eu   /* "NCFG" */
#define NETCFG_VERSION  1u

typedef enum {
    NETCFG_AUTH_WPA2 = 0,     /* WPA2-AES PSK (default) */
    NETCFG_AUTH_WPA3 = 1,     /* WPA3-SAE / WPA2 transition */
    NETCFG_AUTH_OPEN = 2,
} netcfg_auth_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    char     ssid[33];
    char     psk[65];
    char     hostname[33];
    char     country[3];       /* ISO 3166 alpha-2, "XX" = worldwide */
    uint8_t  auth;             /* netcfg_auth_t */
    uint8_t  net_enabled;      /* 0 = WiFi off */
    uint16_t port;             /* CMSIS-DAP TCP port */
    uint16_t reserved;         /* explicit padding, 0 */
    uint32_t allow_net;        /* peer allow list: (ip & mask) == net */
    uint32_t allow_mask;       /* 0 = any peer */
    uint32_t crc32;            /* over everything before this field */
} netcfg_t;

void netcfg_defaults(netcfg_t *c);
uint32_t netcfg_crc32(const void *data, size_t len);
void netcfg_seal(netcfg_t *c);              /* fill magic/version/size/crc */
bool netcfg_valid(const netcfg_t *c);
bool netcfg_has_wifi(const netcfg_t *c);    /* SSID set and enabled */
bool netcfg_peer_allowed(const netcfg_t *c, uint32_t ipv4);

/* "a.b.c.d" -> host order; returns false on syntax error */
bool netcfg_parse_ipv4(const char *s, uint32_t *ip);
/* "a.b.c.d/nn", "a.b.c.d" (= /32) or "any" */
bool netcfg_parse_cidr(const char *s, uint32_t *net, uint32_t *mask);
void netcfg_format_ipv4(uint32_t ip, char *out /* >= 16 */);
int  netcfg_mask_bits(uint32_t mask);

/* Target only (netcfg_flash.c) */
bool netcfg_load(netcfg_t *c);              /* false (and defaults) if flash empty / invalid */
int  netcfg_store(const netcfg_t *c);       /* 0 = ok */
int  netcfg_erase(void);

#endif
