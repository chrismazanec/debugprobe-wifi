/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/*
 * Bit-level SWD target simulator for host tests.
 *
 * Implements debugprobe's probe_* pin layer (probe.h) so that the REAL
 * CMSIS-DAP core (DAP.c) and debugprobe's REAL SWD layer (sw_dp_pio.c) run
 * unmodified on the host. On the other side of the "wire" sits a model of
 * an nRF52840: SW-DP (DPv1, IDCODE 0x2BA01477), AHB-AP (AP0) with a memory
 * map (flash + NVMC write rules, UICR, FICR, RAM, minimal Cortex-M4 debug
 * registers and ROM table), and the Nordic CTRL-AP (AP1, ERASEALL,
 * APPROTECTSTATUS).
 *
 * What is NOT modelled: CPU execution (no flash algorithms; OpenOCD must
 * use its "slow" NVMC path), real timing, electrical behaviour.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "swd_sim.h"

typedef unsigned int uint;

/* ---------------- statistics ---------------- */
sim_stats_t sim_stats;

/* ---------------- memory model ---------------- */

#define FLASH_SIZE   (1024u * 1024u)
#define RAM_BASE     0x20000000u
#define RAM_SIZE     (256u * 1024u)
#define FICR_BASE    0x10000000u
#define UICR_BASE    0x10001000u
#define UICR_SIZE    0x1000u
#define NVMC_BASE    0x4001E000u

static uint8_t flash[FLASH_SIZE];
static uint8_t uicr[UICR_SIZE];
static uint8_t ram[RAM_SIZE];
static uint32_t nvmc_config;
static bool approtect;
static int eraseall_busy;     /* CTRL-AP ERASEALL in progress: reads left */
static uint32_t ctrlap_eraseall;
static uint32_t ctrlap_reset;

/* sparse store for PPB / peripherals */
#define SPARSE_N 4096
static struct { uint32_t addr; uint32_t val; bool used; } sparse[SPARSE_N];

static uint32_t *sparse_slot(uint32_t addr, bool create)
{
    uint32_t h = (addr >> 2) * 2654435761u;
    for (uint32_t i = 0; i < SPARSE_N; i++) {
        uint32_t k = (h + i) % SPARSE_N;
        if (sparse[k].used && sparse[k].addr == addr)
            return &sparse[k].val;
        if (!sparse[k].used) {
            if (!create)
                return NULL;
            sparse[k].used = true;
            sparse[k].addr = addr;
            sparse[k].val = 0;
            return &sparse[k].val;
        }
    }
    return NULL;
}

/* Cortex-M debug */
static uint32_t dhcsr_ctrl;       /* C_* bits as written */
static bool core_halted;
static bool reset_sticky, retire_sticky;
static uint32_t dcrdr, regs[128];
static uint32_t demcr, dfsr, cpacr;

static void core_reset(void)
{
    nvmc_config = 0;
    reset_sticky = true;
    sim_stats.resets++;
    if (demcr & 1u) {                   /* VC_CORERESET */
        core_halted = true;
        dfsr |= (1u << 3);              /* VCATCH */
    } else {
        core_halted = (dhcsr_ctrl & 2u) && (dhcsr_ctrl & 1u);
    }
    /* reset values of SP/PC from the vector table */
    memcpy(&regs[13], &flash[0], 4);
    memcpy(&regs[15], &flash[4], 4);
    regs[16] = 0x01000000u;             /* xPSR T bit */
}

static void erase_all(void)
{
    memset(flash, 0xFF, sizeof flash);
    memset(uicr, 0xFF, sizeof uicr);
    approtect = false;
    sim_stats.mass_erases++;
}

