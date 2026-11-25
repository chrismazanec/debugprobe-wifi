/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "plog.h"

#ifdef DAP_TCP_HOST
#include <time.h>
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#define LOCK()
#define UNLOCK()
#else
#include "pico/time.h"
#include "pico/critical_section.h"
static critical_section_t cs;
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
#define LOCK() critical_section_enter_blocking(&cs)
#define UNLOCK() critical_section_exit(&cs)
#endif

static char ring[PLOG_SIZE];
static size_t head;      /* next write position */
static size_t used;

void plog_init(void)
{
#ifndef DAP_TCP_HOST
    critical_section_init(&cs);
#endif
    head = used = 0;
}

void plog(const char *fmt, ...)
{
    char line[160];
    uint32_t t = now_ms();
    int n = snprintf(line, sizeof line, "[%5lu.%03lu] ", (unsigned long)(t / 1000), (unsigned long)(t % 1000));
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof line - 3) len = sizeof line - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    LOCK();
    for (size_t i = 0; i < len; i++) {
        ring[head] = line[i];
        head = (head + 1) % PLOG_SIZE;
    }
    used = used + len > PLOG_SIZE ? PLOG_SIZE : used + len;
    UNLOCK();
}

size_t plog_read(char *out, size_t n)
{
    LOCK();
    size_t cnt = used < n ? used : n;
    size_t start = (head + PLOG_SIZE - used) % PLOG_SIZE;
    /* when truncating, keep the newest part */
    start = (start + (used - cnt)) % PLOG_SIZE;
    for (size_t i = 0; i < cnt; i++)
        out[i] = ring[(start + i) % PLOG_SIZE];
    UNLOCK();
    return cnt;
}
