/*
 * XPRS over ESP-NOW — see xprsnow.h for the bearer and docs/espnow.md for the
 * wire. The queue, the identifier rings and the §13.2.1 cancel are not here:
 * they are `xprs_bearer`, shared with the LAN.
 *
 * What is here is the radio, and one rule that shapes all of it: the receive
 * callback runs in the WiFi task on core 0, so it copies and returns.
 */

#include "xprsnow.h"
#include "xprsbearer.h"
#include "xprs.h"
#include "xprs_psram.h"   /* xprs_big_calloc: the bearer state off the .bss */

#include <string.h>
#include <stdio.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

static const char *TAG = "xprsnow";

static const uint8_t k_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* One frame, as the callback hands it over. 264 bytes; four of them is the
 * whole cost of getting off the WiFi task. */
typedef struct {
    char    wire[XPRSNOW_WIRE_MAX + 1];
    int     len;
    int     rssi;
    uint8_t mac[6];
} nw_frame_t;

/* Was a 3.4 KB static; xprs_big_calloc'd in start so the .bss stays under
 * the RF-bank ceiling on the xwifi boards (every xb_* entry is NULL-safe,
 * so before start this bearer behaves exactly as a never-started one). */
static xb_t             *s_now;
static QueueHandle_t     s_rxq;
static SemaphoreHandle_t s_tx_mtx;
static xprsnow_rx_cb_t   s_rx_cb;
static uint32_t          s_dropped;
static bool              s_started;
static bool              s_trace;

/* The driver's own accounting, which is the only place "it went out" lives.
 * `issued` rises where we hand a frame over; `done` and `failed` rise in the
 * send callback, which runs on the WiFi task — so both sides are one writer
 * each and a plain volatile is enough to compare them. */
static volatile uint32_t s_tx_issued;
static volatile uint32_t s_tx_done;
static volatile uint32_t s_tx_failed;

/* Every frame the radio delivered, and how many of those were not XPRS. */
static volatile uint32_t s_frames;
static volatile uint32_t s_notxprs;

/* ── The radio ──────────────────────────────────────────────────────────── */

static uint32_t nw_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t nw_random(void) { return esp_random(); }
static void nw_lock(void *ctx)   { (void)ctx; if (s_tx_mtx) xSemaphoreTake(s_tx_mtx, portMAX_DELAY); }
static void nw_unlock(void *ctx) { (void)ctx; if (s_tx_mtx) xSemaphoreGive(s_tx_mtx); }

/* ── Long range on a home channel ────────────────────────────────────────────
 *
 * ESP-NOW has a channel of its own, the home channel (13 by default), and
 * sends on the long-range PHY (LR, 250 kbps). docs/espnow.md has the why.
 *
 * A station on no router sits on the home channel and hears everything. A
 * station on a router stays on the router's channel for WiFi and visits the
 * home channel once a second, for a window aligned to the wall clock (SNTP):
 * [0, s_win_ms) of every second. Every station that knows the time sends
 * inside that window, so the visitors are there to hear it. A station without
 * a clock learns the window from the frames it hears (they arrive inside it);
 * one that has heard nothing yet sends at once, which reaches every station
 * sitting on the home channel. */

#define NW_PERIOD_US  1000000LL
#define NW_SEND_AT_MS 25          /* sends start this far into the window */
#define NW_TXQ        8

typedef struct { int len; char wire[XPRSNOW_WIRE_MAX]; } nw_tx_t;

static uint8_t           s_home_ch = 13;
static bool              s_lr = true;
static uint32_t          s_win_ms = 80;
static nw_tx_t          *s_txq;          /* NW_TXQ entries, claimed at start */
static int               s_txq_n;
static portMUX_TYPE      s_txq_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t  s_learned_t0 = -1;  /* esp_timer us of a window start */
static TaskHandle_t      s_win_task;

/* Window counters: visits to the home channel, refused channel switches,
 * frames sent inside a window, frames dropped from a full window queue. */
static volatile uint32_t s_visits, s_switch_fail, s_win_sent, s_win_dropped;

