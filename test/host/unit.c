/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/* Unit tests for the platform independent parts of debugprobe-wifi. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dap_tcp_proto.h"
#include "netcfg.h"
#include "netcfg_console.h"
#include "plog.h"
#include "diag.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static size_t mkreq(uint8_t *out, const uint8_t *p, size_t n, uint32_t sig, uint8_t type)
{
    out[0] = sig & 255; out[1] = (sig >> 8) & 255; out[2] = (sig >> 16) & 255; out[3] = sig >> 24;
    out[4] = n & 255; out[5] = n >> 8; out[6] = type; out[7] = 0;
    memcpy(out + 8, p, n);
    return n + 8;
}

static void feed(dap_tcp_rx_t *rx, const uint8_t *d, size_t n)
{
    size_t sp;
    uint8_t *dst = dap_tcp_rx_space(rx, &sp);
    CHECK(sp >= n);
    memcpy(dst, d, n);
    dap_tcp_rx_commit(rx, n);
}

static void test_proto(void)
{
    dap_tcp_rx_t rx;
    uint8_t s[4096];
    const uint8_t *pl; size_t len;
    const uint8_t info[] = { 0x00, 0xFF };
    const uint8_t xfer[] = { 0x05, 0x00, 0x01, 0x02 };

    /* byte-by-byte delivery of two back-to-back packets */
    size_t n1 = mkreq(s, info, sizeof info, DAP_TCP_SIGNATURE, 1);
    size_t n2 = mkreq(s + n1, xfer, sizeof xfer, DAP_TCP_SIGNATURE, 1);
    dap_tcp_rx_init(&rx);
    int got = 0;
    for (size_t i = 0; i < n1 + n2; i++) {
        feed(&rx, s + i, 1);
        dap_tcp_status_t st;
        while ((st = dap_tcp_rx_next(&rx, &pl, &len)) == DAP_TCP_PACKET) {
            if (got == 0) CHECK(len == 2 && pl[0] == 0 && pl[1] == 0xFF && i == n1 - 1);
            if (got == 1) CHECK(len == 4 && pl[0] == 5 && pl[3] == 2 && i == n1 + n2 - 1);
            got++;
        }
        CHECK(st == DAP_TCP_NEED_MORE);
    }
    CHECK(got == 2);

    /* both packets in one recv */
    dap_tcp_rx_init(&rx);
    feed(&rx, s, n1 + n2);
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_PACKET && len == 2);
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_PACKET && len == 4);
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_NEED_MORE);

    /* errors */
    dap_tcp_rx_init(&rx);
    n1 = mkreq(s, info, 2, 0x12345678, 1);
    feed(&rx, s, n1);
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_BAD_SIG);
    dap_tcp_rx_init(&rx);
    n1 = mkreq(s, info, 2, DAP_TCP_SIGNATURE, 2);       /* response type from host */
    feed(&rx, s, n1);
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_BAD_TYPE);
    dap_tcp_rx_init(&rx);
    n1 = mkreq(s, info, 0, DAP_TCP_SIGNATURE, 1);
    feed(&rx, s, n1);
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_BAD_LEN);
    dap_tcp_rx_init(&rx);
    uint8_t big[DAP_TCP_PACKET_SIZE + 1] = { 0 };
    n1 = mkreq(s, big, sizeof big, DAP_TCP_SIGNATURE, 1);
    feed(&rx, s, 8);                                     /* header alone is enough to reject */
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_BAD_LEN);

    /* largest legal packet fits the receive buffer */
    dap_tcp_rx_init(&rx);
    uint8_t maxp[DAP_TCP_PACKET_SIZE]; memset(maxp, 0x06, sizeof maxp);
    n1 = mkreq(s, maxp, sizeof maxp, DAP_TCP_SIGNATURE, 1);
    feed(&rx, s, n1);
    CHECK(dap_tcp_rx_next(&rx, &pl, &len) == DAP_TCP_PACKET && len == DAP_TCP_PACKET_SIZE);

    /* response header */
    uint8_t h[8];
    dap_tcp_write_resp_hdr(h, 0x1234);
    CHECK(h[0] == 'D' && h[1] == 'A' && h[2] == 'P' && h[3] == 0 && h[4] == 0x34 && h[5] == 0x12 && h[6] == 2 && h[7] == 0);

    /* command filter */
    uint8_t r[8] = { 0 };
    r[0] = 0x00; CHECK(dap_tcp_request_ok(r, 2));
    r[0] = 0x05; CHECK(dap_tcp_request_ok(r, 3)); CHECK(!dap_tcp_request_ok(r, 2));
    r[0] = 0x06; r[2] = 0xFF; r[3] = 0x00; CHECK(dap_tcp_request_ok(r, 5));      /* 255 words: what OpenOCD uses */
    r[2] = 0xFE; r[3] = 0x03; CHECK(dap_tcp_request_ok(r, 5));                   /* 1022 */
    r[2] = 0xFF; r[3] = 0x03; CHECK(!dap_tcp_request_ok(r, 5));                  /* 1023 would overrun */
    r[2] = 0xFF; r[3] = 0xFF; CHECK(!dap_tcp_request_ok(r, 5));
    r[0] = 0x7F; CHECK(!dap_tcp_request_ok(r, 4));       /* ExecuteCommands */
    r[0] = 0x7E; CHECK(!dap_tcp_request_ok(r, 4));       /* QueueCommands */
    r[0] = 0x80; CHECK(!dap_tcp_request_ok(r, 1));       /* vendor */
    r[0] = 0x14; CHECK(!dap_tcp_request_ok(r, 4));       /* JTAG sequence */
    r[0] = 0x1C; CHECK(!dap_tcp_request_ok(r, 3));       /* SWO data */
    r[0] = 0x12; CHECK(dap_tcp_request_ok(r, 2));
    r[0] = 0x1D; CHECK(dap_tcp_request_ok(r, 2));
    CHECK(!dap_tcp_request_ok(r, 0));

    /* DAP_Info override */
    uint8_t resp[8];
    const uint8_t q1[] = { 0x00, 0xFF }, q2[] = { 0x00, 0xFE }, q3[] = { 0x00, 0x04 };
    CHECK(dap_tcp_info_override(q1, 2, resp) == 4 && resp[1] == 2 && (resp[2] | resp[3] << 8) == DAP_TCP_PACKET_SIZE);
    CHECK(dap_tcp_info_override(q2, 2, resp) == 3 && resp[1] == 1 && resp[2] == DAP_TCP_PACKET_COUNT);
    CHECK(dap_tcp_info_override(q3, 2, resp) == 0);
}

