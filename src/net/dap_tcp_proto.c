#include <string.h>
#include "dap_tcp_proto.h"

/* CMSIS-DAP command ids (subset, mirrors DAP.h without depending on it) */
#define ID_INFO               0x00u
#define ID_HOST_STATUS        0x01u
#define ID_CONNECT            0x02u
#define ID_DISCONNECT         0x03u
#define ID_TRANSFER_CONFIGURE 0x04u
#define ID_TRANSFER           0x05u
#define ID_TRANSFER_BLOCK     0x06u
#define ID_TRANSFER_ABORT     0x07u
#define ID_WRITE_ABORT        0x08u
#define ID_DELAY              0x09u
#define ID_RESET_TARGET       0x0Au
#define ID_SWJ_PINS           0x10u
#define ID_SWJ_CLOCK          0x11u
#define ID_SWJ_SEQUENCE       0x12u
#define ID_SWD_CONFIGURE      0x13u
#define ID_SWD_SEQUENCE       0x1Du

#define INFO_PACKET_COUNT     0xFEu
#define INFO_PACKET_SIZE      0xFFu

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void dap_tcp_rx_init(dap_tcp_rx_t *rx)
{
    rx->len = 0;
    rx->consumed = 0;
}

static void drop_consumed(dap_tcp_rx_t *rx)
{
    if (rx->consumed) {
        size_t rest = rx->len - rx->consumed;
        if (rest)
            memmove(rx->buf, rx->buf + rx->consumed, rest);
        rx->len = rest;
        rx->consumed = 0;
    }
}

uint8_t *dap_tcp_rx_space(dap_tcp_rx_t *rx, size_t *space)
{
    drop_consumed(rx);
    *space = sizeof(rx->buf) - rx->len;
    return rx->buf + rx->len;
}

void dap_tcp_rx_commit(dap_tcp_rx_t *rx, size_t n)
{
    rx->len += n;
    if (rx->len > sizeof(rx->buf))
        rx->len = sizeof(rx->buf); /* cannot happen with dap_tcp_rx_space() */
}

dap_tcp_status_t dap_tcp_rx_next(dap_tcp_rx_t *rx, const uint8_t **payload, size_t *len)
{
    drop_consumed(rx);
    if (rx->len < DAP_TCP_HDR_SIZE)
        return DAP_TCP_NEED_MORE;
    if (rd_le32(rx->buf) != DAP_TCP_SIGNATURE)
        return DAP_TCP_BAD_SIG;
    if (rx->buf[6] != DAP_TCP_TYPE_REQUEST)
        return DAP_TCP_BAD_TYPE;
    size_t plen = (size_t)rx->buf[4] | ((size_t)rx->buf[5] << 8);
    if (plen == 0 || plen > DAP_TCP_PACKET_SIZE)
        return DAP_TCP_BAD_LEN;
    if (rx->len < DAP_TCP_HDR_SIZE + plen)
        return DAP_TCP_NEED_MORE;
    *payload = rx->buf + DAP_TCP_HDR_SIZE;
    *len = plen;
    rx->consumed = DAP_TCP_HDR_SIZE + plen;
    return DAP_TCP_PACKET;
}

void dap_tcp_write_resp_hdr(uint8_t *hdr, uint16_t len)
{
    hdr[0] = 0x44; hdr[1] = 0x41; hdr[2] = 0x50; hdr[3] = 0x00;
    hdr[4] = (uint8_t)(len & 0xFF);
    hdr[5] = (uint8_t)(len >> 8);
    hdr[6] = DAP_TCP_TYPE_RESPONSE;
    hdr[7] = 0;
}

bool dap_tcp_request_ok(const uint8_t *req, size_t len)
{
    if (len == 0 || len > DAP_TCP_PACKET_SIZE)
        return false;
    switch (req[0]) {
    case ID_INFO:
    case ID_HOST_STATUS:
    case ID_CONNECT:
    case ID_DISCONNECT:
    case ID_TRANSFER_CONFIGURE:
    case ID_TRANSFER_ABORT:
    case ID_WRITE_ABORT:
    case ID_DELAY:
    case ID_RESET_TARGET:
    case ID_SWJ_PINS:
    case ID_SWJ_CLOCK:
    case ID_SWJ_SEQUENCE:   /* <= 256 bits: 33 request bytes, 1 response byte */
    case ID_SWD_CONFIGURE:
        return true;
    case ID_TRANSFER:
        /* count is a byte: worst case 3 + 255 * 5 (+ match mask) request bytes,
         * 3 + 255 * 8 response bytes, both < DAP_TCP_BUF_SIZE. */
        return len >= 3;
    case ID_SWD_SEQUENCE:
        /* <= 255 sequences of <= 8 bytes each way: < DAP_TCP_BUF_SIZE. */
        return len >= 2;
    case ID_TRANSFER_BLOCK: {
        if (len < 5)
            return false;
        uint32_t count = (uint32_t)req[2] | ((uint32_t)req[3] << 8);
        /* DAP.c trusts count: response 4 + 4 * count, request (write)
         * 5 + 4 * count bytes must stay inside DAP_TCP_BUF_SIZE. A host that
         * honours DAP_TCP_PACKET_SIZE uses at most 255. */
        return count <= (DAP_TCP_BUF_SIZE - 8u) / 4u;
    }
    default:
        /* JTAG (not built), SWO (not built), UART, Queue/ExecuteCommands
         * (unbounded nesting), vendor commands: refused over the network. */
        return false;
    }
}

size_t dap_tcp_info_override(const uint8_t *req, size_t len, uint8_t *resp)
{
    if (len < 2 || req[0] != ID_INFO)
        return 0;
    if (req[1] == INFO_PACKET_SIZE) {
        resp[0] = ID_INFO;
        resp[1] = 2;
        resp[2] = (uint8_t)(DAP_TCP_PACKET_SIZE & 0xFF);
        resp[3] = (uint8_t)(DAP_TCP_PACKET_SIZE >> 8);
        return 4;
    }
    if (req[1] == INFO_PACKET_COUNT) {
        resp[0] = ID_INFO;
        resp[1] = 1;
        resp[2] = DAP_TCP_PACKET_COUNT;
        return 3;
    }
    return 0;
}