/* ── WiFi in use: the visit waits ────────────────────────────────────────────
 *
 * A visit takes the radio off the router's channel for the window and for
 * what the driver needs to tell the access point it is away and back:
 * measured on a C3 behind a FRITZ!Box, 150 to 290 ms of every second in
 * which nothing addressed to the station arrives (the access point holds it
 * and hands it over after). A page load or a Reticulum transfer waits those
 * out one after another.
 *
 * (What the visits are NOT: the pings this board loses under load, 63 % and
 * 77 % when it was first measured. With the home channel set to the router's,
 * so that no visit happens at all, the same pings lost 0 to 2 % with 17 to
 * 20 KB of heap free and 51 to 87 % with 8 KB. That is the heap, not this.)
 *
 * So the station counts the frames addressed to it alone (unicast: broadcast
 * and multicast, which is all the LAN bearer's chatter, do not count). A
 * second that brought NW_BUSY_FRAMES of them is somebody talking to this
 * station, and the next visit is put off -- NW_BUSY_SKIPS visits in a row at
 * most, then one visit whatever the traffic, so ESP-NOW is heard and what
 * waits in the window queue goes out at least every NW_BUSY_SKIPS + 1
 * seconds. A station nobody is talking to visits every second as before.
 * That is the rule for traffic nobody announced (a ping, a client of some
 * other service). Traffic that is announced is held for outright:
 * xprsnow_hold(), below.
 *
 * The count is taken where lwIP is handed a frame: the interface's input
 * function is wrapped (IN THE WIFI TASK: one test and one increment). */
#define NW_BUSY_FRAMES 12
#define NW_BUSY_SKIPS  5
#define NW_HOLD_TRAIL_MS 60000u

static volatile uint32_t s_uni_rx;
static uint32_t          s_uni_seen, s_uni_last, s_uni_peak;
static uint8_t           s_skip_run;
static volatile uint32_t s_skipped;
static netif_input_fn    s_input_orig;

static err_t nw_input(struct pbuf *p, struct netif *inp)
{
    if (p && p->len >= 1 && !(((const uint8_t *)p->payload)[0] & 1)) s_uni_rx++;
    return s_input_orig(p, inp);
}

/* The station (and the hotspot) interface exists only once WiFi has started,
 * and is made again when it restarts: looked at before every window. */
static void nw_watch_netifs(void)
{
    static const char *const keys[] = { "WIFI_STA_DEF", "WIFI_AP_DEF" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        esp_netif_t *en = esp_netif_get_handle_from_ifkey(keys[i]);
        struct netif *n = en ? esp_netif_get_netif_impl(en) : NULL;
        if (!n || !n->input || n->input == nw_input) continue;
        /* Every lwIP interface here takes its frames through the same
         * function (tcpip_input), so one saved pointer serves both. */
        if (!s_input_orig) s_input_orig = n->input;
        if (n->input == s_input_orig) n->input = nw_input;
    }
}

/* Somebody is using this station over WiFi: a Reticulum link is open, the
 * web server took a connection. Whoever knows says so, and for that long no
 * visit leaves the router's channel -- however long it lasts: ESP-NOW is
 * there for range, such visitors are few, and a station that hops away from
 * the one it is serving serves badly (the operator's decision, 2026-11-11).
 * What waits in the window queue goes out at the first visit after. */
static volatile uint32_t s_hold_until_ms, s_hold_last_ms;
static volatile bool     s_hold_any;
static volatile uint32_t s_held;

void xprsnow_hold(uint32_t ms)
{
    uint32_t now = nw_now_ms(), until = now + ms;
    if (!s_hold_any || (int32_t)(until - s_hold_until_ms) > 0) s_hold_until_ms = until;
    s_hold_last_ms = now;
    s_hold_any = true;
}

