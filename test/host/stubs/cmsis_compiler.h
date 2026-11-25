/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
#pragma once
#define __STATIC_INLINE static inline
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))
#define __WEAK __attribute__((weak))
#define __NO_RETURN __attribute__((noreturn))
#define __USED __attribute__((used))
#define __PACKED __attribute__((packed))
#define __ASM __asm__
#define __NOP() __asm__ volatile ("nop")