static uint8_t ficr_byte(uint32_t off)
{
    uint32_t w;
    switch (off & ~3u) {
    case 0x010: w = 0x1000; break;          /* CODEPAGESIZE */
    case 0x014: w = 256; break;             /* CODESIZE */
    case 0x060: w = 0x12345678; break;      /* DEVICEID[0] */
    case 0x064: w = 0x9ABCDEF0; break;      /* DEVICEID[1] */
    case 0x0A4: w = 0xFFFFFFFE; break;      /* DEVICEADDRTYPE */
    case 0x100: w = 0x00052840; break;      /* INFO.PART */
    case 0x104: w = 0x41414430; break;      /* INFO.VARIANT "AAD0" */
    case 0x108: w = 0x00002004; break;      /* INFO.PACKAGE QI */
    case 0x10C: w = 0x00000100; break;      /* INFO.RAM 256 kB */
    case 0x110: w = 0x00000400; break;      /* INFO.FLASH 1 MB */
    default: w = 0xFFFFFFFF; break;
    }
    return (uint8_t)(w >> (8 * (off & 3u)));
}

static const uint8_t romtable_cid[4] = { 0x0D, 0x10, 0x05, 0xB1 };
static const uint8_t romtable_pid[8] = { 0xC4, 0xB4, 0x0B, 0x00, 0x04, 0, 0, 0 };
static const uint8_t scs_cid[4] = { 0x0D, 0xE0, 0x05, 0xB1 };
static const uint8_t scs_pid[8] = { 0x0C, 0xB0, 0x0B, 0x00, 0x04, 0, 0, 0 };
static const uint8_t dwt_pid[8] = { 0x02, 0xB0, 0x3B, 0x00, 0x04, 0, 0, 0 };
static const uint8_t fpb_pid[8] = { 0x03, 0xB0, 0x2B, 0x00, 0x04, 0, 0, 0 };

/* CoreSight ID block at the top of a 4 KiB component */
static bool id_block(uint32_t addr, const uint8_t *pid, const uint8_t *cid, uint32_t *out)
{
    uint32_t off = addr & 0xFFFu;
    if (off >= 0xFD0 && off < 0xFE0) { *out = pid[4 + (off - 0xFD0) / 4]; return true; }
    if (off >= 0xFE0 && off < 0xFF0) { *out = pid[(off - 0xFE0) / 4]; return true; }
    if (off >= 0xFF0) { *out = cid[(off - 0xFF0) / 4]; return true; }
    return false;
}

