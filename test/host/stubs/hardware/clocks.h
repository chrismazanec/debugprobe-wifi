/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
#pragma once
#include <stdint.h>
enum clock_index { clk_sys = 5 };
static inline uint32_t clock_get_hz(enum clock_index c) { (void)c; return 150000000u; }
