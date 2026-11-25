/* RP2350 side of diag.h: scratch registers, fault handlers, watchdog task. */
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "tusb.h"
#include "diag.h"
#include "plog.h"

diag_boot_t diag_boot_info;
static uint32_t crash_count_now;

static void write_stage(int stage)
{
    watchdog_hw->scratch[2] = DIAG_STAGE_MAGIC | ((crash_count_now & 0xFFu) << 8) | ((uint32_t)stage & 0xFFu);
}

void diag_boot(void)
{
    diag_regs_t r = {
        .s0 = watchdog_hw->scratch[0], .s1 = watchdog_hw->scratch[1],
        .s2 = watchdog_hw->scratch[2], .s3 = watchdog_hw->scratch[3],
        .wd_reboot = watchdog_caused_reboot(),
        .wd_timeout = watchdog_enable_caused_reboot(),
    };
    diag_decode(&r, &diag_boot_info);
    crash_count_now = (uint32_t)diag_boot_info.crash_count;
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[3] = 0;
    write_stage(STAGE_MAIN);
}

void diag_stage(int stage)
{
    write_stage(stage);
}

void __attribute__((noreturn)) diag_fault(int reason, uint32_t info, uint32_t info2)
{
    watchdog_hw->scratch[0] = DIAG_MAGIC | ((uint32_t)reason & 0xFFu);
    watchdog_hw->scratch[1] = info;
    watchdog_hw->scratch[3] = info2;
    watchdog_reboot(0, 0, 10);
    for (;;)
        __asm volatile("nop");
}

/* HardFault: record the stacked PC / LR, then reboot. */
void __attribute__((used)) diag_hardfault_c(uint32_t *frame)
{
    diag_fault(DIAG_HARDFAULT, frame[6], frame[5]);
}

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b diag_hardfault_c \n");
}

/* panic() ends in _exit(); newlib's _exit is weak. */
void __attribute__((noreturn)) _exit(int status)
{
    diag_fault(DIAG_PANIC, (uint32_t)__builtin_return_address(0), (uint32_t)status);
}

static void wdt_thread(void *p)
{
    (void)p;
    watchdog_enable(8000, true);      /* pause while a debugger halts the Pico */
    bool healthy = false;
    bool mounted = false;
    for (;;) {
        watchdog_update();
        bool m = tud_mounted();
        if (m != mounted) {
            mounted = m;
            plog("usb: %s", m ? "configured by host" : "not configured");
        }
        if (!healthy && to_ms_since_boot(get_absolute_time()) > 60000) {
            healthy = true;
            crash_count_now = 0;      /* survived a minute: next crash starts counting again */
            write_stage(STAGE_HEALTHY);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void diag_start_watchdog_task(void)
{
    xTaskCreate(wdt_thread, "WDT", 1024, NULL, tskIDLE_PRIORITY + 1, NULL);
}
