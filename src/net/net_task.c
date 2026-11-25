/*
 * WiFi side of debugprobe-wifi (Pico 2 W):
 *   - NET task: joins the stored network, keeps it joined, mDNS name, LED.
 *   - DTCP task: CMSIS-DAP TCP server (OpenOCD "cmsis-dap tcp" protocol).
 *   - config console over the USB CDC port (magic baud rate).
 *
 * LED (CYW43 WL_GPIO0):
 *   off                     no WiFi configured / wifi off
 *   slow blink (1 Hz)       joining
 *   double flash every 2 s  join failed, retrying (wrong password, no AP)
 *   on                      joined, waiting for a DAP client
 *   fast blink (5 Hz)       DAP client connected
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/pio.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"
#include "tusb.h"

#include "DAP_config.h"
#include "DAP.h"
#include "net_task.h"
#include "netcfg.h"
#include "netcfg_console.h"
#include "dap_tcp_proto.h"
#include "dap_tcp_server.h"

#define NET_TASK_PRIO   (tskIDLE_PRIORITY + 2)
#define DTCP_TASK_PRIO  (tskIDLE_PRIORITY + 1)

enum link_state { LS_OFF, LS_JOINING, LS_FAILED, LS_UP, LS_INIT_FAILED };

static SemaphoreHandle_t dap_mutex;
static SemaphoreHandle_t cfg_mutex;
static netcfg_t active_cfg;               /* guarded by cfg_mutex */
static volatile uint32_t cfg_generation;
static volatile enum link_state link_state = LS_OFF;
static volatile int last_join_status;
static volatile uint32_t ip_addr;          /* host order, 0 = none */
static volatile bool client_connected;
static volatile uint32_t client_ip;
static char hostname_buf[33];

/* ---------------- DAP lock ---------------- */

void dap_lock_init(void)
{
    dap_mutex = xSemaphoreCreateMutex();
}

void dap_lock(void)
{
    xSemaphoreTake(dap_mutex, portMAX_DELAY);
}

void dap_unlock(void)
{
    xSemaphoreGive(dap_mutex);
}

/* ---------------- config access ---------------- */

static void cfg_get(netcfg_t *out)
{
    xSemaphoreTake(cfg_mutex, portMAX_DELAY);
    *out = active_cfg;
    xSemaphoreGive(cfg_mutex);
}

static void cfg_set(const netcfg_t *in)
{
    xSemaphoreTake(cfg_mutex, portMAX_DELAY);
    active_cfg = *in;
    cfg_generation++;
    xSemaphoreGive(cfg_mutex);
}

/* ---------------- DAP TCP server ---------------- */

static size_t dtcp_execute(const uint8_t *req, size_t len, uint8_t *resp, void *ctx)
{
    (void)len; (void)ctx;
    dap_lock();
    uint32_t r = DAP_ExecuteCommand(req, resp);
    dap_unlock();
    return r & 0xFFFFu;
}

static bool dtcp_allow(uint32_t ip, void *ctx)
{
    (void)ctx;
    netcfg_t c;
    cfg_get(&c);
    bool ok = netcfg_peer_allowed(&c, ip);
    memset(c.psk, 0, sizeof c.psk);
    return ok;
}

static void dtcp_on_client(bool connected, uint32_t ip, void *ctx)
{
    (void)ctx;
    if (!connected) {
        /* Release SWD pins if the host vanished without DAP_Disconnect. */
        static const uint8_t disc[DAP_TCP_BUF_SIZE] = { ID_DAP_Disconnect };
        static uint8_t resp[DAP_TCP_BUF_SIZE];
        dap_lock();
        DAP_ExecuteCommand(disc, resp);
        dap_unlock();
    }
    client_ip = ip;
    client_connected = connected;
}

static uint32_t dtcp_generation;

static bool dtcp_keep_running(void *ctx)
{
    (void)ctx;
    return dtcp_generation == cfg_generation;
}