/* 32-bit aligned read of the system bus. false = bus error. */
static bool bus_read32(uint32_t a, uint32_t *v)
{
    a &= ~3u;
    if (a < FLASH_SIZE) { memcpy(v, &flash[a], 4); return true; }
    if (a >= FICR_BASE && a < FICR_BASE + 0x1000) {
        uint32_t o = a - FICR_BASE;
        *v = ficr_byte(o) | (ficr_byte(o + 1) << 8) | (ficr_byte(o + 2) << 16) | ((uint32_t)ficr_byte(o + 3) << 24);
        return true;
    }
    if (a >= UICR_BASE && a < UICR_BASE + UICR_SIZE) { memcpy(v, &uicr[a - UICR_BASE], 4); return true; }
    if (a >= RAM_BASE && a < RAM_BASE + RAM_SIZE) { memcpy(v, &ram[a - RAM_BASE], 4); return true; }
    if (a >= NVMC_BASE && a < NVMC_BASE + 0x1000) {
        switch (a - NVMC_BASE) {
        case 0x400: *v = 1; return true;            /* READY */
        case 0x408: *v = 1; return true;            /* READYNEXT */
        case 0x504: *v = nvmc_config; return true;  /* CONFIG */
        default: *v = 0; return true;
        }
    }
    /* Cortex-M4 private peripheral bus */
    if (a >= 0xE00FF000u && a < 0xE0100000u) {
        uint32_t o = a - 0xE00FF000u;
        if (id_block(a, romtable_pid, romtable_cid, v)) return true;
        switch (o) {
        case 0x000: *v = 0xFFF0F003u; return true;  /* SCS  @ 0xE000E000 */
        case 0x004: *v = 0xFFF02003u; return true;  /* DWT  @ 0xE0001000 */
        case 0x008: *v = 0xFFF03003u; return true;  /* FPB  @ 0xE0002000 */
        case 0xFCC: *v = 1; return true;            /* MEMTYPE: system memory present */
        default: *v = 0; return true;
        }
    }
    if (a >= 0xE000E000u && a < 0xE000F000u) {
        if (id_block(a, scs_pid, scs_cid, v)) return true;
        switch (a) {
        case 0xE000ED00u: *v = 0x410FC241u; return true;   /* CPUID: Cortex-M4 r0p1 */
        case 0xE000ED30u: *v = dfsr; return true;
        case 0xE000ED88u: *v = cpacr; return true;
        case 0xE000EDF0u: {
            uint32_t s = (dhcsr_ctrl & 0xFu) | (1u << 16);  /* S_REGRDY */
            if (core_halted) s |= 1u << 17;
            if (retire_sticky) s |= 1u << 24;
            if (reset_sticky) s |= 1u << 25;
            reset_sticky = false;
            retire_sticky = false;
            *v = s;
            return true;
        }
        case 0xE000EDF8u: *v = dcrdr; return true;
        case 0xE000EDFCu: *v = demcr; return true;
        case 0xE000EF40u: *v = 0x10110021u; return true;   /* MVFR0 */
        case 0xE000EF44u: *v = 0x11000011u; return true;   /* MVFR1 */
        default: break;
        }
    }
    if (a >= 0xE0001000u && a < 0xE0002000u) {
        if (id_block(a, dwt_pid, scs_cid, v)) return true;
        if (a == 0xE0001000u) { *v = 0x40000000u; return true; }   /* DWT_CTRL: 4 comparators */
    }
    if (a >= 0xE0002000u && a < 0xE0003000u) {
        if (id_block(a, fpb_pid, scs_cid, v)) return true;
        if (a == 0xE0002000u) {
            uint32_t *p = sparse_slot(a, false);
            *v = 0x00000260u | (p ? (*p & 1u) : 0);   /* 6 code + 2 literal comparators */
            return true;
        }
    }
    if ((a >= 0xE0000000u && a < 0xE0100000u) || (a >= 0x40000000u && a < 0x60000000u)) {
        uint32_t *p = sparse_slot(a, false);
        *v = p ? *p : 0;
        return true;
    }
    return false;
}

static void nvm_write(uint8_t *base, uint32_t off, uint32_t val, uint32_t mask)
{
    /* flash cells can only go 1 -> 0, and only with CONFIG.WEN */
    if (nvmc_config != 1) {
        sim_stats.nvm_writes_rejected++;
        return;
    }
    uint32_t old;
    memcpy(&old, &base[off], 4);
    uint32_t nv = old & (val | ~mask);
    memcpy(&base[off], &nv, 4);
    sim_stats.nvm_writes++;
}

