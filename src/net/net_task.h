#ifndef NET_TASK_H_
#define NET_TASK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lock shared by the USB and the TCP DAP paths: one command at a time. */
void dap_lock_init(void);
void dap_lock(void);
void dap_unlock(void);

/* Load config, start WiFi + DAP server tasks. Call before the scheduler. */
void net_init(void);

/* Config console glue for cdc_uart.c (USB CDC at NETCFG_CONSOLE_BAUD). */
void netcon_enter(void);
void netcon_exit(void);
bool netcon_active(void);
void netcon_poll(void);   /* called from the CDC thread instead of the UART bridge */

#endif
