/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/*
 * Pico 2 W (RP2350 + CYW43439) as a debug probe with a WiFi CMSIS-DAP
 * transport (OpenOCD "cmsis-dap tcp" protocol, default port 4441).
 *
 * Pin-out is identical to the stock "debugprobe_on_pico2" build:
 *   GP2 SWCLK, GP3 SWDIO, GP1 target reset, GP4 UART TX, GP5 UART RX.
 * GP23/24/25/29 belong to the CYW43439 and must not be used. The board LED is
 * driven by the CYW43 (WL_GPIO0) and shows the WiFi state (see src/net/).
 */
#ifndef BOARD_PICO2_W_CONFIG_H_
#define BOARD_PICO2_W_CONFIG_H_

#define PROBE_IO_RAW
#define PROBE_CDC_UART

// PIO config
#define PROBE_SM 0
#define PROBE_PIN_OFFSET 2
#define PROBE_PIN_SWCLK (PROBE_PIN_OFFSET + 0) // 2
#define PROBE_PIN_SWDIO (PROBE_PIN_OFFSET + 1) // 3
// Target reset config
#define PROBE_PIN_RESET 1

// UART config
#define PROBE_UART_TX 4
#define PROBE_UART_RX 5
#define PROBE_UART_INTERFACE uart1
#define PROBE_UART_BAUDRATE 115200

// No PROBE_USB_CONNECTED_LED: GPIO25 is the CYW43 chip select on the W boards.

// Network transport
#define PROBE_NET_WIFI 1

/* USB string descriptors are capped at 31 characters (usb_descriptors.c);
 * hosts look for "CMSIS-DAP" in the product string. 27 characters: */
#define PROBE_PRODUCT_STRING "Debugprobe WiFi (CMSIS-DAP)"

#endif
