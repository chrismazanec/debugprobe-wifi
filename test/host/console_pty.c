/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/* Runs the firmware's config console on a pseudo terminal, for testing
 * wifiprobe-provision.py. Prints the slave path; writes the saved config
 * (as the firmware would store it) to argv[1] on every save. */
#define _XOPEN_SOURCE 600
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "netcfg_console.h"

static int mfd;
static const char *out_path;
static time_t saved_at;
static netcfg_t cur;
static void w(const char *s, size_t n, void *c) { (void)c; if (write(mfd, s, n) < 0) {} }
static int sv(const netcfg_t *c, void *x) { (void)x; FILE *f = fopen(out_path, "wb"); fwrite(c, sizeof *c, 1, f); fclose(f); cur = *c; saved_at = time(NULL); return 0; }
static int er(void *x) { (void)x; netcfg_defaults(&cur); return 0; }
static void rb(bool b, void *x) { (void)b; (void)x; exit(0); }
static int in_console;
static void qt(void *x) { (void)x; in_console = 0; }
static void lg(void *x) { (void)x; const char *l = "[    0.010] boot: test log\r\n"; w(l, strlen(l), 0); }
static void st(char *o, size_t n, void *x)
{
    (void)x;
    bool up = saved_at && time(NULL) - saved_at >= 2 && netcfg_has_wifi(&cur);
    snprintf(o, n, "wifi     : %s%s\r\nlast err : 0\r\nip       : %s\r\ndap      : %s:%u\r\nmdns     : %s.local\r\nclient   : none\r\n",
             up ? "connected to " : (netcfg_has_wifi(&cur) ? "joining" : "off (no SSID or 'wifi off')"),
             up ? cur.ssid : "", up ? "192.0.2.10" : "-", up ? "192.0.2.10" : "-", cur.port, cur.hostname);
}

int main(int argc, char **argv)
{
    out_path = argv[1];
    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(mfd); unlockpt(mfd);
    printf("%s\n", ptsname(mfd));
    fflush(stdout);
    netcfg_defaults(&cur);
    netcfg_console_t con;
    netcfg_console_io_t io = { w, sv, er, rb, st, lg, qt, NULL };
    netcfg_magic_t mg = { 0 };
    uint8_t buf[256];
    time_t start = time(NULL);
    while (time(NULL) - start < 60) {
        ssize_t n = read(mfd, buf, sizeof buf);
        if (n > 0 && in_console) netcfg_console_input(&con, buf, (size_t)n);
        else if (n > 0 && netcfg_magic_feed(&mg, buf, (size_t)n)) {   /* bridge mode */
            in_console = 1;
            netcfg_console_init(&con, &io, &cur);
            netcfg_console_banner(&con);
        }
        else usleep(10000);
    }
    return 0;
}
