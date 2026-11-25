/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The debugprobe-wifi authors
 */
/*
 * WiFi side of debugprobe-wifi (Pico 2 W).
 *
 * Tasks (all at tskIDLE_PRIORITY + 1, i.e. below the stock USB (+2) and
 * UART (+3) tasks, so nothing here can starve USB DAP):
 *   NET   joins the stored network, keeps it joined, mDNS
 *   DTCP  CMSIS-DAP TCP server (OpenOCD "cmsis-dap tcp" protocol), core 1
 *   LED   LED patterns (needs the CYW43, which drives the LED)
 *   CFG   config console (8 KiB stack; the stock UART task has 1 KiB)
 *   WDT   hardware watchdog feed + USB state log (diag_rp.c)
 *   CYW43 async context (created here: priority +1, core 1)
 *
 * LED (CYW43 WL_GPIO0), see led_thread():
 *   3 quick blinks at boot     firmware running, CYW43 OK
 *   then N long blinks x3      previous boot crashed: 1 stack, 2 panic, 3 hard fault, 4 hang
 *   short flash every 2 s      alive, no WiFi configured (or 'wifi off')
 *   1 Hz 50 %                  joining
 *   double flash every 2 s     join failed, retrying
 *   on                         joined, no DAP client
 *   fast blink 5 Hz            DAP client connected
 *   never lights               firmware not running, CYW43 init failed or safe mode:
 *                              see the boot log (CDC port at 115200) / "status"
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/async_context_freertos.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
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
#include "diag.h"
#include "plog.h"
#include "probe/version.h"

#define LOW_PRIO   (tskIDLE_PRIORITY + 1)

enum link_state { LS_OFF, LS_INIT, LS_JOINING, LS_FAILED, LS_UP, LS_INIT_FAILED, LS_SAFE_MODE };

static SemaphoreHandle_t dap_mutex;
static SemaphoreHandle_t cfg_mutex;
static netcfg_t active_cfg;               /* guarded by cfg_mutex */
static bool cfg_from_flash;
static volatile uint32_t cfg_generation;
static volatile enum link_state link_state = LS_OFF;
static volatile bool cyw43_ready;
static volatile int cyw43_init_rc = 1;     /* 1 = not tried yet */
static volatile int last_join_status;
static volatile uint32_t ip_addr;          /* host order, 0 = none */
static volatile bool client_connected;
static volatile uint32_t client_ip;
static volatile uint32_t dtcp_clients, dtcp_commands;
static char hostname_buf[33];
static TaskHandle_t h_net, h_dtcp, h_led, h_cfg;

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
    dtcp_commands++;
    return r & 0xFFFFu;
}