/* True when the visit about to start should be put off. */
static bool nw_wifi_busy(void)
{
    uint32_t cnt = s_uni_rx, now = nw_now_ms();
    s_uni_last = cnt - s_uni_seen;
    s_uni_seen = cnt;
    if (s_uni_last > s_uni_peak) s_uni_peak = s_uni_last;
    bool rate = s_uni_last >= NW_BUSY_FRAMES;
    if (s_hold_any && (int32_t)(s_hold_until_ms - now) > 0) { s_held++; s_skip_run = 0; return true; }
    /* Traffic that follows a hold within a minute is the same visitor still
     * at it (a long download on a connection opened a while ago). */
    if (rate && s_hold_any && now - s_hold_last_ms < NW_HOLD_TRAIL_MS) { s_held++; s_skip_run = 0; return true; }
    if (rate && s_skip_run < NW_BUSY_SKIPS) {
        s_skip_run++;
        s_skipped++;
        return true;
    }
    s_skip_run = 0;
    return false;
}

static bool nw_wall_ok(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1735689600;   /* 2025-01-01: SNTP has run */
}

/* Microseconds into the current one-second period, or -1 when this station
 * does not know where the window is. */
static int64_t nw_phase_us(void)
{
    if (nw_wall_ok()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return ((int64_t)tv.tv_sec * 1000000LL + tv.tv_usec) % NW_PERIOD_US;
    }
    int64_t t0 = s_learned_t0;
    if (t0 < 0) return -1;
    return (esp_timer_get_time() - t0) % NW_PERIOD_US;
}

/* IN THE WIFI TASK. A station without a clock takes the window from what it
 * hears: a frame arrives about NW_SEND_AT_MS into the sender's window. */
static void nw_learn_phase(void)
{
    if (nw_wall_ok()) return;
    s_learned_t0 = esp_timer_get_time() - (int64_t)NW_SEND_AT_MS * 1000;
}

/* Queue a frame for the next window. false: send it now instead. */
static bool nw_defer(const char *wire, int len)
{
    if (!s_txq || !s_win_task || nw_phase_us() < 0) return false;
    bool queued = false;
    portENTER_CRITICAL(&s_txq_mux);
    if (s_txq_n < NW_TXQ) {
        memcpy(s_txq[s_txq_n].wire, wire, (size_t)len);
        s_txq[s_txq_n].len = len;
        s_txq_n++;
        queued = true;
    }
    portEXIT_CRITICAL(&s_txq_mux);
    if (!queued) s_win_dropped++;
    return true;   /* a full queue drops the frame, as a busy radio would */
}

static bool nw_send_now(const char *wire, int len);

/* Send what waits for the window. */
static void nw_flush(void)
{
    static nw_tx_t one;   /* the window task is the only caller */
    for (;;) {
        bool have = false;
        portENTER_CRITICAL(&s_txq_mux);
        if (s_txq_n > 0) {
            one = s_txq[0];
            memmove(&s_txq[0], &s_txq[1], sizeof(nw_tx_t) * (size_t)(s_txq_n - 1));
            s_txq_n--;
            have = true;
        }
        portEXIT_CRITICAL(&s_txq_mux);
        if (!have) return;
        if (nw_send_now(one.wire, one.len)) s_win_sent++;
    }
}

/* A station on no router belongs on the home channel; anything that moved
 * it (a mode switch, a scan) is undone here. */
/* A network configured for the station: it is joining one, or will. */
static bool nw_has_router(void)
{
    wifi_config_t wc;
    return esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0];
}

static void nw_home_if_free(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return;
    /* Joining a router scans every channel; pulling the radio back to the
     * home channel would break the scan every time. */
    if (nw_has_router()) return;
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return;
    if (primary != s_home_ch &&
        esp_wifi_set_channel(s_home_ch, WIFI_SECOND_CHAN_NONE) != ESP_OK)
        s_switch_fail++;
}

/* The driver refuses esp_wifi_set_channel() while the station is on a router.
 * Remain-on-channel is how it leaves the router's channel for a while and
 * comes back by itself (802.11 off-channel operation, used for DPP). The
 * call is in the driver's private header in IDF 5.2, so it is declared here. */
extern esp_err_t esp_wifi_remain_on_channel(uint8_t ifx, uint8_t type,
                                            uint8_t channel, uint32_t wait_time_ms,
                                            wifi_action_rx_cb_t rx_cb);

