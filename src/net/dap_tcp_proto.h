/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/*
 * CMSIS-DAP over TCP framing, compatible with OpenOCD's "cmsis-dap tcp"
 * backend (src/jtag/drivers/cmsis_dap_tcp.c, OpenOCD master since 2025-09):
 *
 *   offset 0  u32 signature  0x00504144 ("DAP\0"), little endian
 *   offset 4  u16 length     payload length, header not included
 *   offset 6  u8  type       1 = request (host -> probe), 2 = response
 *   offset 7  u8  reserved   0
 *   offset 8  payload        one CMSIS-DAP command / response
 *
 * Pure C, no platform dependencies: compiled into the firmware and into the
 * host-side tests.
 */
#ifndef DAP_TCP_PROTO_H_
#define DAP_TCP_PROTO_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DAP_TCP_SIGNATURE      0x00504144u
#define DAP_TCP_TYPE_REQUEST   0x01u
#define DAP_TCP_TYPE_RESPONSE  0x02u
#define DAP_TCP_HDR_SIZE       8u
#define DAP_TCP_DEFAULT_PORT   4441u

/* Packet size / count reported to the TCP host via DAP_Info (0xFF / 0xFE).
 * OpenOCD's backend accepts up to 1024 bytes. */
#define DAP_TCP_PACKET_SIZE    1024u
#define DAP_TCP_PACKET_COUNT   4u

/* Request / response buffers handed to the DAP core. They are much larger
 * than DAP_TCP_PACKET_SIZE so that no command which passes
 * dap_tcp_request_ok() can make DAP.c read or write outside them, whatever
 * the content of the request (DAP.c trusts the host completely). */
#define DAP_TCP_BUF_SIZE       4096u

typedef enum {
    DAP_TCP_NEED_MORE = 0,   /* no complete packet buffered yet */
    DAP_TCP_PACKET    = 1,   /* *payload / *len valid */
    DAP_TCP_BAD_SIG   = -1,
    DAP_TCP_BAD_TYPE  = -2,
    DAP_TCP_BAD_LEN   = -3,  /* 0 or > DAP_TCP_PACKET_SIZE */
} dap_tcp_status_t;

typedef struct {
    uint8_t  buf[DAP_TCP_HDR_SIZE + DAP_TCP_PACKET_SIZE];
    size_t   len;        /* bytes buffered */
    size_t   consumed;   /* bytes of the packet returned last time */
} dap_tcp_rx_t;

void dap_tcp_rx_init(dap_tcp_rx_t *rx);
/* Free space for the next recv() and where to put it. */
uint8_t *dap_tcp_rx_space(dap_tcp_rx_t *rx, size_t *space);
/* Account for n bytes received into dap_tcp_rx_space(). */
void dap_tcp_rx_commit(dap_tcp_rx_t *rx, size_t n);
/* Extract the next complete request. The payload pointer stays valid until
 * the next call of any dap_tcp_rx_* function. */
dap_tcp_status_t dap_tcp_rx_next(dap_tcp_rx_t *rx, const uint8_t **payload, size_t *len);

/* Write an 8-byte response header for a payload of len bytes. */
void dap_tcp_write_resp_hdr(uint8_t *hdr, uint16_t len);

/* Command filter for requests arriving over the network: only the commands
 * that OpenOCD / pyOCD use for SWD are allowed, with bounds that keep DAP.c
 * inside DAP_TCP_BUF_SIZE. Returns false for anything else; the caller then
 * answers with ID_DAP_Invalid (0xFF) without touching the DAP core. */
bool dap_tcp_request_ok(const uint8_t *req, size_t len);

/* Answer DAP_Info packet size / count locally (the DAP core reports the USB
 * values). Returns the response length, or 0 if req is not such a query. */
size_t dap_tcp_info_override(const uint8_t *req, size_t len, uint8_t *resp);

#endif