static void dtcp_thread(void *ptr)
{
    (void)ptr;
    for (;;) {
        netcfg_t c;
        cfg_get(&c);
        memset(c.psk, 0, sizeof c.psk);
        dtcp_generation = cfg_generation;
        if (!netcfg_has_wifi(&c) || link_state != LS_UP) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        dap_tcp_server_cfg_t sc = {
            .port = c.port,
            .allow = dtcp_allow,
            .execute = dtcp_execute,
            .on_client = dtcp_on_client,
            .keep_running = dtcp_keep_running,
            .ctx = NULL,
            .idle_timeout_ms = 0,
        };
        if (dap_tcp_server_run(&sc) < 0)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------- WiFi ---------------- */

static void led(bool on)
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

static uint32_t auth_mode(uint8_t a)
{
    switch (a) {
    case NETCFG_AUTH_WPA3: return CYW43_AUTH_WPA3_WPA2_AES_PSK;
    case NETCFG_AUTH_OPEN: return CYW43_AUTH_OPEN;
    default:               return CYW43_AUTH_WPA2_AES_PSK;
    }
}

static struct netif *sta_netif(void)
{
    return &cyw43_state.netif[CYW43_ITF_STA];
}

static bool mdns_started;
static bool mdns_on_netif;

static void mdns_up(void)
{
    cyw43_arch_lwip_begin();
    if (!mdns_started) {
        mdns_resp_init();
        mdns_started = true;
    }
    if (mdns_on_netif)
        mdns_resp_remove_netif(sta_netif());
    mdns_resp_add_netif(sta_netif(), hostname_buf);
    mdns_on_netif = true;
    cyw43_arch_lwip_end();
}

static void mdns_down(void)
{
    cyw43_arch_lwip_begin();
    if (mdns_on_netif)
        mdns_resp_remove_netif(sta_netif());
    mdns_on_netif = false;
    cyw43_arch_lwip_end();
}

static void wait_ms_blinking(uint32_t ms, int pattern, uint32_t gen)
{
    /* pattern 0: off, 1: slow blink, 2: double flash */
    for (uint32_t t = 0; t < ms && gen == cfg_generation; t += 100) {
        bool on = false;
        uint32_t ph = t % 2000;
        if (pattern == 1)
            on = (t % 1000) < 500;
        else if (pattern == 2)
            on = ph < 100 || (ph >= 200 && ph < 300);
        led(on);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void net_thread(void *ptr)
{
    (void)ptr;
    netcfg_t c;
    cfg_get(&c);
    uint32_t country = CYW43_COUNTRY(c.country[0], c.country[1], 0);

    /* The SWD state machine uses pio0 SM0 without claiming it, and autobaud
     * claims pio0 SMs at run time. Park all pio0 SMs while the CYW43 driver
     * picks its PIO, so the WiFi SPI ends up on pio1 or pio2. */
    uint parked = 0;
    for (uint sm = 0; sm < 4; sm++) {
        if (!pio_sm_is_claimed(pio0, sm)) {
            pio_sm_claim(pio0, sm);
            parked |= 1u << sm;
        }
    }
    int rc = cyw43_arch_init_with_country(country);
    for (uint sm = 0; sm < 4; sm++)
        if (parked & (1u << sm))
            pio_sm_unclaim(pio0, sm);
    if (rc) {
        link_state = LS_INIT_FAILED;
        vTaskDelete(NULL);
    }
    cyw43_arch_enable_sta_mode();

    for (;;) {
        uint32_t gen = cfg_generation;
        cfg_get(&c);
        if (!netcfg_has_wifi(&c)) {
            link_state = LS_OFF;
            ip_addr = 0;
            led(false);
            while (gen == cfg_generation)
                vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        memcpy(hostname_buf, c.hostname, sizeof hostname_buf);
        hostname_buf[sizeof hostname_buf - 1] = 0;
        cyw43_arch_lwip_begin();
        netif_set_hostname(sta_netif(), hostname_buf);
        cyw43_arch_lwip_end();

        link_state = LS_JOINING;
        rc = cyw43_arch_wifi_connect_async(c.ssid, c.auth == NETCFG_AUTH_OPEN ? NULL : c.psk, auth_mode(c.auth));
        memset(c.psk, 0, sizeof c.psk);

        int status = CYW43_LINK_JOIN;
        for (uint32_t t = 0; rc == 0 && t < 30000 && gen == cfg_generation; t += 100) {
            status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (status == CYW43_LINK_UP || status < 0)
                break;
            led((t % 1000) < 500);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (gen != cfg_generation) {
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            continue;
        }
        if (rc != 0 || status != CYW43_LINK_UP) {
            last_join_status = rc ? rc : status;
            link_state = LS_FAILED;
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            wait_ms_blinking(10000, 2, gen);
            continue;
        }

        /* Power save adds up to a DTIM interval of latency per DAP round
         * trip; a probe on a USB charger does not need it. */
        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
        ip_addr = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(sta_netif())));
        mdns_up();
        link_state = LS_UP;
        last_join_status = 0;

        uint32_t t = 0;
        while (gen == cfg_generation) {
            int s = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (s != CYW43_LINK_UP)
                break;
            ip_addr = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(sta_netif())));
            led(client_connected ? ((t % 200) < 100) : true);
            vTaskDelay(pdMS_TO_TICKS(100));
            t += 100;
        }
        link_state = LS_JOINING;
        ip_addr = 0;
        mdns_down();
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        led(false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void net_init(void)
{
    cfg_mutex = xSemaphoreCreateMutex();
    netcfg_load(&active_cfg);
    xTaskCreate(net_thread, "NET", 2048, NULL, NET_TASK_PRIO, NULL);
    TaskHandle_t h;
    xTaskCreate(dtcp_thread, "DTCP", 2048, NULL, DTCP_TASK_PRIO, &h);
#if (configNUMBER_OF_CORES > 1)
    vTaskCoreAffinitySet(h, (1 << 1));   /* same core as the USB DAP thread */
#endif
}

/* ---------------- config console ---------------- */

static netcfg_console_t console;
static volatile bool console_active;
static volatile bool console_banner_pending;
static volatile int console_reboot;      /* 0 none, 1 reboot, 2 bootsel */

static void con_write(const char *s, size_t n, void *ctx)
{
    (void)ctx;
    while (n) {
        uint32_t w = tud_cdc_write(s, n);
        tud_cdc_write_flush();
        if (w == 0) {
            if (!tud_cdc_connected())
                return;
            vTaskDelay(1);
            continue;
        }
        s += w;
        n -= w;
    }
}

static int con_save(const netcfg_t *c, void *ctx)
{
    (void)ctx;
    netcfg_t old;
    cfg_get(&old);
    int rc = netcfg_store(c);
    if (rc == 0) {
        cfg_set(c);
        if (memcmp(old.country, c->country, sizeof old.country) != 0)
            con_write("note: the country code takes effect after 'reboot'\r\n", 52, NULL);
    }
    memset(&old, 0, sizeof old);
    return rc;
}

static int con_erase(void *ctx)
{
    (void)ctx;
    int rc = netcfg_erase();
    netcfg_t d;
    netcfg_defaults(&d);
    cfg_set(&d);
    return rc;
}

static void con_reboot(bool bootsel, void *ctx)
{
    (void)ctx;
    console_reboot = bootsel ? 2 : 1;
}

static const char *state_name(enum link_state s)
{
    switch (s) {
    case LS_OFF: return "off (no SSID or 'wifi off')";
    case LS_JOINING: return "joining";
    case LS_FAILED: return "join failed, retrying";
    case LS_UP: return "connected";
    case LS_INIT_FAILED: return "CYW43 init failed";
    }
    return "?";
}

static void con_status(char *out, size_t n, void *ctx)
{
    (void)ctx;
    netcfg_t c;
    cfg_get(&c);
    memset(c.psk, 0, sizeof c.psk);
    char ip[16], cip[16];
    netcfg_format_ipv4(ip_addr, ip);
    netcfg_format_ipv4(client_ip, cip);
    snprintf(out, n,
             "wifi     : %s%s%s\r\n"
             "last err : %d\r\n"
             "ip       : %s\r\n"
             "dap      : %s:%u  (openocd: cmsis-dap tcp host %s)\r\n"
             "mdns     : %s.local\r\n"
             "client   : %s\r\n"
             "uptime   : %lu s\r\n",
             state_name(link_state), c.ssid[0] ? " to " : "", c.ssid,
             last_join_status,
             ip_addr ? ip : "-",
             ip_addr ? ip : "-", (unsigned)c.port, ip_addr ? ip : "-",
             c.hostname,
             client_connected ? cip : "none",
             (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000));
}

static const netcfg_console_io_t console_io = {
    .write = con_write,
    .save = con_save,
    .erase = con_erase,
    .reboot = con_reboot,
    .status = con_status,
    .ctx = NULL,
};

void netcon_enter(void)
{
    if (console_active)
        return;
    netcfg_t c;
    cfg_get(&c);
    netcfg_console_init(&console, &console_io, &c);
    memset(&c, 0, sizeof c);
    console_banner_pending = true;
    console_active = true;
}

void netcon_exit(void)
{
    console_active = false;
    memset(&console, 0, sizeof console);   /* drop edits incl. password */
}

bool netcon_active(void)
{
    return console_active;
}

void netcon_poll(void)
{
    if (!tud_cdc_connected())
        return;
    if (console_banner_pending) {
        console_banner_pending = false;
        netcfg_console_banner(&console);
    }
    uint8_t buf[64];
    uint32_t n;
    while ((n = tud_cdc_read(buf, sizeof buf)) > 0)
        netcfg_console_input(&console, buf, n);
    memset(buf, 0, sizeof buf);
    if (console_reboot) {
        vTaskDelay(pdMS_TO_TICKS(200));    /* let "OK" reach the host */
        if (console_reboot == 2)
            reset_usb_boot(0, 0);
        else
            watchdog_reboot(0, 0, 10);
        for (;;)
            ;
    }
}