static bool bus_write32(uint32_t a, uint32_t val, uint32_t mask)
{
    a &= ~3u;
    if (a < FLASH_SIZE) { nvm_write(flash, a, val, mask); return true; }
    if (a >= UICR_BASE && a < UICR_BASE + UICR_SIZE) { nvm_write(uicr, a - UICR_BASE, val, mask); return true; }
    if (a >= FICR_BASE && a < FICR_BASE + 0x1000) return true;   /* read-only, ignored */
    if (a >= RAM_BASE && a < RAM_BASE + RAM_SIZE) {
        uint32_t old;
        memcpy(&old, &ram[a - RAM_BASE], 4);
        old = (old & ~mask) | (val & mask);
        memcpy(&ram[a - RAM_BASE], &old, 4);
        return true;
    }
    if (a >= NVMC_BASE && a < NVMC_BASE + 0x1000) {
        switch (a - NVMC_BASE) {
        case 0x504: nvmc_config = val & 3u; return true;
        case 0x508:                                   /* ERASEPAGE */
            if (nvmc_config == 2 && val < FLASH_SIZE) {
                memset(&flash[val & ~0xFFFu], 0xFF, 0x1000);
                sim_stats.page_erases++;
            }
            return true;
        case 0x50C:                                   /* ERASEALL */
            if (nvmc_config == 2 && (val & 1u)) {
                memset(flash, 0xFF, sizeof flash);
                memset(uicr, 0xFF, sizeof uicr);
                sim_stats.nvmc_eraseall++;
            }
            return true;
        case 0x514:                                   /* ERASEUICR */
            if (nvmc_config == 2 && (val & 1u))
                memset(uicr, 0xFF, sizeof uicr);
            return true;
        default: return true;
        }
    }
    if (a >= 0xE000E000u && a < 0xE000F000u) {
        switch (a) {
        case 0xE000ED0Cu:                             /* AIRCR */
            if ((val >> 16) == 0x05FAu && (val & (1u << 2)))
                core_reset();
            return true;
        case 0xE000ED30u: dfsr &= ~val; return true;  /* W1C */
        case 0xE000ED88u: cpacr = val; return true;
        case 0xE000EDF0u:                             /* DHCSR */
            if ((val >> 16) != 0xA05Fu)
                return true;
            dhcsr_ctrl = val & 0xFu;
            if ((val & 1u) && (val & 2u)) {
                if (!core_halted) dfsr |= 1u;         /* HALTED */
                core_halted = true;
            } else if ((val & 1u) && !(val & 2u)) {
                core_halted = false;                  /* "running" (nothing executes) */
                retire_sticky = true;
            }
            return true;
        case 0xE000EDF4u:                             /* DCRSR */
            if (val & (1u << 16)) regs[val & 0x7Fu] = dcrdr;
            else dcrdr = regs[val & 0x7Fu];
            return true;
        case 0xE000EDF8u: dcrdr = val; return true;
        case 0xE000EDFCu: demcr = val; return true;
        default: break;
        }
    }
    if ((a >= 0xE0000000u && a < 0xE0100000u) || (a >= 0x40000000u && a < 0x60000000u)) {
        uint32_t *p = sparse_slot(a, true);
        if (p) *p = (*p & ~mask) | (val & mask);
        return true;
    }
    return false;
}

/* ---------------- ADIv5 DP / AP model ---------------- */

static uint32_t dp_ctrlstat;     /* writable bits */
static bool sticky_err;
static uint32_t dp_select;
static uint32_t ap_rdbuff;       /* posted read buffer */
static uint32_t csw = 0x03000002u, tar;

#define CSW_SIZE(c)    ((c) & 7u)
#define CSW_ADDRINC(c) (((c) >> 4) & 3u)

static bool memap_access(bool rnw, uint32_t *data)
{
    if (approtect) {
        sim_stats.protected_accesses++;
        return false;
    }
    uint32_t size = CSW_SIZE(csw);
    uint32_t bytes = size == 0 ? 1 : size == 1 ? 2 : 4;
    uint32_t lane = tar & 3u;
    bool ok;
    if (rnw) {
        uint32_t w;
        ok = bus_read32(tar, &w);
        *data = ok ? w : 0;            /* data appears in its byte lane */
        sim_stats.mem_reads++;
    } else {
        uint32_t mask = bytes == 4 ? 0xFFFFFFFFu : (((1u << (8 * bytes)) - 1u) << (8 * lane));
        ok = bus_write32(tar, *data, mask);
        sim_stats.mem_writes++;
    }
    if (CSW_ADDRINC(csw) == 1)
        tar += bytes;
    return ok;
}

