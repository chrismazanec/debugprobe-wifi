/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {
    unsigned long transactions, mem_reads, mem_writes, nvm_writes, nvm_writes_rejected;
    unsigned long page_erases, mass_erases, nvmc_eraseall, faults, protocol_errors;
    unsigned long parity_errors, line_resets, resets, protected_accesses;
    unsigned long bits_out, bits_in, probe_inits, probe_deinits;
    unsigned int last_khz;
} sim_stats_t;
extern sim_stats_t sim_stats;
void sim_init(bool protected_device);
int  sim_dump(const char *path);
void sim_print_stats(FILE *f);
