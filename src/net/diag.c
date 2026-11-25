#include <stdio.h>
#include <string.h>
#include "diag.h"

void diag_decode(const diag_regs_t *r, diag_boot_t *o)
{
    memset(o, 0, sizeof *o);
    bool stage_ok = (r->s2 & 0xFFFF0000u) == DIAG_STAGE_MAGIC;
    int prev_count = stage_ok ? (int)((r->s2 >> 8) & 0xFFu) : 0;
    o->last_stage = stage_ok ? (int)(r->s2 & 0xFFu) : 0;
    if (r->wd_timeout) {
        o->reason = DIAG_HANG;
    } else if (r->wd_reboot && (r->s0 & 0xFFFF0000u) == DIAG_MAGIC && (r->s0 & 0xFFu) <= DIAG_USER) {
        o->reason = (int)(r->s0 & 0xFFu);
        o->info = r->s1;
        o->info2 = r->s3;
    } else {
        o->reason = DIAG_NONE;
        o->last_stage = 0;
        prev_count = 0;
    }
    if (o->reason >= DIAG_STACK && o->reason <= DIAG_HANG)
        o->crash_count = prev_count < 255 ? prev_count + 1 : 255;
    else
        o->crash_count = 0;
    o->safe_mode = o->crash_count >= DIAG_SAFE_MODE_CRASHES;
}

const char *diag_reason_str(int r)
{
    switch (r) {
    case DIAG_NONE: return "power-on / reset / UF2 load";
    case DIAG_STACK: return "CRASH: stack overflow";
    case DIAG_PANIC: return "CRASH: panic";
    case DIAG_HARDFAULT: return "CRASH: hard fault";
    case DIAG_HANG: return "CRASH: watchdog (hang)";
    case DIAG_USER: return "reboot command";
    }
    return "?";
}

const char *diag_stage_str(int s)
{
    switch (s) {
    case STAGE_MAIN: return "early init";
    case STAGE_SCHED: return "scheduler started";
    case STAGE_CYW43_INIT: return "CYW43 init";
    case STAGE_CYW43_OK: return "CYW43 ready";
    case STAGE_JOINING: return "joining WiFi";
    case STAGE_JOINED: return "joined WiFi";
    case STAGE_FLASH_WRITE: return "writing settings to flash";
    case STAGE_HEALTHY: return "running > 60 s";
    }
    return "unknown";
}

void diag_describe(const diag_boot_t *b, char *out, unsigned n)
{
    if (b->reason == DIAG_STACK) {
        char name[5];
        for (int i = 0; i < 4; i++) {
            char c = (char)((b->info >> (8 * i)) & 0xFF);
            name[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        name[4] = 0;
        snprintf(out, n, "%s in task '%s', previous boot reached '%s'", diag_reason_str(b->reason), name,
                 diag_stage_str(b->last_stage));
    } else if (b->reason == DIAG_HARDFAULT || b->reason == DIAG_PANIC) {
        snprintf(out, n, "%s at pc 0x%08lx lr 0x%08lx, previous boot reached '%s'", diag_reason_str(b->reason),
                 (unsigned long)b->info, (unsigned long)b->info2, diag_stage_str(b->last_stage));
    } else if (b->reason == DIAG_HANG) {
        snprintf(out, n, "%s, previous boot reached '%s'", diag_reason_str(b->reason), diag_stage_str(b->last_stage));
    } else {
        snprintf(out, n, "%s", diag_reason_str(b->reason));
    }
}