static void test_netcfg(void)
{
    uint32_t ip, net, mask;
    CHECK(netcfg_parse_ipv4("192.168.1.20", &ip) && ip == 0xC0A80114u);
    CHECK(!netcfg_parse_ipv4("192.168.1", &ip));
    CHECK(!netcfg_parse_ipv4("192.168.1.256", &ip));
    CHECK(!netcfg_parse_ipv4("192.168.1.2x", &ip));
    CHECK(!netcfg_parse_ipv4("1.2.3.4.5", &ip));
    CHECK(netcfg_parse_cidr("192.168.1.77/24", &net, &mask) && net == 0xC0A80100u && mask == 0xFFFFFF00u);
    CHECK(netcfg_parse_cidr("10.0.0.5", &net, &mask) && net == 0x0A000005u && mask == 0xFFFFFFFFu);
    CHECK(netcfg_parse_cidr("any", &net, &mask) && mask == 0);
    CHECK(netcfg_parse_cidr("0.0.0.0/0", &net, &mask) && mask == 0);
    CHECK(!netcfg_parse_cidr("10.0.0.0/33", &net, &mask));
    CHECK(!netcfg_parse_cidr("10.0.0.0/", &net, &mask));
    char b[16];
    netcfg_format_ipv4(0xC0A80114u, b);
    CHECK(!strcmp(b, "192.168.1.20"));
    CHECK(netcfg_mask_bits(0xFFFFFF00u) == 24);

    netcfg_t c;
    netcfg_defaults(&c);
    CHECK(netcfg_valid(&c));
    CHECK(!netcfg_has_wifi(&c));
    CHECK(c.port == 4441 && !strcmp(c.hostname, "dapprobe") && !strcmp(c.country, "XX"));
    CHECK(netcfg_peer_allowed(&c, 0x01020304u));
    c.allow_net = 0xC0A80100u; c.allow_mask = 0xFFFFFF00u;
    CHECK(netcfg_peer_allowed(&c, 0xC0A801FEu));
    CHECK(!netcfg_peer_allowed(&c, 0xC0A802FEu));
    CHECK(!netcfg_valid(&c));                       /* crc stale */
    netcfg_seal(&c);
    CHECK(netcfg_valid(&c));
    netcfg_t erased;
    memset(&erased, 0xFF, sizeof erased);           /* blank flash */
    CHECK(!netcfg_valid(&erased));
    c.ssid[5] ^= 1;
    CHECK(!netcfg_valid(&c));
    CHECK(netcfg_crc32("123456789", 9) == 0xCBF43926u);   /* standard CRC-32 check value */
}

