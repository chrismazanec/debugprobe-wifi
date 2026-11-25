/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
#include <string.h>
#include "dap_tcp_proto.h"
#include "dap_tcp_server.h"

#ifdef DAP_TCP_HOST
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#define S(fn) fn
#define S_CLOSE close
#else
#include "lwip/sockets.h"
#include "lwip/errno.h"
#define S(fn) lwip_##fn
#define S_CLOSE lwip_close
#endif

#define POLL_MS 500u

static uint8_t req_buf[DAP_TCP_BUF_SIZE];
static uint8_t resp_buf[DAP_TCP_HDR_SIZE + DAP_TCP_BUF_SIZE];
static dap_tcp_rx_t rx;

static bool running(const dap_tcp_server_cfg_t *cfg)
{
    return cfg->keep_running ? cfg->keep_running(cfg->ctx) : true;
}

static void set_rcv_timeout(int fd, uint32_t ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000u;
    tv.tv_usec = (ms % 1000u) * 1000u;
    S(setsockopt)(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int r = (int)S(send)(fd, p, n, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void serve_client(const dap_tcp_server_cfg_t *cfg, int fd)
{
    int one = 1;
    S(setsockopt)(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    S(setsockopt)(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    /* A vanished host (laptop asleep, WiFi gone) is detected after
     * 20 + 4 * 5 = 40 s, then the next client can connect. */
    int idle = 20, intvl = 5, cnt = 4;
    S(setsockopt)(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    S(setsockopt)(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    S(setsockopt)(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    set_rcv_timeout(fd, POLL_MS);
    dap_tcp_rx_init(&rx);
    uint32_t idle_ms = 0;

    for (;;) {
        const uint8_t *payload;
        size_t plen;
        dap_tcp_status_t st;
        while ((st = dap_tcp_rx_next(&rx, &payload, &plen)) == DAP_TCP_PACKET) {
            size_t rlen;
            memset(req_buf, 0, sizeof(req_buf));
            memcpy(req_buf, payload, plen);
            uint8_t *resp = resp_buf + DAP_TCP_HDR_SIZE;
            memset(resp, 0, DAP_TCP_BUF_SIZE);
            if (!dap_tcp_request_ok(req_buf, plen)) {
                resp[0] = 0xFF;          /* ID_DAP_Invalid */
                rlen = 1;
            } else if ((rlen = dap_tcp_info_override(req_buf, plen, resp)) == 0) {
                rlen = cfg->execute(req_buf, plen, resp, cfg->ctx);
            }
            if (rlen > DAP_TCP_PACKET_SIZE)  /* never expected; keep the host sane */
                rlen = DAP_TCP_PACKET_SIZE;
            dap_tcp_write_resp_hdr(resp_buf, (uint16_t)rlen);
            if (send_all(fd, resp_buf, DAP_TCP_HDR_SIZE + rlen) < 0)
                return;
        }
        if (st != DAP_TCP_NEED_MORE)
            return;                      /* framing error: drop the client */

        size_t space;
        uint8_t *dst = dap_tcp_rx_space(&rx, &space);
        int r = (int)S(recv)(fd, dst, space, 0);
        if (r > 0) {
            dap_tcp_rx_commit(&rx, (size_t)r);
            idle_ms = 0;
            continue;
        }
        if (r == 0)
            return;                      /* orderly close */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
#ifdef ETIMEDOUT
            || errno == ETIMEDOUT
#endif
           ) {
            if (!running(cfg))
                return;
            idle_ms += POLL_MS;
            if (cfg->idle_timeout_ms && idle_ms >= cfg->idle_timeout_ms)
                return;
            continue;
        }
        return;                          /* reset / error */
    }
}

int dap_tcp_server_run(const dap_tcp_server_cfg_t *cfg)
{
    int ls = (int)S(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0)
        return -1;
    int one = 1;
    S(setsockopt)(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (S(bind)(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || S(listen)(ls, 1) < 0) {
        S_CLOSE(ls);
        return -2;
    }
    set_rcv_timeout(ls, POLL_MS);   /* lwIP and Linux both honour SO_RCVTIMEO on accept() */

    while (running(cfg)) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = (int)S(accept)(ls, (struct sockaddr *)&peer, &plen);
        if (c < 0)
            continue;                    /* timeout or transient error */
        uint32_t ip = ntohl(peer.sin_addr.s_addr);
        if (cfg->allow && !cfg->allow(ip, cfg->ctx)) {
            S_CLOSE(c);
            continue;
        }
        if (cfg->on_client)
            cfg->on_client(true, ip, cfg->ctx);
        serve_client(cfg, c);
        S(shutdown)(c, SHUT_RDWR);
        S_CLOSE(c);
        if (cfg->on_client)
            cfg->on_client(false, ip, cfg->ctx);
    }
    S_CLOSE(ls);
    return 0;
}
