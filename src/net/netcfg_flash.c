/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/* Flash persistence for netcfg_t: last 4 KiB sector of the Pico's flash. */
#include <string.h>
#include "pico/flash.h"
#include "hardware/flash.h"
#include "netcfg.h"

#ifndef PICO_FLASH_SIZE_BYTES
#error PICO_FLASH_SIZE_BYTES must be defined by the board header
#endif

#define NETCFG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

_Static_assert(sizeof(netcfg_t) <= FLASH_PAGE_SIZE, "netcfg_t must fit one flash page");

static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static void do_erase(void *unused)
{
    (void)unused;
    flash_range_erase(NETCFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
}

static void do_store(void *unused)
{
    (void)unused;
    flash_range_erase(NETCFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(NETCFG_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
}

bool netcfg_load(netcfg_t *c)
{
    const netcfg_t *f = (const netcfg_t *)(XIP_BASE + NETCFG_FLASH_OFFSET);
    memcpy(c, f, sizeof(*c));
    if (netcfg_valid(c))
        return true;
    netcfg_defaults(c);
    return false;
}

int netcfg_store(const netcfg_t *c)
{
    memset(page, 0xFF, sizeof page);
    memcpy(page, c, sizeof(*c));
    int rc = flash_safe_execute(do_store, NULL, 2000);
    memset(page, 0xFF, sizeof page);   /* no password copy left in RAM */
    if (rc != PICO_OK)
        return rc;
    netcfg_t check;
    netcfg_load(&check);
    rc = memcmp(&check, c, sizeof check) == 0 ? 0 : -1;
    memset(&check, 0, sizeof check);
    return rc;
}

int netcfg_erase(void)
{
    return flash_safe_execute(do_erase, NULL, 2000) == PICO_OK ? 0 : -1;
}