/* console harness */
static char outbuf[16384];
static size_t outlen;
static netcfg_t saved;
static int saves, erases, reboots;
static void w(const char *s, size_t n, void *ctx) { (void)ctx; memcpy(outbuf + outlen, s, n); outlen += n; outbuf[outlen] = 0; }
static int sv(const netcfg_t *c, void *ctx) { (void)ctx; saved = *c; saves++; return 0; }
static int er(void *ctx) { (void)ctx; erases++; return 0; }
static void rb(bool bs, void *ctx) { (void)ctx; reboots += bs ? 10 : 1; }
static int logs, quits;
static void lg(void *ctx) { (void)ctx; logs++; }
static void qt(void *ctx) { (void)ctx; quits++; }
static void st(char *o, size_t n, void *ctx) { (void)ctx; snprintf(o, n, "wifi     : test\r\n"); }

static void type(netcfg_console_t *con, const char *s)
{
    outlen = 0; outbuf[0] = 0;
    netcfg_console_input(con, (const uint8_t *)s, strlen(s));
}

static void test_console(void)
{
    netcfg_console_t con;
    netcfg_console_io_t io = { w, sv, er, rb, st, lg, qt, NULL };
    netcfg_t c;
    netcfg_defaults(&c);
    netcfg_console_init(&con, &io, &c);
    outlen = 0;
    netcfg_console_banner(&con);
    CHECK(strstr(outbuf, "config console") && strstr(outbuf, "> "));

    type(&con, "ssid My Home Net\r\n");
    CHECK(strstr(outbuf, "OK"));
    type(&con, "save\r\n");                                /* psk missing */
    CHECK(strstr(outbuf, "ERR psk"));
    type(&con, "psk\r");
    CHECK(strstr(outbuf, "password: "));
    type(&con, "s3cret-pass\r\n");                          /* CR LF after CR mode */
    CHECK(strstr(outbuf, "OK") && !strstr(outbuf, "s3cret"));   /* never echoed */
    type(&con, "show\n");
    CHECK(strstr(outbuf, "My Home Net") && strstr(outbuf, "(set, hidden)") && !strstr(outbuf, "s3cret"));
    CHECK(strstr(outbuf, "unsaved"));
    type(&con, "allow 192.168.7.10/24\r");
    CHECK(strstr(outbuf, "OK"));
    type(&con, "allow 300.1.1.1\r");
    CHECK(strstr(outbuf, "ERR"));
    type(&con, "port 4442\r");
    type(&con, "hostname Bad_Name\r");
    CHECK(strstr(outbuf, "ERR"));
    type(&con, "hostname jackprobe\r");
    type(&con, "country sk\r");
    type(&con, "save\r");
    CHECK(strstr(outbuf, "OK") && saves == 1);
    CHECK(netcfg_valid(&saved));
    CHECK(!strcmp(saved.ssid, "My Home Net") && !strcmp(saved.psk, "s3cret-pass"));
    CHECK(saved.allow_net == 0xC0A80700u && saved.allow_mask == 0xFFFFFF00u);
    CHECK(saved.port == 4442 && !strcmp(saved.hostname, "jackprobe") && !strcmp(saved.country, "SK"));
    CHECK(saved.auth == NETCFG_AUTH_WPA2 && netcfg_has_wifi(&saved));

    type(&con, "psk\r");
    type(&con, "short\r");
    CHECK(strstr(outbuf, "ERR password"));
    type(&con, "port 0\r"); CHECK(strstr(outbuf, "ERR"));
    type(&con, "port 70000\r"); CHECK(strstr(outbuf, "ERR"));
    type(&con, "wifi off\r"); CHECK(strstr(outbuf, "OK"));
    type(&con, "revert\r");
    type(&con, "show\r"); CHECK(strstr(outbuf, "wifi     : on") && !strstr(outbuf, "unsaved"));
    type(&con, "auth open\r");
    type(&con, "save\r");
    CHECK(saves == 2 && saved.auth == NETCFG_AUTH_OPEN && saved.psk[0] == 0);
    type(&con, "status\r"); CHECK(strstr(outbuf, "wifi     : test"));
    type(&con, "bogus\r"); CHECK(strstr(outbuf, "ERR unknown"));
    type(&con, "abc\x7f\x7f\x7fhelp\r"); CHECK(strstr(outbuf, "bootsel"));
    type(&con, "psk\rhalfway\x03"); CHECK(strstr(outbuf, "^C"));
    type(&con, "show\r"); CHECK(strstr(outbuf, "(empty)"));
    type(&con, "erase\r"); CHECK(erases == 1 && strstr(outbuf, "OK"));
    type(&con, "show\r"); CHECK(strstr(outbuf, "(not set)"));
    char longline[400]; memset(longline, 'a', sizeof longline - 2); longline[398] = '\r'; longline[399] = 0;
    type(&con, longline); CHECK(strstr(outbuf, "ERR"));      /* truncated to 159 chars, unknown command */
    type(&con, "log\r"); CHECK(logs == 1 && strstr(outbuf, "OK"));
    type(&con, "exit\r"); CHECK(quits == 1);
    type(&con, "reboot\r"); CHECK(reboots == 1);
    type(&con, "bootsel\r"); CHECK(reboots == 11);
}