static bool dtcp_allow(uint32_t ip, void *ctx)
{
    (void)ctx;
    netcfg_t c;
    cfg_get(&c);
    bool ok = netcfg_peer_allowed(&c, ip);
    memset(c.psk, 0, sizeof c.psk);
    if (!ok)
        plog("dap: refused %lu.%lu.%lu.%lu (allow list)", (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 255),
             (unsigned long)((ip >> 8) & 255), (unsigned long)(ip & 255));
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
    } else {
        dtcp_clients++;
    }
    client_ip = ip;
    client_connected = connected;
    plog("dap: client %lu.%lu.%lu.%lu %s", (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 255),
         (unsigned long)((ip >> 8) & 255), (unsigned long)(ip & 255), connected ? "connected" : "disconnected");
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
        plog("dap: listening on tcp port %u", (unsigned)c.port);
        int rc = dap_tcp_server_run(&sc);
        if (rc < 0) {
            plog("dap: socket setup failed (%d)", rc);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---------------- LED ---------------- */

enum led_mode { LED_ALIVE, LED_JOINING, LED_FAILED, LED_UP, LED_CLIENT };
static volatile enum led_mode led_mode = LED_ALIVE;

static void led(bool on)
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

static void led_for(bool on, uint32_t ms)
{
    led(on);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void led_thread(void *ptr)
{
    (void)ptr;
    for (int i = 0; i < 3; i++) {             /* "firmware running, CYW43 OK" */
        led_for(true, 80);
        led_for(false, 120);
    }
    vTaskDelay(pdMS_TO_TICKS(800));
    int code = diag_boot_info.reason;
    if (code >= DIAG_STACK && code <= DIAG_HANG) {
        for (int rep = 0; rep < 3; rep++) {
            for (int i = 0; i < code; i++) {
                led_for(true, 600);
                led_for(false, 400);
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
    uint32_t t = 0;
    for (;;) {
        bool on;
        uint32_t ph = t % 2000;
        switch (led_mode) {
        case LED_JOINING: on = (t % 1000) < 500; break;
        case LED_FAILED:  on = ph < 100 || (ph >= 250 && ph < 350); break;
        case LED_UP:      on = true; break;
        case LED_CLIENT:  on = (t % 200) < 100; break;
        default:          on = ph < 50; break;   /* alive heartbeat */
        }
        led(on);
        vTaskDelay(pdMS_TO_TICKS(50));
        t += 50;
    }
}

/* ---------------- WiFi ---------------- */

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

static async_context_freertos_t cyw43_ctx;

static bool cyw43_bringup(uint32_t country)
{
    /* Own async context: low priority and on core 1, so the CYW43 driver can
     * never preempt the USB / UART tasks (stock default would be +4). */
    async_context_freertos_config_t ac = async_context_freertos_default_config();
    ac.task_priority = LOW_PRIO;
    ac.task_stack_size = 2048;
#if configUSE_CORE_AFFINITY && configNUMBER_OF_CORES > 1
    ac.task_core_id = 1;
#endif
    if (!async_context_freertos_init(&cyw43_ctx, &ac)) {
        cyw43_init_rc = -1000;
        return false;
    }
    cyw43_arch_set_async_context(&cyw43_ctx.core);

    /* The SWD state machine uses pio0 SM0 without claiming it, autobaud
     * claims pio0 SMs at run time: park pio0 so the CYW43 SPI goes elsewhere. */
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
    cyw43_init_rc = rc;
    return rc == 0;
}

static void wait_gen(uint32_t gen, uint32_t ms)
{
    for (uint32_t t = 0; t < ms && gen == cfg_generation; t += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
}

static void net_thread(void *ptr)
{
    (void)ptr;
    netcfg_t c;
    cfg_get(&c);

    if (diag_boot_info.safe_mode) {
        link_state = LS_SAFE_MODE;
        plog("net: SAFE MODE after %d crashes in a row: WiFi not started (USB DAP only)", diag_boot_info.crash_count);
        vTaskSuspend(NULL);
    }
    if (!c.net_enabled) {
        link_state = LS_OFF;
        plog("net: wifi off in settings: CYW43 not started (no LED)");
        do {
            vTaskDelay(pdMS_TO_TICKS(500));
            cfg_get(&c);
        } while (!netcfg_has_wifi(&c));
    }

    link_state = LS_INIT;
    diag_stage(STAGE_CYW43_INIT);
    plog("net: CYW43 init (country %s) ...", c.country);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    if (!cyw43_bringup(CYW43_COUNTRY(c.country[0], c.country[1], 0))) {
        link_state = LS_INIT_FAILED;
        plog("net: CYW43 init FAILED rc=%d: no WiFi, no LED; USB DAP unaffected", cyw43_init_rc);
        vTaskSuspend(NULL);
    }
    cyw43_ready = true;
    diag_stage(STAGE_CYW43_OK);
    plog("net: CYW43 ready in %lu ms", (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
    xTaskCreate(led_thread, "LED", 1024, NULL, LOW_PRIO, &h_led);
    cyw43_arch_enable_sta_mode();

    for (;;) {
        uint32_t gen = cfg_generation;
        cfg_get(&c);
        if (!netcfg_has_wifi(&c)) {
            link_state = LS_OFF;
            led_mode = LED_ALIVE;
            ip_addr = 0;
            plog("net: %s", c.ssid[0] ? "wifi off" : "no WiFi configured (use the config console)");
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
        led_mode = LED_JOINING;
        diag_stage(STAGE_JOINING);
        plog("net: joining '%s' (%s)", c.ssid, c.auth == NETCFG_AUTH_OPEN ? "open" : c.auth == NETCFG_AUTH_WPA3 ? "wpa3" : "wpa2");
        int rc = cyw43_arch_wifi_connect_async(c.ssid, c.auth == NETCFG_AUTH_OPEN ? NULL : c.psk, auth_mode(c.auth));
        memset(c.psk, 0, sizeof c.psk);

        int status = CYW43_LINK_JOIN;
        uint32_t t = 0;
        for (; rc == 0 && t < 30000 && gen == cfg_generation; t += 100) {
            status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (status == CYW43_LINK_UP || status < 0)
                break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (gen != cfg_generation) {
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            continue;
        }
        if (rc != 0 || status != CYW43_LINK_UP) {
            last_join_status = rc ? rc : status;
            link_state = LS_FAILED;
            led_mode = LED_FAILED;
            plog("net: join failed: %s (%d), retry in 10 s",
                 rc ? "connect call failed" :
                 status == CYW43_LINK_BADAUTH ? "wrong password" :
                 status == CYW43_LINK_NONET ? "network not found" :
                 status == CYW43_LINK_FAIL ? "connection failed" :
                 status == CYW43_LINK_NOIP ? "no IP address (DHCP)" : "timeout",
                 last_join_status);
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            wait_gen(gen, 10000);
            continue;
        }

        /* Power save adds up to a DTIM interval of latency per DAP round trip. */
        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
        ip_addr = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(sta_netif())));
        mdns_up();
        link_state = LS_UP;
        last_join_status = 0;
        diag_stage(STAGE_JOINED);
        {
            char ip[16];
            netcfg_format_ipv4(ip_addr, ip);
            plog("net: joined, ip %s, mdns %s.local, %lu ms", ip, hostname_buf, (unsigned long)t);
        }

        while (gen == cfg_generation) {
            int s = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (s != CYW43_LINK_UP) {
                plog("net: link lost (%d)", s);
                break;
            }
            ip_addr = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(sta_netif())));
            led_mode = client_connected ? LED_CLIENT : LED_UP;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        link_state = LS_JOINING;
        ip_addr = 0;
        mdns_down();
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------------- config console (CFG task) ---------------- */

static netcfg_console_t console;
static volatile bool console_active;      /* UART bridge yields the CDC port */
static volatile bool console_enter_req, console_exit_req;
static volatile const char *console_enter_how;
static volatile int console_reboot;      /* 0 none, 1 reboot, 2 bootsel */

static void con_write(const char *s, size_t n, void *ctx)
{
    (void)ctx;
    uint32_t idle = 0;
    while (n) {
        uint32_t w = tud_cdc_write(s, n);
        tud_cdc_write_flush();
        if (w == 0) {
            if (!tud_cdc_connected() || ++idle > 500)   /* host not reading: drop after ~0.5 s */
                return;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        idle = 0;
        s += w;
        n -= w;
    }
}

static int con_save(const netcfg_t *c, void *ctx)
{
    (void)ctx;
    /* Never write flash while the CYW43 firmware is being streamed from it. */
    for (int i = 0; i < 300 && link_state == LS_INIT; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    netcfg_t old;
    cfg_get(&old);
    diag_stage(STAGE_FLASH_WRITE);
    int rc = netcfg_store(c);
    diag_stage(cyw43_ready ? STAGE_CYW43_OK : STAGE_SCHED);
    plog("cfg: settings saved (rc %d), ssid '%s'", rc, c->ssid);
    if (rc == 0) {
        cfg_set(c);
        cfg_from_flash = true;
        if (memcmp(old.country, c->country, sizeof old.country) != 0 || (!cyw43_ready && link_state != LS_SAFE_MODE))
            con_write("note: 'reboot' to apply (country code / WiFi chip start)\r\n", 58, NULL);
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
    cfg_from_flash = false;
    plog("cfg: settings erased (rc %d)", rc);
    return rc;
}

static void con_reboot(bool bootsel, void *ctx)
{
    (void)ctx;
    console_reboot = bootsel ? 2 : 1;
}

static void con_quit(void *ctx)
{
    (void)ctx;
    console_exit_req = true;
}

static char log_buf[PLOG_SIZE];

static void con_log(void *ctx)
{
    (void)ctx;
    size_t n = plog_read(log_buf, sizeof log_buf);
    con_write(log_buf, n, NULL);
}

static const char *state_name(enum link_state s)
{
    switch (s) {
    case LS_OFF: return "off (no SSID or 'wifi off')";
    case LS_INIT: return "starting the WiFi chip";
    case LS_JOINING: return "joining";
    case LS_FAILED: return "join failed, retrying";
    case LS_UP: return "connected";
    case LS_INIT_FAILED: return "CYW43 init FAILED";
    case LS_SAFE_MODE: return "SAFE MODE (crash loop, WiFi not started)";
    }
    return "?";
}

static unsigned hw(TaskHandle_t h)
{
    return h ? (unsigned)uxTaskGetStackHighWaterMark(h) * 4u : 0u;
}

static void con_status(char *out, size_t n, void *ctx)
{
    (void)ctx;
    netcfg_t c;
    cfg_get(&c);
    memset(c.psk, 0, sizeof c.psk);
    char ip[16], cip[16], why[160];
    netcfg_format_ipv4(ip_addr, ip);
    netcfg_format_ipv4(client_ip, cip);
    diag_describe(&diag_boot_info, why, sizeof why);
    snprintf(out, n,
             "wifi     : %s%s%s\r\n"
             "last err : %d\r\n"
             "ip       : %s\r\n"
             "dap      : %s:%u  (openocd: cmsis-dap tcp host %s)\r\n"
             "mdns     : %s.local\r\n"
             "client   : %s (clients %lu, commands %lu)\r\n"
             "uptime   : %lu s\r\n"
             "firmware : debugprobe-wifi %s\r\n"
             "settings : %s\r\n"
             "cyw43    : %s (rc %d)\r\n"
             "reset    : %s\r\n"
             "crashes  : %d in a row%s\r\n"
             "heap     : %u free, %u min\r\n"
             "stack    : free bytes NET %u DTCP %u LED %u CFG %u\r\n",
             state_name(link_state), c.ssid[0] ? " to " : "", c.ssid,
             last_join_status,
             ip_addr ? ip : "-",
             ip_addr ? ip : "-", (unsigned)c.port, ip_addr ? ip : "-",
             c.hostname,
             client_connected ? cip : "none", (unsigned long)dtcp_clients, (unsigned long)dtcp_commands,
             (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000),
             PROBE_VERSION,
             cfg_from_flash ? "stored in flash" : "defaults (nothing stored)",
             cyw43_ready ? "ready" : cyw43_init_rc == 1 ? "not started" : "init failed", cyw43_init_rc,
             why,
             diag_boot_info.crash_count, diag_boot_info.safe_mode ? " -> safe mode" : "",
             (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize(),
             hw(h_net), hw(h_dtcp), hw(h_led), hw(h_cfg));
}

static const netcfg_console_io_t console_io = {
    .write = con_write,
    .save = con_save,
    .erase = con_erase,
    .reboot = con_reboot,
    .status = con_status,
    .log = con_log,
    .quit = con_quit,
    .ctx = NULL,
};

/* These run in the TinyUSB / UART tasks (small stacks): flags only. */
void netcon_enter(const char *how)
{
    console_enter_how = how;
    console_enter_req = true;
    console_active = true;      /* bridge stops touching the CDC port at once */
    if (h_cfg)
        xTaskNotifyGive(h_cfg);
}

void netcon_exit(void)
{
    if (console_active)
        console_exit_req = true;
    if (h_cfg)
        xTaskNotifyGive(h_cfg);
}

bool netcon_active(void)
{
    return console_active;
}

static void cfg_thread(void *ptr)
{
    (void)ptr;
    static uint8_t buf[64];
    bool open = false;
    for (;;) {
        if (console_exit_req) {
            console_exit_req = false;
            if (open)
                plog("cfg: console closed");
            open = false;
            console_enter_req = false;
            memset(&console, 0, sizeof console);   /* drop edits incl. password */
            console_active = false;
        }
        if (!console_active) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }
        if (console_enter_req) {
            console_enter_req = false;
            netcfg_t c;
            cfg_get(&c);
            netcfg_console_init(&console, &console_io, &c);
            memset(&c, 0, sizeof c);
            open = true;
            plog("cfg: console opened (%s)", console_enter_how ? (const char *)console_enter_how : "?");
            /* give the host a moment to raise DTR after setting the line coding */
            for (int i = 0; i < 50 && !tud_cdc_connected(); i++)
                vTaskDelay(pdMS_TO_TICKS(10));
            netcfg_console_banner(&console);
        }
        if (!tud_cdc_connected()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t n = tud_cdc_available() ? tud_cdc_read(buf, sizeof buf) : 0;
        if (n) {
            netcfg_console_input(&console, buf, n);
            memset(buf, 0, sizeof buf);
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        }
        if (console_reboot) {
            plog("cfg: %s", console_reboot == 2 ? "reboot to BOOTSEL" : "reboot");
            vTaskDelay(pdMS_TO_TICKS(200));    /* let "OK" reach the host */
            if (console_reboot == 2) {
                hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);   /* no reset while in BOOTSEL */
                watchdog_hw->scratch[2] = 0;   /* the next firmware must not read this as a crash */
                watchdog_hw->scratch[4] = 0;
                reset_usb_boot(0, 0);
            } else {
                diag_fault(DIAG_USER, 0, 0);
            }
        }
    }
}

/* Bridge side: boot log when the host opens the port and the target UART
 * has never sent anything (nothing wired). Runs in the UART task: no printf. */
void netcon_bridge_opened(uint32_t uart_rx_total)
{
    if (uart_rx_total != 0)
        return;
    static const char head[] =
        "\r\n--- debugprobe-wifi log (shown because the target UART has sent nothing) ---\r\n";
    static const char tail[] =
        "--- end of log; type +++wifiprobe and Enter for the config console ---\r\n";
    uint32_t n = (uint32_t)plog_read(log_buf, sizeof log_buf);
    uint32_t room = tud_cdc_write_available();
    uint32_t off = 0;
    if (room < sizeof head + sizeof tail + n) {      /* keep the newest lines */
        uint32_t fit = room > sizeof head + sizeof tail ? room - sizeof head - sizeof tail : 0;
        off = n - fit;
        n = fit;
    }
    tud_cdc_write(head, sizeof head - 1);
    tud_cdc_write(log_buf + off, n);
    tud_cdc_write(tail, sizeof tail - 1);
    tud_cdc_write_flush();
}

void net_init(void)
{
    cfg_mutex = xSemaphoreCreateMutex();
    cfg_from_flash = netcfg_load(&active_cfg);
    {
        char why[160];
        diag_describe(&diag_boot_info, why, sizeof why);
        plog("boot: debugprobe-wifi %s, reset: %s", PROBE_VERSION, why);
    }
    plog("boot: settings %s, ssid '%s', wifi %s, port %u, host %s",
         cfg_from_flash ? "from flash" : "defaults", active_cfg.ssid, active_cfg.net_enabled ? "on" : "off",
         (unsigned)active_cfg.port, active_cfg.hostname);

    xTaskCreate(cfg_thread, "CFG", 2048, NULL, LOW_PRIO, &h_cfg);
    xTaskCreate(net_thread, "NET", 2048, NULL, LOW_PRIO, &h_net);
    xTaskCreate(dtcp_thread, "DTCP", 2048, NULL, LOW_PRIO, &h_dtcp);
#if (configNUMBER_OF_CORES > 1)
    vTaskCoreAffinitySet(h_dtcp, (1 << 1));   /* same core as the USB DAP thread */
    vTaskCoreAffinitySet(h_net, (1 << 1));
#endif
    diag_start_watchdog_task();
}