static void nw_window_task(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t ph = nw_phase_us();
        if (ph < 0) {
            /* No window known: whatever waits goes now, on the home channel
             * if this station is not on a router. */
            nw_home_if_free();
            nw_flush();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int64_t wait_us = NW_PERIOD_US - ph;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_us / 1000) + 1));
        int64_t start = esp_timer_get_time();

        wifi_ap_record_t ap;
        bool assoc = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        if (!assoc && nw_has_router()) {
            /* Still joining: leave the radio to the scan, and send what
             * waits on whatever channel it is on. */
            nw_flush();
            continue;
        }
        uint8_t primary = 0;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&primary, &second);
        bool roc = false;
        /* Somebody is talking to this station over WiFi: stay with them. */
        nw_watch_netifs();
        bool busy = nw_wifi_busy();
        if (assoc && primary != s_home_ch && busy) {
            /* Well past the boundary before the next one is waited for: the
             * delay above can end a tick early, and the same window would be
             * judged again on the few frames of those milliseconds. */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (primary != s_home_ch) {
            esp_err_t e;
            if (assoc) {
                /* Off-channel for the window; the driver returns by itself. */
                e = esp_wifi_remain_on_channel(WIFI_IF_STA, WIFI_ROC_REQ, s_home_ch,
                                               s_win_ms + 20, NULL);
                roc = e == ESP_OK;
            } else {
                e = esp_wifi_set_channel(s_home_ch, WIFI_SECOND_CHAN_NONE);
            }
            if (e != ESP_OK) {
                if (s_switch_fail++ < 3)
                    ESP_LOGW(TAG, "home channel %u refused: %s", s_home_ch,
                             esp_err_to_name(e));
                continue;
            }
        }
        s_visits++;

        /* Into the window, with a little spread so the senders do not all
         * start in the same microsecond (the radio's own carrier sense does
         * the rest). */
        uint32_t at = NW_SEND_AT_MS + (esp_random() % 10);
        int64_t now = esp_timer_get_time();
        if ((now - start) / 1000 < at)
            vTaskDelay(pdMS_TO_TICKS(at - (uint32_t)((now - start) / 1000)));
        nw_flush();

        /* Stay to listen, and do not leave before the driver has sent. */
        for (;;) {
            uint32_t el = (uint32_t)((esp_timer_get_time() - start) / 1000);
            bool sent = s_tx_done == s_tx_issued;
            if ((el >= s_win_ms && sent) || el >= s_win_ms + 40) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (roc) esp_wifi_remain_on_channel(WIFI_IF_STA, WIFI_ROC_CANCEL, s_home_ch,
                                            0, NULL);
    }
}

void xprsnow_configure(uint8_t home_channel, bool long_range, uint32_t window_ms)
{
    if (home_channel >= 1 && home_channel <= 14) s_home_ch = home_channel;
    s_lr = long_range;
    if (window_ms >= 30 && window_ms <= 500) s_win_ms = window_ms;
}

uint8_t xprsnow_home_channel(void) { return s_home_ch; }
bool    xprsnow_long_range(void)   { return s_lr; }

void xprsnow_window_stats(uint32_t *visits, uint32_t *switch_fail,
                          uint32_t *sent, uint32_t *dropped)
{
    if (visits)      *visits = s_visits;
    if (switch_fail) *switch_fail = s_switch_fail;
    if (sent)        *sent = s_win_sent;
    if (dropped)     *dropped = s_win_dropped;
}

void xprsnow_window_busy(uint32_t *skipped, uint32_t *last, uint32_t *peak)
{
    if (skipped) *skipped = s_skipped + s_held;
    if (last)    *last = s_uni_last;
    if (peak)    { *peak = s_uni_peak; s_uni_peak = 0; }
}

static bool nw_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_started || len <= 0 || len > XPRSNOW_WIRE_MAX) return false;
    /* A station that knows when the window is sends inside it: that is when
     * the stations on a router are on the home channel to hear it. */
    if (nw_defer(wire, len)) return true;
    return nw_send_now(wire, len);
}

