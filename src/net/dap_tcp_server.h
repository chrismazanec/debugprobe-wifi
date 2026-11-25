/*
 * Single-client CMSIS-DAP TCP server. Written against the BSD socket API so
 * the same file runs on lwIP (firmware) and on POSIX (host tests, -DDAP_TCP_HOST).
 */
#ifndef DAP_TCP_SERVER_H_
#define DAP_TCP_SERVER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t port;
    /* Peer filter; ipv4 is in host byte order (a.b.c.d == a<<24|...). NULL = allow all. */
    bool (*allow)(uint32_t ipv4, void *ctx);
    /* Run one CMSIS-DAP command. req is zero padded to DAP_TCP_BUF_SIZE,
     * resp has DAP_TCP_BUF_SIZE bytes. Returns the response length. */
    size_t (*execute)(const uint8_t *req, size_t len, uint8_t *resp, void *ctx);
    /* Called on connect (true) / disconnect (false). May be NULL. */
    void (*on_client)(bool connected, uint32_t ipv4, void *ctx);
    /* Polled between connections and on receive timeouts; returning false
     * closes the client and the listening socket and returns. NULL = forever. */
    bool (*keep_running)(void *ctx);
    void *ctx;
    /* Close a client that sends nothing for this long (0 = never). OpenOCD
     * polls the target continuously, so an idle client is a dead client. */
    uint32_t idle_timeout_ms;
} dap_tcp_server_cfg_t;

/* Returns 0 when keep_running() asked to stop, negative on socket setup error. */
int dap_tcp_server_run(const dap_tcp_server_cfg_t *cfg);

#endif