/* returns false on AP transaction error (sets STICKYERR) */
static bool ap_access(bool rnw, uint32_t a32, uint32_t *data)
{
    uint32_t apsel = dp_select >> 24;
    uint32_t reg = (dp_select & 0xF0u) | (a32 & 0xCu);
    bool ok = true;
    if (apsel == 0) {
        switch (reg) {
        case 0x00:
            if (rnw) *data = csw | (1u << 6);   /* DeviceEn */
            else csw = (*data & ~(1u << 6) & ~(1u << 7)) | 0;
            break;
        case 0x04: if (rnw) *data = tar; else tar = *data; break;
        case 0x0C: ok = memap_access(rnw, data); break;
        case 0x10: case 0x14: case 0x18: case 0x1C: {
            uint32_t save = tar;
            tar = (tar & ~0xFu) | (reg & 0xCu);
            uint32_t save_csw = csw;
            csw &= ~0x30u;                        /* banked access: no increment */
            ok = memap_access(rnw, data);
            csw = save_csw;
            tar = save;
            break;
        }
        case 0xF4: if (rnw) *data = 0; break;               /* CFG */
        case 0xF8: if (rnw) *data = 0xE00FF003u; break;     /* BASE */
        case 0xFC: if (rnw) *data = 0x24770011u; break;     /* IDR AHB-AP */
        default: if (rnw) *data = 0; break;
        }
    } else if (apsel == 1) {                    /* Nordic CTRL-AP */
        switch (reg) {
        case 0x000:
            if (rnw) *data = ctrlap_reset;
            else {
                if ((ctrlap_reset & 1u) && !(*data & 1u))
                    core_reset();               /* release of soft reset */
                ctrlap_reset = *data & 1u;
            }
            break;
        case 0x004:
            if (rnw) *data = ctrlap_eraseall;
            else {
                if (!(ctrlap_eraseall & 1u) && (*data & 1u)) {
                    erase_all();
                    eraseall_busy = 3;          /* report busy for a few polls */
                }
                ctrlap_eraseall = *data & 1u;
            }
            break;
        case 0x008:
            if (rnw) {
                *data = eraseall_busy > 0 ? 1u : 0u;
                if (eraseall_busy > 0) eraseall_busy--;
            }
            break;
        case 0x00C: if (rnw) *data = approtect ? 0u : 1u; break;
        case 0x0FC: if (rnw) *data = 0x02880000u; break;
        default: if (rnw) *data = 0; break;
        }
    } else {
        if (rnw) *data = 0;                     /* no AP: IDR reads 0 */
    }
    if (!ok)
        sticky_err = true;
    return ok;
}

/* one complete SWD transaction; returns ACK (1 OK, 2 WAIT, 4 FAULT) */
static uint32_t swd_transaction(bool apndp, bool rnw, uint32_t a32, uint32_t *data)
{
    sim_stats.transactions++;
    if (!apndp) {
        switch (a32) {
        case 0x0:
            if (rnw) *data = 0x2BA01477u;               /* DPIDR */
            else {                                      /* ABORT */
                if (*data & (1u << 2)) sticky_err = false;
            }
            return 1;
        case 0x4:
            if (rnw) {
                uint32_t v = dp_ctrlstat & 0x54000F0Fu;
                if (dp_ctrlstat & (1u << 30)) v |= 1u << 31;
                if (dp_ctrlstat & (1u << 28)) v |= 1u << 29;
                if (dp_ctrlstat & (1u << 26)) v |= 1u << 27;
                if (sticky_err) v |= 1u << 5;
                *data = v;
            } else {
                dp_ctrlstat = *data;
                if (*data & (1u << 5)) sticky_err = false;   /* STICKYERR is W1C in JTAG-DP only; harmless */
            }
            return 1;
        case 0x8:
            if (rnw) *data = ap_rdbuff;                 /* RESEND */
            else dp_select = *data;
            return 1;
        case 0xC:
            if (rnw) *data = ap_rdbuff;                 /* RDBUFF */
            return 1;
        }
        return 1;
    }
    /* AP access */
    if (sticky_err) {
        sim_stats.faults++;
        return 4;
    }
    if (!(dp_ctrlstat & (1u << 28))) {   /* debug domain not powered */
        sim_stats.faults++;
        sticky_err = true;
        return 4;
    }
    if (rnw) {
        uint32_t v = 0;
        ap_access(true, a32, &v);
        *data = ap_rdbuff;                              /* posted */
        ap_rdbuff = v;
    } else {
        ap_access(false, a32, data);
    }
    return 1;
}