static bool nw_send_now(const char *wire, int len)
{
    esp_err_t err = esp_now_send(k_broadcast, (const uint8_t *)wire, (size_t)len);
    if (err == ESP_OK) { s_tx_issued++; return true; }
    /* Loud, including the full queue. This was ESP_LOGD and therefore invisible,
     * and an invitation that never left the board looked from every log on the
     * device exactly like an invitation nobody answered. */
    ESP_LOGW(TAG, "esp_now_send(%d bytes) failed: %s", len,
             esp_err_to_name(err));
    return false;
}

/*
 * Also the WiFi task. Two counters and nothing else.
 *
 * IDF 5.5 changed the argument: the peer's MAC became an esp_now_send_info_t
 * carrying it. Both spellings are kept because both toolchains are in use --
 * PlatformIO pins espressif32@6.7.0 (IDF 5.2) for the boards, and the
 * standalone IDF on a workstation is newer. Nothing here reads the argument,
 * so the two bodies are the same body.
 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void nw_send_cb(const esp_now_send_info_t *info,
                       esp_now_send_status_t status)
{
    (void)info;
#else
static void nw_send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
#endif
    if (status != ESP_NOW_SEND_SUCCESS) s_tx_failed++;
    s_tx_done++;
}

bool xprsnow_settle(uint32_t timeout_ms)
{
    if (!s_started) return false;
    uint32_t start = nw_now_ms();
    while (s_tx_done != s_tx_issued) {
        if (nw_now_ms() - start >= timeout_ms) {
            ESP_LOGW(TAG, "%u frame(s) still in the driver after %ums",
                     (unsigned)(s_tx_issued - s_tx_done), (unsigned)timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void xprsnow_set_trace(bool on) { s_trace = on; }

/*
 * IN THE WIFI TASK, ON CORE 0. Copy and get out.
 *
 * Everything this packet needs — the §5 identifier (a SHA-256), the archive,
 * the relay decision — happens on the bearer task in nw_drain(). Doing any of
 * it here is the mistake docs/esp32.md measured at 1 of 96 pings.
 */
static void nw_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data,
                       int len)
{
    if (!info || !data || len <= 0 || len > XPRSNOW_WIRE_MAX) return;
    /* Counted BEFORE the byte test, because "the radio heard nothing" and "the
     * radio heard something that was not ours" are different faults and every
     * counter downstream of here conflates them. */
    s_frames++;
    if (!xprs_looks_like(data, len)) { s_notxprs++; return; }
    nw_learn_phase();

    nw_frame_t f;
    memcpy(f.wire, data, (size_t)len);
    f.wire[len] = 0;
    f.len = len;
    f.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    if (info->src_addr) memcpy(f.mac, info->src_addr, 6);
    else                memset(f.mac, 0, 6);

    /* Never block the WiFi task. A full queue means the bearer task is behind,
     * and losing the frame is cheaper than stalling the radio for everybody. */
    if (s_rxq && xQueueSend(s_rxq, &f, 0) != pdTRUE) s_dropped++;
}

/* On the bearer task, once per tick. */
static void nw_drain(void *ctx)
{
    (void)ctx;
    if (!s_rxq) return;
    nw_frame_t f;
    while (xQueueReceive(s_rxq, &f, 0) == pdTRUE) {
        if (s_trace) {
            /* On the bearer task, not the WiFi task, and only while something
             * is asking. 56 bytes reaches past t:/f:/d:/r: on every packet the
             * choreography uses, which is all this needs to identify. */
            ESP_LOGW(TAG, "heard %d bytes at %d dBm: %.56s", f.len, f.rssi,
                     f.wire);
        }
        uint64_t peer = 0;
        for (int i = 0; i < 6; i++) peer = (peer << 8) | f.mac[i];
        xb_on_wire(s_now, f.wire, f.len, peer, f.rssi);
    }
    if (s_trace) {
        /* The frames that never reached the queue at all. Reported as a change,
         * because a total tells you nothing about the window you are watching. */
        static uint32_t seen_frames, seen_notxprs, seen_dropped;
        if (s_frames != seen_frames || s_notxprs != seen_notxprs ||
            s_dropped != seen_dropped) {
            ESP_LOGW(TAG, "radio delivered %u frame(s) (+%u), %u not XPRS (+%u), "
                          "%u dropped by a full queue (+%u)",
                     (unsigned)s_frames, (unsigned)(s_frames - seen_frames),
                     (unsigned)s_notxprs, (unsigned)(s_notxprs - seen_notxprs),
                     (unsigned)s_dropped, (unsigned)(s_dropped - seen_dropped));
            seen_frames = s_frames;
            seen_notxprs = s_notxprs;
            seen_dropped = s_dropped;
        }
    }
}

