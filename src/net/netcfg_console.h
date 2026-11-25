/*
 * Line-oriented configuration console. Reached by opening the probe's
 * existing USB CDC serial port at the magic baud rate NETCFG_CONSOLE_BAUD;
 * any other baud rate is the normal target UART bridge. Platform
 * independent (host tests drive it directly).
 */
#ifndef NETCFG_CONSOLE_H_
#define NETCFG_CONSOLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netcfg.h"

#define NETCFG_CONSOLE_BAUD 9727u    /* autobaud uses 9728 */

typedef struct {
    void (*write)(const char *s, size_t n, void *ctx);
    int  (*save)(const netcfg_t *c, void *ctx);   /* store in flash and apply; 0 = ok */
    int  (*erase)(void *ctx);                      /* wipe flash sector; 0 = ok */
    void (*reboot)(bool to_bootsel, void *ctx);
    /* Append human readable runtime state (WiFi link, IP, client) to out. */
    void (*status)(char *out, size_t n, void *ctx);
    /* Write the boot / event log (may be NULL). */
    void (*log)(void *ctx);
    /* Leave the console, back to the UART bridge (may be NULL). */
    void (*quit)(void *ctx);
    void *ctx;
} netcfg_console_io_t;

typedef struct {
    netcfg_console_io_t io;
    netcfg_t edit;          /* working copy, committed by "save" */
    netcfg_t saved;         /* what is in flash */
    char     line[160];
    size_t   len;
    bool     hidden;        /* next line is a password: no echo */
    bool     dirty;
    bool     last_cr;
} netcfg_console_t;

/* Typed at any baud rate on the CDC port, this string opens the console
 * ("+++wifiprobe" followed by CR or LF). */
#define NETCFG_CONSOLE_MAGIC "+++wifiprobe"

typedef struct { uint8_t idx; } netcfg_magic_t;
/* Feed bridge-bound bytes; returns true when the magic string + CR/LF completed. */
bool netcfg_magic_feed(netcfg_magic_t *m, const uint8_t *data, size_t n);

void netcfg_console_init(netcfg_console_t *con, const netcfg_console_io_t *io, const netcfg_t *current);
void netcfg_console_banner(netcfg_console_t *con);
void netcfg_console_input(netcfg_console_t *con, const uint8_t *data, size_t n);

#endif
