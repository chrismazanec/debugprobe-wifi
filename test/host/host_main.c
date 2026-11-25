/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/*
 * Host harness: the firmware's DAP TCP server + the real CMSIS-DAP core
 * (DAP.c) + debugprobe's SWD layer (sw_dp_pio.c), talking to a simulated
 * nRF52840 (swd_sim.c). OpenOCD / pyOCD connect to it exactly as they would
 * to the Pico 2 W.
 *
 *   daptcp-sim [-p port] [--protected] [--allow cidr] [--once] [--dump file]
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "DAP_config.h"
#include "DAP.h"
#include "dap_tcp_server.h"
#include "netcfg.h"
#include "swd_sim.h"

static volatile sig_atomic_t stop;
static bool once;
static int served;
static uint32_t allow_net, allow_mask;
static unsigned long commands, rejected;

static size_t exec(const uint8_t *req, size_t len, uint8_t *resp, void *ctx)
{
    (void)len; (void)ctx;
    commands++;
    return DAP_ExecuteCommand(req, resp) & 0xFFFF;
}

static bool allow(uint32_t ip, void *ctx)
{
    (void)ctx;
    bool ok = (ip & allow_mask) == (allow_net & allow_mask);
    if (!ok) {
        rejected++;
        fprintf(stderr, "sim: rejected peer %u.%u.%u.%u\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    }
    return ok;
}

static void on_client(bool c, uint32_t ip, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "sim: client %u.%u.%u.%u %s\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
            c ? "connected" : "disconnected");
    if (!c) {
        uint8_t d[4096] = { ID_DAP_Disconnect }, r[4096];
        DAP_ExecuteCommand(d, r);
        served++;
    }
}

static bool keep(void *ctx)
{
    (void)ctx;
    return !stop && !(once && served > 0);
}

static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
    int port = 4441;
    bool prot = false;
    const char *dump = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--protected")) prot = true;
        else if (!strcmp(argv[i], "--once")) once = true;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--allow") && i + 1 < argc) {
            if (!netcfg_parse_cidr(argv[++i], &allow_net, &allow_mask)) { fprintf(stderr, "bad cidr\n"); return 2; }
        } else { fprintf(stderr, "usage: %s [-p port] [--protected] [--allow cidr] [--once] [--dump file]\n", argv[0]); return 2; }
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    sim_init(prot);
    DAP_Setup();
    dap_tcp_server_cfg_t cfg = {
        .port = (uint16_t)port, .allow = allow, .execute = exec, .on_client = on_client,
        .keep_running = keep, .ctx = NULL, .idle_timeout_ms = 0,
    };
    fprintf(stderr, "sim: listening on %d%s\n", port, prot ? " (APPROTECT on)" : "");
    int rc = dap_tcp_server_run(&cfg);
    fprintf(stderr, "sim: dap commands=%lu rejected_peers=%lu\n", commands, rejected);
    sim_print_stats(stderr);
    if (dump && sim_dump(dump) != 0) { perror("dump"); return 1; }
    return rc < 0 ? 1 : 0;
}
