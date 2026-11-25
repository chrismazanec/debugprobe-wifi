/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/* Crash / hang diagnostics that survive a watchdog reboot (watchdog scratch
 * registers 0..3; the SDK and bootrom use 4..7). */
#ifndef DIAG_H_
#define DIAG_H_
#include <stdbool.h>
#include <stdint.h>

enum diag_reason {
    DIAG_NONE = 0,        /* power-on, RUN pin, UF2 load */
    DIAG_STACK = 1,       /* FreeRTOS stack overflow (info = task name) */
    DIAG_PANIC = 2,       /* panic() / exit (info = caller) */
    DIAG_HARDFAULT = 3,   /* HardFault (info = PC, info2 = LR) */
    DIAG_HANG = 4,        /* watchdog expired (8 s without feed) */
    DIAG_USER = 5,        /* console "reboot" */
};

enum diag_stage {
    STAGE_MAIN = 1, STAGE_SCHED = 2, STAGE_CYW43_INIT = 3, STAGE_CYW43_OK = 4,
    STAGE_JOINING = 5, STAGE_JOINED = 6, STAGE_FLASH_WRITE = 7, STAGE_HEALTHY = 8,
};

#define DIAG_MAGIC        0xD1A60000u
#define DIAG_STAGE_MAGIC  0xB0070000u
#define DIAG_SAFE_MODE_CRASHES 2   /* consecutive crashes -> boot without WiFi */

typedef struct {
    uint32_t s0, s1, s2, s3;
    bool wd_reboot;      /* watchdog_caused_reboot() */
    bool wd_timeout;     /* watchdog_enable_caused_reboot(): the timer really expired */
} diag_regs_t;

typedef struct {
    int reason;
    uint32_t info, info2;
    int last_stage;      /* stage reached by the previous boot (0 = unknown) */
    int crash_count;     /* consecutive crash boots including this reason */
    bool safe_mode;
} diag_boot_t;

void diag_decode(const diag_regs_t *r, diag_boot_t *out);
const char *diag_reason_str(int reason);
const char *diag_stage_str(int stage);
/* "STACK in task 'UART'" etc. into out (>= 64 bytes) */
void diag_describe(const diag_boot_t *b, char *out, unsigned n);

/* target only (diag_rp.c) */
extern diag_boot_t diag_boot_info;
void diag_boot(void);                    /* first thing in main() */
void diag_stage(int stage);
void diag_fault(int reason, uint32_t info, uint32_t info2) __attribute__((noreturn));
void diag_start_watchdog_task(void);
#endif
