/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/* host stub */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef unsigned int uint;
uint32_t time_us_32(void);
static inline void gpio_put(uint pin, bool v) { (void)pin; (void)v; }
static inline void gpio_init(uint pin) { (void)pin; }
static inline void gpio_set_dir(uint pin, bool out) { (void)pin; (void)out; }
#define GPIO_OUT 1
#define RASPBERRYPI_PICO2_W 1
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