/* ---------------- wire level ---------------- */

enum wstate { W_IDLE, W_REQ, W_WDATA, W_LOCKOUT };
static enum wstate ws = W_LOCKOUT;   /* needs a line reset first */
static uint32_t ones_run;
static uint32_t req_bits, req_n;
static uint64_t wdata;
static uint32_t wdata_n;
static uint32_t pend_a32;

/* bits the target drives, consumed by probe_read_bits / probe_hiz_clocks */
static uint8_t outq[128];
static uint32_t outq_head, outq_len;

static void outq_push(uint32_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        outq[(outq_head + outq_len) % sizeof outq] = (v >> i) & 1u;
        outq_len++;
    }
}

static uint32_t outq_pop(void)
{
    if (!outq_len)
        return 1;                       /* line pulled up, nobody driving */
    uint32_t b = outq[outq_head];
    outq_head = (outq_head + 1) % sizeof outq;
    outq_len--;
    return b;
}

static void handle_request(uint32_t r)
{
    /* r: bit0 start, 1 APnDP, 2 RnW, 3 A2, 4 A3, 5 parity, 6 stop, 7 park */
    bool start = r & 1, apndp = (r >> 1) & 1, rnw = (r >> 2) & 1;
    uint32_t a32 = ((r >> 3) & 3u) << 2;
    uint32_t par = (r >> 5) & 1, stop = (r >> 6) & 1, park = (r >> 7) & 1;
    uint32_t calc = (apndp + rnw + ((a32 >> 2) & 1) + ((a32 >> 3) & 1)) & 1u;
    if (!start || stop || !park || par != calc) {
        sim_stats.protocol_errors++;
        ws = W_LOCKOUT;
        return;
    }
    outq_len = 0;
    if (rnw) {
        uint32_t v = 0;
        uint32_t ack = swd_transaction(apndp, true, a32, &v);
        outq_push(1, 1);                /* turnaround */
        outq_push(ack, 3);
        if (ack == 1) {
            outq_push(v, 32);
            outq_push(__builtin_popcount(v) & 1u, 1);
        }
        ws = W_IDLE;
    } else {
        /* ACK for a write is decided before the data phase */
        uint32_t ack;
        if (apndp && sticky_err) ack = 4;
        else if (apndp && !(dp_ctrlstat & (1u << 28))) { ack = 4; sticky_err = true; }
        else ack = 1;
        if (ack != 1) sim_stats.faults++;
        outq_push(1, 1);
        outq_push(ack, 3);
        if (ack == 1) {
            pend_a32 = a32 | (apndp ? 0x100u : 0u);
            wdata = 0;
            wdata_n = 0;
            ws = W_WDATA;
        } else {
            ws = W_IDLE;
        }
    }
}

static void wire_bit_in(uint32_t b)
{
    ones_run = b ? ones_run + 1 : 0;
    if (ones_run >= 50) {               /* line reset */
        if (ws != W_IDLE || ones_run == 50)
            sim_stats.line_resets += (ones_run == 50);
        ws = W_IDLE;
        req_n = 0;
        outq_len = 0;
        return;
    }
    switch (ws) {
    case W_LOCKOUT:
        return;
    case W_IDLE:
        if (b) {
            req_bits = 1;
            req_n = 1;
            ws = W_REQ;
        }
        return;
    case W_REQ:
        req_bits |= b << req_n;
        if (++req_n == 8)
            handle_request(req_bits);
        return;
    case W_WDATA:
        wdata |= (uint64_t)b << wdata_n;
        if (++wdata_n == 33) {
            uint32_t v = (uint32_t)wdata;
            uint32_t p = (uint32_t)(wdata >> 32) & 1u;
            if ((__builtin_popcount(v) & 1u) != p) {
                sim_stats.parity_errors++;
                ws = W_IDLE;            /* DP ignores the write */
                return;
            }
            swd_transaction((pend_a32 & 0x100u) != 0, false, pend_a32 & 0xCu, &v);
            ws = W_IDLE;
        }
        return;
    }
}

