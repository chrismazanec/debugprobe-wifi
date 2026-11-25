/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "netcfg_console.h"

static void out(netcfg_console_t *con, const char *s)
{
    con->io.write(s, strlen(s), con->io.ctx);
}

/* Static buffers: the console must also be safe on a small task stack. */
static char outf_buf[200];
static char status_buf[1024];

static void outf(netcfg_console_t *con, const char *fmt, const char *a)
{
    char *buf = outf_buf;
    snprintf(buf, sizeof outf_buf, fmt, a);
    out(con, buf);
}

static void ok(netcfg_console_t *con) { out(con, "OK\r\n"); }
static void err(netcfg_console_t *con, const char *m) { outf(con, "ERR %s\r\n", m); }
static void prompt(netcfg_console_t *con) { out(con, con->dirty ? "*> " : "> "); }

void netcfg_console_init(netcfg_console_t *con, const netcfg_console_io_t *io, const netcfg_t *current)
{
    memset(con, 0, sizeof(*con));
    con->io = *io;
    con->edit = *current;
    con->saved = *current;
}

void netcfg_console_banner(netcfg_console_t *con)
{
    out(con, "\r\ndebugprobe-wifi config console (type 'help')\r\n");
    prompt(con);
}

static const char *auth_name(uint8_t a)
{
    switch (a) {
    case NETCFG_AUTH_WPA2: return "wpa2";
    case NETCFG_AUTH_WPA3: return "wpa3";
    case NETCFG_AUTH_OPEN: return "open";
    default: return "?";
    }
}

static void show(netcfg_console_t *con)
{
    const netcfg_t *c = &con->edit;
    char ip[16], buf[64];
    outf(con, "ssid     : %s\r\n", c->ssid[0] ? c->ssid : "(not set)");
    outf(con, "psk      : %s\r\n", c->psk[0] ? "(set, hidden)" : "(empty)");
    outf(con, "auth     : %s\r\n", auth_name(c->auth));
    outf(con, "hostname : %s (mDNS: <hostname>.local)\r\n", c->hostname);
    outf(con, "country  : %s\r\n", c->country);
    snprintf(buf, sizeof buf, "%u", (unsigned)c->port);
    outf(con, "port     : %s\r\n", buf);
    if (c->allow_mask == 0) {
        out(con, "allow    : any\r\n");
    } else {
        netcfg_format_ipv4(c->allow_net, ip);
        snprintf(buf, sizeof buf, "%s/%d", ip, netcfg_mask_bits(c->allow_mask));
        outf(con, "allow    : %s\r\n", buf);
    }
    outf(con, "wifi     : %s\r\n", c->net_enabled ? "on" : "off");
    if (con->dirty)
        out(con, "(unsaved changes - 'save' to store and apply)\r\n");
}

static void help(netcfg_console_t *con)
{
    out(con,
        "show                 settings being edited\r\n"
        "status               WiFi link, IP address, DAP client, diagnostics\r\n"
        "log                  boot / event log\r\n"
        "exit                 leave the console (back to the UART bridge)\r\n"
        "ssid <name>          network name (rest of line, spaces allowed)\r\n"
        "psk                  prompt for the password (not echoed)\r\n"
        "auth wpa2|wpa3|open  security (default wpa2; open clears psk)\r\n"
        "hostname <name>      mDNS name, a-z 0-9 '-', max 32\r\n"
        "country <XX>         WiFi regulatory country, e.g. SK; XX = worldwide\r\n"
        "port <n>             CMSIS-DAP TCP port (default 4441)\r\n"
        "allow <ip/nn>|any    only accept DAP clients from this network\r\n"
        "wifi on|off          enable / disable the radio\r\n"
        "save                 write to flash and (re)connect\r\n"
        "revert               drop unsaved edits\r\n"
        "erase                wipe stored settings (back to defaults)\r\n"
        "reboot | bootsel     restart / restart into the UF2 bootloader\r\n");
}