static void nw_rx_shim(const char *wire, int len, uint64_t peer, int rssi)
{
    if (!s_rx_cb) return;
    uint8_t mac[6];
    for (int i = 5; i >= 0; i--) { mac[i] = (uint8_t)(peer & 0xFF); peer >>= 8; }
    s_rx_cb(wire, len, mac, rssi);
}

static const xb_ops_t k_now_ops = {
    .air = nw_air,
    .now_ms = nw_now_ms,
    .random = nw_random,
    .lock = nw_lock,
    .unlock = nw_unlock,
    .drain = nw_drain,
    .ctx = NULL,
    .name = "espnow",
};

/* ── The public bearer ──────────────────────────────────────────────────── */

void xprsnow_offer(const char *wire, int len) { xb_offer(s_now, wire, len); }
void xprsnow_digipeat(const char *wire, int len) { xb_digipeat(s_now, wire, len); }
void xprsnow_echo(const char *wire, int len)
{
    xb_echo(s_now, wire, len);
}

uint32_t xprsnow_idle_ms(uint32_t now_ms)
{
    return xb_idle_ms(s_now, now_ms);
}

bool xprsnow_send(const char *wire, int len)  { return xb_send(s_now, wire, len); }
bool xprsnow_is_active(void)                  { return xb_is_active(s_now); }

void xprsnow_set_rx_cb(xprsnow_rx_cb_t cb)
{
    s_rx_cb = cb;
    xb_set_rx_cb(s_now, cb ? nw_rx_shim : NULL);
}
void xprsnow_set_heard_cb(xprsnow_heard_cb_t cb) { xb_set_heard_cb(s_now, cb); }

void xprsnow_set_beacon(xprsnow_beacon_cb_t cb, uint32_t interval_sec,
                        uint32_t first_delay_sec)
{
    xb_set_beacon(s_now, cb, interval_sec, first_delay_sec);
}

int xprsnow_peer_count(uint32_t max_age_sec)
{
    return xb_peer_count(s_now, max_age_sec);
}

uint8_t xprsnow_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return 0;
    return primary;
}

void xprsnow_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                   uint32_t *dropped)
{
    xb_stats(s_now, rx, tx, cancelled);
    if (dropped) *dropped = s_dropped;
}

void xprsnow_tx_stats(uint32_t *issued, uint32_t *done, uint32_t *failed)
{
    if (issued) *issued = s_tx_issued;
    if (done)   *done   = s_tx_done;
    if (failed) *failed = s_tx_failed;
}