/* ---------------- probe.h API used by sw_dp_pio.c / DAP_config.h ---------------- */

void probe_set_swclk_freq(uint freq_khz) { sim_stats.last_khz = freq_khz; }

void probe_write_bits(uint bit_count, uint32_t data_byte)
{
    /* 256 zero bits for idle cycles are passed with data 0 */
    for (uint i = 0; i < bit_count; i++)
        wire_bit_in(i < 32 ? (data_byte >> i) & 1u : 0u);
    sim_stats.bits_out += bit_count;
}

uint32_t probe_read_bits(uint bit_count)
{
    uint32_t v = 0;
    for (uint i = 0; i < bit_count; i++) {
        uint32_t b = outq_pop();
        if (i < 32) v |= b << i;
    }
    /* reading clocks the line with SWDIO released: pull-up = ones for the
     * line-reset detector, but a driven target bit does not count. */
    ones_run = 0;
    sim_stats.bits_in += bit_count;
    return v;
}

void probe_hiz_clocks(uint bit_count)
{
    for (uint i = 0; i < bit_count; i++)
        outq_pop();
}

void probe_read_mode(void) {}
void probe_write_mode(void) {}
void probe_init(void) { sim_stats.probe_inits++; }
void probe_deinit(void) { sim_stats.probe_deinits++; }

static bool nreset_asserted;
void probe_assert_reset(bool state)
{
    /* debugprobe: probe_assert_reset(1) de-asserts (sets the pin high) */
    bool assert_now = !state;
    if (nreset_asserted && !assert_now)
        core_reset();
    nreset_asserted = assert_now;
}
int probe_reset_level(void) { return nreset_asserted ? 0 : 1; }

uint32_t time_us_32(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
}

/* ---------------- control ---------------- */

void sim_init(bool protected_device)
{
    memset(flash, 0xFF, sizeof flash);
    memset(uicr, 0xFF, sizeof uicr);
    memset(ram, 0, sizeof ram);
    approtect = protected_device;
    /* some recognisable pre-existing firmware so a mass erase is visible */
    for (uint32_t i = 0; i < 0x2000; i += 4) {
        uint32_t w = 0xA5A50000u | i;
        memcpy(&flash[i], &w, 4);
    }
    uint32_t uicr_bl = 0x000F4000u;      /* UICR.NRFFW[0] bootloader addr */
    memcpy(&uicr[0x014], &uicr_bl, 4);
}

int sim_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(flash, 1, sizeof flash, f);
    fwrite(uicr, 1, sizeof uicr, f);
    fclose(f);
    return 0;
}

void sim_print_stats(FILE *f)
{
    fprintf(f,
            "sim: transactions=%lu mem_reads=%lu mem_writes=%lu nvm_writes=%lu rejected=%lu "
            "page_erases=%lu ctrlap_eraseall=%lu nvmc_eraseall=%lu faults=%lu protocol_errors=%lu "
            "parity_errors=%lu line_resets=%lu resets=%lu protected_accesses=%lu last_khz=%u\n",
            sim_stats.transactions, sim_stats.mem_reads, sim_stats.mem_writes, sim_stats.nvm_writes,
            sim_stats.nvm_writes_rejected, sim_stats.page_erases, sim_stats.mass_erases,
            sim_stats.nvmc_eraseall, sim_stats.faults, sim_stats.protocol_errors,
            sim_stats.parity_errors, sim_stats.line_resets, sim_stats.resets,
            sim_stats.protected_accesses, sim_stats.last_khz);
}