static char *skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static bool valid_hostname(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32 || s[0] == '-' || s[n - 1] == '-')
        return false;
    for (; *s; s++)
        if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '-'))
            return false;
    return true;
}

static void command(netcfg_console_t *con, char *line)
{
    char *cmd = skip_ws(line);
    rstrip(cmd);
    if (*cmd == 0)
        return;
    char *arg = cmd;
    while (*arg && *arg != ' ' && *arg != '\t')
        arg++;
    if (*arg)
        *arg++ = 0;
    arg = skip_ws(arg);
    netcfg_t *c = &con->edit;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        help(con);
        ok(con);
    } else if (!strcmp(cmd, "show")) {
        show(con);
        ok(con);
    } else if (!strcmp(cmd, "status")) {
        status_buf[0] = 0;
        if (con->io.status)
            con->io.status(status_buf, sizeof status_buf, con->io.ctx);
        out(con, status_buf);
        ok(con);
    } else if (!strcmp(cmd, "log")) {
        if (con->io.log)
            con->io.log(con->io.ctx);
        ok(con);
    } else if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit")) {
        ok(con);
        if (con->io.quit)
            con->io.quit(con->io.ctx);
    } else if (!strcmp(cmd, "ssid")) {
        if (strlen(arg) == 0 || strlen(arg) > 32) {
            err(con, "ssid must be 1..32 characters");
            return;
        }
        strcpy(c->ssid, arg);
        con->dirty = true;
        ok(con);
    } else if (!strcmp(cmd, "psk")) {
        if (*arg) {
            err(con, "type 'psk' alone, then the password on the next line (not echoed)");
            return;
        }
        con->hidden = true;
        out(con, "password: ");
        return;
    } else if (!strcmp(cmd, "auth")) {
        if (!strcmp(arg, "wpa2")) c->auth = NETCFG_AUTH_WPA2;
        else if (!strcmp(arg, "wpa3")) c->auth = NETCFG_AUTH_WPA3;
        else if (!strcmp(arg, "open")) { c->auth = NETCFG_AUTH_OPEN; memset(c->psk, 0, sizeof c->psk); }
        else { err(con, "auth wpa2|wpa3|open"); return; }
        con->dirty = true;
        ok(con);
    } else if (!strcmp(cmd, "hostname")) {
        if (!valid_hostname(arg)) {
            err(con, "hostname: 1..32 of a-z 0-9 '-'");
            return;
        }
        strcpy(c->hostname, arg);
        con->dirty = true;
        ok(con);
    } else if (!strcmp(cmd, "country")) {
        if (strlen(arg) != 2 || !isalpha((unsigned char)arg[0]) || !isalpha((unsigned char)arg[1])) {
            err(con, "country: two letters, e.g. SK, DE, US; XX = worldwide");
            return;
        }
        c->country[0] = (char)toupper((unsigned char)arg[0]);
        c->country[1] = (char)toupper((unsigned char)arg[1]);
        c->country[2] = 0;
        con->dirty = true;
        ok(con);
    } else if (!strcmp(cmd, "port")) {
        unsigned long v = 0;
        const char *p = arg;
        if (!*p) { err(con, "port 1..65535"); return; }
        for (; *p; p++) {
            if (!isdigit((unsigned char)*p)) { err(con, "port 1..65535"); return; }
            v = v * 10 + (unsigned long)(*p - '0');
            if (v > 65535) { err(con, "port 1..65535"); return; }
        }
        if (v == 0) { err(con, "port 1..65535"); return; }
        c->port = (uint16_t)v;
        con->dirty = true;
        ok(con);
    } else if (!strcmp(cmd, "allow")) {
        uint32_t net, mask;
        if (!netcfg_parse_cidr(arg, &net, &mask)) {
            err(con, "allow a.b.c.d/nn | a.b.c.d | any");
            return;
        }
        c->allow_net = net;
        c->allow_mask = mask;
        con->dirty = true;
        ok(con);
    } else if (!strcmp(cmd, "wifi")) {
        if (!strcmp(arg, "on")) c->net_enabled = 1;
        else if (!strcmp(arg, "off")) c->net_enabled = 0;
        else { err(con, "wifi on|off"); return; }
        con->dirty = true;
        ok(con);
    } else if (!strcmp(cmd, "save")) {
        if (c->auth != NETCFG_AUTH_OPEN && c->ssid[0] && strlen(c->psk) < 8) {
            err(con, "psk must be 8..63 characters for wpa2/wpa3 (or 'auth open')");
            return;
        }
        netcfg_seal(c);
        if (con->io.save(c, con->io.ctx) != 0) {
            err(con, "flash write failed");
            return;
        }
        con->saved = *c;
        con->dirty = false;
        ok(con);
    } else if (!strcmp(cmd, "revert")) {
        con->edit = con->saved;
        con->dirty = false;
        ok(con);
    } else if (!strcmp(cmd, "erase")) {
        if (con->io.erase(con->io.ctx) != 0) {
            err(con, "flash erase failed");
            return;
        }
        netcfg_defaults(c);
        con->saved = *c;
        con->dirty = false;
        ok(con);
    } else if (!strcmp(cmd, "reboot")) {
        ok(con);
        con->io.reboot(false, con->io.ctx);
    } else if (!strcmp(cmd, "bootsel")) {
        ok(con);
        con->io.reboot(true, con->io.ctx);
    } else {
        err(con, "unknown command, type 'help'");
    }
}