esp_err_t xprsnow_start(const char *callsign)
{
    if (s_started) return ESP_OK;

    if (!s_now) s_now = xprs_big_calloc(1, sizeof *s_now);
    if (!s_now) return ESP_ERR_NO_MEM;

    s_rxq = xQueueCreate(XPRSNOW_RX_QUEUE, sizeof(nw_frame_t));
    if (!s_rxq) return ESP_ERR_NO_MEM;
    if (!s_tx_mtx) s_tx_mtx = xSemaphoreCreateMutex();
    if (!s_tx_mtx) { vQueueDelete(s_rxq); s_rxq = NULL; return ESP_ERR_NO_MEM; }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_init failed: %s — is WiFi started?",
                 esp_err_to_name(err));
        vQueueDelete(s_rxq); s_rxq = NULL;
        return err;
    }

    /* One peer, ever: the broadcast address. channel 0 means "whatever channel
     * the station is on", which is the only channel we are allowed to use while
     * associated — see the header. Broadcast is never encrypted. */
    esp_now_peer_info_t peer = {
        .channel = 0,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, k_broadcast, 6);
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "broadcast peer refused: %s", esp_err_to_name(err));
        esp_now_deinit();
        vQueueDelete(s_rxq); s_rxq = NULL;
        return err;
    }

    /* The long-range PHY for everything this station sends. Receiving it
     * needs LR in the interface's protocols, which wifi_up() set
     * (xprsnow_wifi_prepare). */
    if (s_lr) {
        esp_now_rate_config_t rc = {
            .phymode = WIFI_PHY_MODE_LR,
            .rate = WIFI_PHY_RATE_LORA_250K,
        };
        esp_err_t re = esp_now_set_peer_rate_config(k_broadcast, &rc);
        if (re != ESP_OK)
            ESP_LOGW(TAG, "long-range rate refused: %s; sending at 1 Mbps",
                     esp_err_to_name(re));
    }

    esp_now_register_recv_cb(nw_recv_cb);
    esp_now_register_send_cb(nw_send_cb);

    if (!s_txq) s_txq = xprs_big_calloc(NW_TXQ, sizeof *s_txq);
    if (s_txq && !s_win_task)
        xTaskCreate(nw_window_task, "nowwin", 3072, NULL, 6, &s_win_task);
    if (!s_win_task)
        ESP_LOGW(TAG, "no window task: frames go out at once, and stations on "
                      "a router hear them only when they share our channel");

    /* A station that modem-sleeps misses ESP-NOW frames while it is asleep.
     * This costs power and takes a share of the airtime back from Bluetooth,
     * which is a trade this board has to be measured against, not assumed. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_started = true;
    xb_init(s_now, &k_now_ops, callsign);
    xb_register_ticked(s_now);

    /* This bearer does not own a task — it is pumped by whoever does. If nobody
     * has claimed that job, nothing here will ever re-air or beacon, and the
     * failure would be silent: frames arrive, the queue drains never, the peer
     * count sits at zero. Say it now, loudly, instead. */
    if (!xb_has_driver()) {
        ESP_LOGE(TAG, "no bearer task is running — ESP-NOW will receive nothing "
                      "and beacon never (start the LAN bearer, or give this one "
                      "a task)");
    }

    ESP_LOGI(TAG, "XPRS over ESP-NOW up: home channel %u (now on %u), %s, "
                  "window %ums a second, callsign %s",
             s_home_ch, xprsnow_channel(), s_lr ? "long range 250 kbps" : "1 Mbps",
             (unsigned)s_win_ms, callsign ? callsign : "");
    return ESP_OK;
}

void xprsnow_stop(void)
{
    if (!s_started) return;
    xb_stop(s_now);
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    s_started = false;
}

/* Between esp_wifi_init() and esp_wifi_start(): the country (channel 13 is
 * not in the chip's default "world safe" set), and LR among the station
 * interface's protocols so this radio hears the long-range PHY. Returns the
 * home channel actually usable: the configured one, or 11 where the
 * country has no 13. */
uint8_t xprsnow_wifi_prepare(const char *country)
{
    if (country && country[0]) {
        esp_err_t e = esp_wifi_set_country_code(country, true);
        if (e != ESP_OK)
            ESP_LOGW(TAG, "country \"%s\" refused: %s", country, esp_err_to_name(e));
    }
    wifi_country_t c;
    if (esp_wifi_get_country(&c) == ESP_OK &&
        s_home_ch > c.schan + c.nchan - 1) {
        ESP_LOGW(TAG, "channel %u is not allowed in \"%.2s\": home channel 11",
                 s_home_ch, c.cc);
        s_home_ch = 11;
    }
    if (s_lr) {
        esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA,
            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
            WIFI_PROTOCOL_LR);
        if (e != ESP_OK)
            ESP_LOGW(TAG, "LR protocol refused: %s", esp_err_to_name(e));
    }
    return s_home_ch;
}