static bool magic(const char *s)
{
    netcfg_magic_t m = { 0 };
    return netcfg_magic_feed(&m, (const uint8_t *)s, strlen(s));
}

static void test_magic_log_diag(void)
{
    CHECK(magic("+++wifiprobe\r"));
    CHECK(magic("+++wifiprobe\n"));
    CHECK(magic("garbage++++++wifiprobe\r\n"));
    CHECK(magic("+++wifi+++wifiprobe\r"));
    CHECK(!magic("+++wifiprobe"));
    CHECK(!magic("++wifiprobe\r"));
    CHECK(!magic("+++wifiprobeX\r"));
    CHECK(!magic("+++wifiprob\r"));
    /* split across calls */
    netcfg_magic_t m = { 0 };
    CHECK(!netcfg_magic_feed(&m, (const uint8_t *)"xx+++wif", 8));
    CHECK(netcfg_magic_feed(&m, (const uint8_t *)"iprobe\r", 7));

    plog_init();
    char buf[PLOG_SIZE + 1];
    CHECK(plog_read(buf, sizeof buf) == 0);
    plog("hello %d", 42);
    size_t n = plog_read(buf, sizeof buf); buf[n] = 0;
    CHECK(strstr(buf, "] hello 42\r\n") != NULL);
    for (int i = 0; i < 200; i++) plog("line %03d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
    n = plog_read(buf, sizeof buf); buf[n] = 0;
    CHECK(n == PLOG_SIZE && strstr(buf, "line 199") && !strstr(buf, "hello"));
    n = plog_read(buf, 100); buf[n] = 0;
    CHECK(n == 100 && strstr(buf, "line 199"));               /* newest part kept */
    char big[400]; memset(big, 'z', sizeof big - 1); big[399] = 0;
    plog("%s", big);                                            /* truncated, still CRLF terminated */
    n = plog_read(buf, sizeof buf); buf[n] = 0;
    CHECK(n >= 2 && buf[n - 2] == '\r' && buf[n - 1] == '\n');

    diag_regs_t r = { 0 };
    diag_boot_t b;
    diag_decode(&r, &b);                                        /* power-on */
    CHECK(b.reason == DIAG_NONE && b.crash_count == 0 && !b.safe_mode);
    r.wd_reboot = true; r.s0 = DIAG_MAGIC | DIAG_STACK; r.s1 = 0x54524155; /* "UART" */
    r.s2 = DIAG_STAGE_MAGIC | (0 << 8) | STAGE_CYW43_OK;
    diag_decode(&r, &b);
    CHECK(b.reason == DIAG_STACK && b.crash_count == 1 && !b.safe_mode && b.last_stage == STAGE_CYW43_OK);
    char d[160]; diag_describe(&b, d, sizeof d);
    CHECK(strstr(d, "stack overflow") && strstr(d, "'UART'") && strstr(d, "CYW43 ready"));
    r.s2 = DIAG_STAGE_MAGIC | (1 << 8) | STAGE_CYW43_INIT; r.s0 = 0; r.wd_timeout = true;
    diag_decode(&r, &b);
    CHECK(b.reason == DIAG_HANG && b.crash_count == 2 && b.safe_mode);
    diag_describe(&b, d, sizeof d); CHECK(strstr(d, "hang") && strstr(d, "CYW43 init"));
    r.wd_timeout = false; r.s0 = DIAG_MAGIC | DIAG_USER;
    diag_decode(&r, &b);
    CHECK(b.reason == DIAG_USER && b.crash_count == 0 && !b.safe_mode);
    r.wd_reboot = false; r.s0 = DIAG_MAGIC | DIAG_HARDFAULT;    /* stale record after power cycle */
    diag_decode(&r, &b);
    CHECK(b.reason == DIAG_NONE && b.crash_count == 0);
    r.wd_reboot = true; r.s0 = DIAG_MAGIC | 0x77;               /* garbage reason */
    diag_decode(&r, &b);
    CHECK(b.reason == DIAG_NONE);
}

int main(void)
{
    test_magic_log_diag();
    test_proto();
    test_netcfg();
    test_console();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