static void password_line(netcfg_console_t *con, const char *pw)
{
    size_t n = strlen(pw);
    out(con, "\r\n");
    if (n < 8 || n > 63) {
        err(con, "password must be 8..63 characters (unchanged)");
        return;
    }
    memset(con->edit.psk, 0, sizeof con->edit.psk);
    memcpy(con->edit.psk, pw, n);
    if (con->edit.auth == NETCFG_AUTH_OPEN)
        con->edit.auth = NETCFG_AUTH_WPA2;
    con->dirty = true;
    ok(con);
}

void netcfg_console_input(netcfg_console_t *con, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = data[i];
        if (ch == '\n' && con->last_cr) {   /* CR LF counts once */
            con->last_cr = false;
            continue;
        }
        con->last_cr = (ch == '\r');
        if (ch == '\r' || ch == '\n') {
            con->line[con->len] = 0;
            if (con->hidden) {
                con->hidden = false;
                password_line(con, con->line);
            } else {
                out(con, "\r\n");
                command(con, con->line);
            }
            memset(con->line, 0, sizeof con->line);   /* do not keep the password around */
            con->len = 0;
            if (!con->hidden)
                prompt(con);
        } else if (ch == 0x7F || ch == 0x08) {
            if (con->len) {
                con->len--;
                if (!con->hidden)
                    out(con, "\b \b");
            }
        } else if (ch == 0x03) {             /* Ctrl-C: abandon line */
            memset(con->line, 0, sizeof con->line);
            con->len = 0;
            con->hidden = false;
            out(con, "^C\r\n");
            prompt(con);
        } else if (ch >= 0x20 && ch < 0x7F) {
            if (con->len < sizeof(con->line) - 1) {
                con->line[con->len++] = (char)ch;
                if (!con->hidden) {
                    char e[2] = { (char)ch, 0 };
                    out(con, e);
                }
            }
        }
    }
}

bool netcfg_magic_feed(netcfg_magic_t *m, const uint8_t *data, size_t n)
{
    static const char magic[] = NETCFG_CONSOLE_MAGIC;
    const size_t len = sizeof(magic) - 1;
    bool hit = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (m->idx == len) {
            if (b == '\r' || b == '\n') {
                hit = true;
                m->idx = 0;
                continue;
            }
            m->idx = 0;
        }
        if (b == (uint8_t)magic[m->idx])
            m->idx++;
        else if (b == '+')                 /* prefix is "+++": a longer run of '+' stays matched */
            m->idx = (m->idx == 3) ? 3 : 1;
        else
            m->idx = 0;
    }
    return hit;
}
