/* xprslora.c -- see the header. Shaped on xprs_bearer_now, which is the
 * reference for how a radio hides behind xprs_bearer. */

#include "xprslora.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "mt.h"
#include "mt_mesh.h"
#include "sx1262.h"
#include "xprs.h"
#include "xprsbearer.h"
#include "xb_airtime.h"

static const char *TAG = "xprslora";

/* Heap, not BSS, and only once the radio is really there.
 *
 * An xb_t carries the re-air queue (eight 250-byte packets), two 32-slot
 * identifier rings and a peer table -- a few kilobytes that a board with no
 * SX1262 was paying for a bearer it could never start. On the M5Stack that
 * was the difference between an HTTP server that starts and one that
 * answers ESP_ERR_HTTPD_TASK. */
static xb_t *s_lora;
static xb_duty_t s_duty;               /* the ledger; ~150 B of BSS */
static xb_lora_air_t s_air;            /* what one byte costs here */
static const xprslora_region_t *s_region;
static uint32_t s_self;                /* our Meshtastic node number */

/*
 * The regions. The frequency is Meshtastic's LongFast slot (mt_slot_freq_hz,
 * checked against the published 869.525 / 906.875 / 919.875 MHz in the
 * host test). The EU budget is ERC 70-03 band g3: 10% of the hour, 500 mW
 * e.r.p. The reserve is ten full SF11 frames -- enough that an emergency is
 * never what a spent budget silences, small enough that it cannot BE the
 * budget. The 900 MHz rows carry no hourly budget and, since the move, no
 * dwell either: see the header.
 */
static const xprslora_region_t k_regions[] = {
    { "eu", 869525000u, 360000u, 21000u, 0, 27 },
    { "us", 906875000u,       0,     0, 0, 30 },
    { "au", 919875000u,       0,     0, 0, 30 },
};

const xprslora_region_t *xprslora_regions(int *count)
{
    if (count) *count = (int)(sizeof k_regions / sizeof k_regions[0]);
    return k_regions;
}

const xprslora_region_t *xprslora_region(void)
{
    return s_region ? s_region : &k_regions[0];
}

/* What an XPRS wire of [len] bytes costs here: its frame, or its two. */
static uint32_t lr_airtime(int len, void *ctx)
{
    (void)ctx;
    uint32_t ms = 0;
    for (int part = 0; part < mt_xprs_frames_for(len); part++)
        ms += xb_lora_airtime_ms(&s_air, mt_xprs_frame_len(len, part));
    return ms;
}

uint32_t xprslora_airtime_ms(int len)
{
    return s_air.bw_hz ? lr_airtime(len, NULL) : 0;
}

static sx1262_handle_t s_radio;
static SemaphoreHandle_t s_mutex;      /* several tasks air on one radio */
static SemaphoreHandle_t s_mt_mutex;   /* the bridge's state; recursive */
static xprslora_rx_cb_t s_rx_cb;

/* The bridge's state, and the buffers the radio path needs. One heap block
 * claimed at start (PSRAM where the board has it); the SPI buffers below
 * stay internal, because the SPI master DMAs out of them. */
typedef struct {
    mt_mesh_t  mesh;
    mt_reasm_t reasm;
    bool       mesh_on;
} lr_state_t;
static lr_state_t *s_st;

static uint8_t s_rxbuf[MT_FRAME_MAX + 1];
static uint8_t s_txbuf[MT_FRAME_MAX];
static uint8_t s_frames[2][MT_FRAME_MAX];
static char    s_wire[XB_WIRE_MAX + 1];
static uint32_t s_hdr_since;           /* first saw "a header is arriving" */
static uint32_t s_cad_busy, s_cad_waits;

/* The DIO1 interrupt only raises this flag; every SPI byte moves on the
 * bearer task in lr_drain(). An ISR that touched the bus would collide with
 * whatever transfer the display has in flight. */
static volatile bool s_rx_pending;

static void lr_rx_isr(void *user)
{
    (void)user;
    s_rx_pending = true;
}

static uint32_t lr_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t lr_random(void) { return esp_random(); }
static void lr_lock(void *ctx)   { (void)ctx; xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void lr_unlock(void *ctx) { (void)ctx; xSemaphoreGive(s_mutex); }

/* ── The transmitter ─────────────────────────────────────────────────────
 *
 * Non-blocking: sx1262_tx_start() returns as soon as the frame is in the
 * FIFO and the bearer task goes back to pumping the LAN, ESP-NOW and BLE.
 * lr_tx_service() notices the end on a later tick and puts the radio back
 * in receive. Radio lock held by every caller. */

static void lr_listen(void)
{
    sx1262_start_receive(s_radio, lr_rx_isr, NULL);
}

static void lr_tx_service(void)
{
    if (!s_radio || !sx1262_tx_active(s_radio)) return;
    int r = sx1262_tx_poll(s_radio);
    if (r == 0) return;
    if (r < 0) ESP_LOGW(TAG, "TX failed");
    /* Back to listening either way -- a radio parked in standby after a
     * failed send is deaf and looks exactly like an empty band. */
    lr_listen();
}

/* Wait out a transmission already on the air (two fragments back to back,
 * or an XPRS packet behind a Meshtastic relay). Bounded by the longest
 * frame there is. */
static void lr_tx_wait_idle(void)
{
    for (int i = 0; i < 300 && sx1262_tx_active(s_radio); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lr_tx_service();
    }
}

/* Is the channel taken? A header already arriving, or channel activity
 * detection -- what Meshtastic checks, on the one EU slot both use. */
static bool lr_channel_busy(void)
{
    uint16_t irq = sx1262_irq_status(s_radio);
    uint32_t now = lr_now_ms();
    if (irq & SX1262_IRQ_HEADER_OK) {
        if (!s_hdr_since) s_hdr_since = now ? now : 1;
        /* A latch older than the longest frame is left over from a packet
         * that never completed; clear it rather than wait on it forever. */
        if (now - s_hdr_since < 2500) return true;
        sx1262_irq_clear(s_radio, SX1262_IRQ_HEADER_OK | SX1262_IRQ_PREAMBLE);
    }
    s_hdr_since = 0;
    bool busy = false;
    if (sx1262_cad(s_radio, &busy) != ESP_OK) busy = false;
    if (busy) s_cad_busy++;
    return busy;
}

static bool lr_start(const uint8_t *frame, int len)
{
    memcpy(s_txbuf, frame, (size_t)len);
    /* The watchdog is the radio's own, not an expectation: 255 bytes at
     * SF11/250 kHz is 2.1 s. */
    esp_err_t err = sx1262_tx_start(s_radio, s_txbuf, (uint8_t)len, 4000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX start failed: %s", esp_err_to_name(err));
        lr_listen();
        return false;
    }
    return true;
}

/* An XPRS wire, wrapped. Waits for a clear channel: the xb pump that calls
 * this has already charged the ledger and dropped the packet from its
 * queue, so "not now" is not an answer it can take. */
static bool lr_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_radio || len <= 0 || len > XB_WIRE_MAX) return false;
    int fl[2];
    int n = mt_xprs_wrap(wire, len, s_self, s_frames, fl);
    if (!n) {
        ESP_LOGW(TAG, "not wrappable: %.40s", wire);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        lr_tx_wait_idle();
        /* Up to eight tries a slot or more apart, then go anyway: a packet
         * that waited out two seconds of somebody else's traffic has been
         * polite enough, and one that never leaves is lost. */
        for (int t = 0; t < 8 && lr_channel_busy(); t++) {
            s_cad_waits++;
            lr_listen();
            vTaskDelay(pdMS_TO_TICKS(mt_mesh_slot_ms() * (1 + esp_random() % 8)));
        }
        /* No bridge lock here: this runs under the radio lock, and the
         * bridge takes the two the other way round (mt tick, then air). */
        ok = lr_start(s_frames[i], fl[i]);
    }
    return ok;
}

/* A Meshtastic frame from the bridge: the ledger first, then the channel.
 * "Not now" is an answer here -- the bridge keeps the frame and asks again. */
static bool lr_air_mt(void *ctx, const uint8_t *frame, int len, int prio)
{
    (void)ctx;
    if (!s_radio || !s_lora || len <= 0 || len > MT_FRAME_MAX) return false;
    lr_lock(NULL);
    bool ok = false;
    if (!sx1262_tx_active(s_radio) && !lr_channel_busy()) {
        uint32_t ms = xb_lora_airtime_ms(&s_air, len);
        if (xb_spend(s_lora, ms, prio >= MT_PRIO_URGENT)) {
            ok = lr_start(frame, len);
            mt_hdr_t h;
            if (ok && mt_hdr_parse(frame, len, &h))
                ESP_LOGI(TAG, "mt tx %08lx>%08lx id %08lx hop %d/%d ch %02x %dB",
                         (unsigned long)h.from, (unsigned long)h.to,
                         (unsigned long)h.id, h.hop_limit, h.hop_start,
                         h.channel, len);
        } else {
            lr_listen();
        }
    } else if (!sx1262_tx_active(s_radio)) {
        lr_listen();                     /* CAD left it in standby */
    }
    lr_unlock(NULL);
    return ok;
}

/* ── The receiver ────────────────────────────────────────────────────── */

static void lr_mt_tick(void)
{
    if (!s_st || !s_st->mesh_on) return;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    mt_mesh_tick(&s_st->mesh, lr_now_ms());
    xSemaphoreGiveRecursive(s_mt_mutex);
}

/* On the bearer task, once per tick: finish a transmission, fetch what the
 * interrupt announced, and let the bridge air what is due. */
static void lr_drain(void *ctx)
{
    (void)ctx;
    if (!s_radio || !s_lora) return;
    lr_lock(NULL);
    lr_tx_service();
    lr_unlock(NULL);

    if (s_rx_pending && !sx1262_tx_active(s_radio)) {
        s_rx_pending = false;
        sx1262_rx_info_t info;
        lr_lock(NULL);
        esp_err_t got = sx1262_get_packet(s_radio, s_rxbuf, MT_FRAME_MAX, &info);
        lr_unlock(NULL);
        if (got == ESP_OK && info.len > 0) {
            int n = mt_xprs_unwrap(s_st ? &s_st->reasm : NULL, s_rxbuf,
                                   info.len, lr_now_ms(), s_wire, sizeof s_wire);
            if (n > 0 && xprs_looks_like((const uint8_t *)s_wire, n)) {
                /* Before the dupe rings swallow it: the one log line that
                 * proves a packet crossed on RF rather than on WiFi, with
                 * the RSSI only a radio has. */
                ESP_LOGI(TAG, "RX %d bytes at %d dBm SNR %d: %.48s", n,
                         info.rssi, info.snr, s_wire);
                if (s_st) {
                    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
                    mt_mesh_note_xprs_frame(&s_st->mesh, s_rxbuf, info.len);
                    xSemaphoreGiveRecursive(s_mt_mutex);
                }
                xb_on_wire(s_lora, s_wire, n, 0, info.rssi);
            } else if (n < 0) {
                mt_hdr_t h;
                if (mt_hdr_parse(s_rxbuf, info.len, &h))
                    ESP_LOGI(TAG, "mt %08lx>%08lx id %08lx hop %d/%d ch %02x %uB %d dBm",
                             (unsigned long)h.from, (unsigned long)h.to,
                             (unsigned long)h.id, h.hop_limit, h.hop_start,
                             h.channel, (unsigned)info.len, info.rssi);
                if (s_st && s_st->mesh_on) {
                    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
                    mt_mesh_on_frame(&s_st->mesh, s_rxbuf, info.len,
                                     info.rssi, info.snr);
                    xSemaphoreGiveRecursive(s_mt_mutex);
                }
            }
            /* n == 0: a fragment waiting for its sibling. */
        }
        /* The radio stays in continuous receive after a packet. */
    }
    lr_mt_tick();

    /* This task pumps every bearer, and the bridge added X25519 (about
     * 1.3 KB deep) to what it runs. Say so each time the margin shrinks, so
     * the headroom is a measured number (docs/esp32.md, task stacks). */
    static UBaseType_t s_low = (UBaseType_t)-1;
    UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
    if (hw + 128 < s_low || (s_low == (UBaseType_t)-1)) {
        s_low = hw;
        ESP_LOGI(TAG, "bearer task: %u bytes of stack never used",
                 (unsigned)hw);
    }
}

static void lr_rx_shim(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    if (s_rx_cb) s_rx_cb(wire, len, rssi);
}

static const xb_ops_t k_lora_ops = {
    .air = lr_air,
    .now_ms = lr_now_ms,
    .random = lr_random,
    .lock = lr_lock,
    .unlock = lr_unlock,
    .drain = lr_drain,
    .ctx = NULL,
    .name = "lora",
};

/* ── The public bearer ──────────────────────────────────────────────────── */

esp_err_t xprslora_start(const char *callsign, const xprslora_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_radio) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    s_mt_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex || !s_mt_mutex) return ESP_ERR_NO_MEM;
    s_lora = calloc(1, sizeof *s_lora);
    if (!s_lora) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    sx1262_spi_config_t spi = {
        .mosi_pin = cfg->mosi_pin,
        .miso_pin = cfg->miso_pin,
        .sck_pin = cfg->sck_pin,
        .cs_pin = cfg->cs_pin,
        .rst_pin = cfg->rst_pin,
        .busy_pin = cfg->busy_pin,
        .dio1_pin = cfg->dio1_pin,
    };
    esp_err_t err = sx1262_create(&spi, &s_radio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no radio: %s", esp_err_to_name(err));
        return err;
    }

    s_region = &k_regions[0];
    if (cfg->region && cfg->region[0])
        for (size_t i = 0; i < sizeof k_regions / sizeof k_regions[0]; i++)
            if (strcasecmp(k_regions[i].name, cfg->region) == 0)
                s_region = &k_regions[i];

    /* LongFast, which every Meshtastic node in the region shares. The two
     * ends of a link agree by construction because both run these lines. */
    sx1262_lora_config_t lora = {
        .frequency_hz = cfg->freq_hz ? cfg->freq_hz : s_region->freq_hz,
        .sf = SX1262_SF11,
        .bw = SX1262_BW_250,
        .cr = SX1262_CR_4_5,
        .tx_power_dbm = cfg->tx_power_dbm ? cfg->tx_power_dbm : 14,
        .preamble_len = MT_LF_PREAMBLE,
        .crc_on = true,
        .use_tcxo = cfg->use_tcxo,
        .use_dio2_rf_switch = cfg->use_dio2_rf_switch,
        .sync_word = MT_LF_SYNC,
    };
    /* The airtime table is built from the SAME values the radio was just
     * given, so the ledger cannot drift from the modem. */
    s_air = (xb_lora_air_t){ .bw_hz = MT_LF_BW_HZ, .sf = MT_LF_SF,
                             .cr = MT_LF_CR, .preamble = MT_LF_PREAMBLE,
                             .crc = true, .implicit_header = false };
    err = sx1262_init(s_radio, &lora);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed: %s", esp_err_to_name(err));
        sx1262_delete(s_radio);
        s_radio = NULL;
        return err;
    }
    s_self = mt_node_of_call(callsign, (int)strlen(callsign));

    xb_init(s_lora, &k_lora_ops, callsign);
    /* section 31.1. Set before anything can be offered, so the radio is never
     * unmetered even for the first packet after boot -- the pace as the
     * collision spacer, the ledger as the accountant. */
    xb_set_pace(s_lora, XPRSLORA_PACE_DEFAULT_MS);
    xb_set_duty(s_lora, &s_duty, lr_airtime, NULL,
                s_region->duty_ms, s_region->reserve_ms, s_region->dwell_ms);
    xb_register_ticked(s_lora);
    if (!xb_has_driver())
        ESP_LOGE(TAG, "no bearer task is pumping -- start the LAN bearer "
                      "first, or nothing will ever leave this radio");

    err = sx1262_start_receive(s_radio, lr_rx_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receive mode failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "up: %lu Hz LongFast (SF11/250k, sync 0x2B) %d dBm as %s "
             "(node %08lx) -- %s: %lus of airtime an hour, %lus reserved",
             (unsigned long)lora.frequency_hz, lora.tx_power_dbm, callsign,
             (unsigned long)s_self, s_region->name,
             (unsigned long)(s_region->duty_ms / 1000u),
             (unsigned long)(s_region->reserve_ms / 1000u));
    if (lora.tx_power_dbm > s_region->max_dbm)
        ESP_LOGW(TAG, "%d dBm exceeds the %s region's %d dBm e.r.p. ceiling"
                 " -- the operator owns that call", lora.tx_power_dbm,
                 s_region->name, s_region->max_dbm);
    return ESP_OK;
}

void xprslora_set_rx_cb(xprslora_rx_cb_t cb)
{
    s_rx_cb = cb;
    xb_set_rx_cb(s_lora, cb ? lr_rx_shim : NULL);
}

bool xprslora_send(const char *wire, int len)
{
    return s_lora && xb_send(s_lora, wire, len);
}
void xprslora_offer(const char *wire, int len)
{
    if (s_lora) xb_offer(s_lora, wire, len);
}
void xprslora_digipeat(const char *wire, int len)
{
    if (s_lora) xb_digipeat(s_lora, wire, len);
}
void xprslora_echo(const char *wire, int len)
{
    if (s_lora) xb_echo(s_lora, wire, len);
}

uint32_t xprslora_idle_ms(uint32_t now_ms)
{
    return s_lora ? xb_idle_ms(s_lora, now_ms) : 0xFFFFFFFFu;
}

void xprslora_set_pace(uint32_t per_packet_ms)
{
    if (s_lora) xb_set_pace(s_lora, per_packet_ms);
}

uint32_t xprslora_owed_ms(void)
{
    return s_lora ? xb_owed_ms(s_lora) : 0;
}

void xprslora_set_duty(uint32_t budget_ms, uint32_t reserve_ms,
                       uint32_t dwell_ms)
{
    if (s_lora)
        xb_set_duty(s_lora, &s_duty, lr_airtime, NULL,
                    budget_ms, reserve_ms, dwell_ms);
}

void xprslora_duty(xb_duty_report_t *out)
{
    if (!out) return;
    if (s_lora) xb_duty_report(s_lora, lr_now_ms(), out);
    else memset(out, 0, sizeof *out);
}

bool xprslora_is_active(void)
{
    return s_radio && s_lora && xb_is_active(s_lora);
}

void xprslora_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                    uint32_t *dupes)
{
    if (!s_lora) {
        if (rx) *rx = 0;
        if (tx) *tx = 0;
        if (cancelled) *cancelled = 0;
        if (dupes) *dupes = 0;
        return;
    }
    if (rx) *rx = s_lora->rx_count;
    if (tx) *tx = s_lora->tx_count;
    if (cancelled) *cancelled = s_lora->cancelled;
    if (dupes) *dupes = s_lora->dupes;
}

/* ── Meshtastic ──────────────────────────────────────────────────────── */

static xprslora_mt_hooks_t s_hooks;

static void mt_deliver(void *ctx, const char *wire, int len, bool sign)
{
    (void)ctx;
    if (s_hooks.deliver) s_hooks.deliver(wire, len, sign);
}

static int mt_stamp(void *ctx, char *out, int cap, bool to_minute)
{
    (void)ctx;
    return s_hooks.stamp ? s_hooks.stamp(out, cap, to_minute) : 0;
}

static bool mt_nick(void *ctx, const char *call, char *out, int cap)
{
    (void)ctx;
    return s_hooks.nick_of && s_hooks.nick_of(call, out, cap);
}

static void mt_log(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
}

/* SNTP's clock, once it has one: before that the bridge cannot tell old
 * news from new and lets everything through. */
static uint32_t mt_utc(void *ctx)
{
    (void)ctx;
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;
}

/* What the bridge keeps across a restart, one NVS blob each: the Meshtastic
 * keys it learned ("keys") and the XPRS callsigns it speaks for ("vnodes").
 * Written at most once a minute and only when something is new
 * (mt_mesh_tick), because a flash write stops the cache on both cores. */
static int mt_blob_load(const char *key, void *buf, int cap)
{
    nvs_handle_t h;
    if (nvs_open("xprsmt", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t n = (size_t)cap;
    esp_err_t err = nvs_get_blob(h, key, buf, &n);
    nvs_close(h);
    return err == ESP_OK ? (int)n : 0;
}

static void mt_blob_save(const char *key, const void *buf, int len)
{
    nvs_handle_t h;
    if (nvs_open("xprsmt", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, key, buf, (size_t)len) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static int mt_keys_load(void *ctx, void *buf, int cap)
{
    (void)ctx;
    return mt_blob_load("keys", buf, cap);
}

static void mt_keys_save(void *ctx, const void *buf, int len)
{
    (void)ctx;
    mt_blob_save("keys", buf, len);
}

static int mt_vnodes_load(void *ctx, void *buf, int cap)
{
    (void)ctx;
    return mt_blob_load("vnodes", buf, cap);
}

static void mt_vnodes_save(void *ctx, const void *buf, int len)
{
    (void)ctx;
    mt_blob_save("vnodes", buf, len);
}

esp_err_t xprslora_mt_start(const xprslora_mt_hooks_t *hooks,
                            const mt_mesh_cfg_t *cfg, const char *nick)
{
    if (!s_lora || !hooks || !cfg) return ESP_ERR_INVALID_STATE;
    if (s_st) return ESP_OK;
    lr_state_t *st = heap_caps_calloc(1, sizeof *st,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) st = calloc(1, sizeof *st);
    if (!st) {
        ESP_LOGE(TAG, "no room for the Meshtastic bridge (%u bytes) -- "
                      "XPRS still runs on this radio, Meshtastic does not",
                 (unsigned)sizeof *st);
        return ESP_ERR_NO_MEM;
    }
    s_hooks = *hooks;
    mt_mesh_ops_t ops = {
        .air = lr_air_mt, .now_ms = lr_now_ms, .random = lr_random,
        .deliver = mt_deliver, .stamp = mt_stamp, .nick_of = mt_nick,
        .log = mt_log, .keys_load = mt_keys_load, .keys_save = mt_keys_save,
        .vnodes_load = mt_vnodes_load, .vnodes_save = mt_vnodes_save,
        .utc_now = mt_utc,
        .ctx = NULL,
    };
    mt_mesh_init(&st->mesh, &ops, cfg, s_lora->call, nick);
    st->mesh_on = true;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    s_st = st;
    xSemaphoreGiveRecursive(s_mt_mutex);
    ESP_LOGI(TAG, "Meshtastic: node %08lx, repeater %s, bridge %s, %u "
                  "broadcasts an hour, %u bytes %s",
             (unsigned long)s_self, cfg->repeat ? "on" : "off",
             cfg->bridge ? "on" : "off", (unsigned)cfg->bcast_per_hour,
             (unsigned)sizeof *st,
             esp_ptr_external_ram(st) ? "in PSRAM" : "internal");
    return ESP_OK;
}

void xprslora_mt_offer(const char *wire, int len, int origin)
{
    if (!s_st || !s_st->mesh_on) return;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    mt_mesh_on_xprs(&s_st->mesh, wire, len, origin);
    xSemaphoreGiveRecursive(s_mt_mutex);
}

bool xprslora_mt_stats(mt_mesh_stats_t *out)
{
    if (!s_st || !out) return false;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    *out = s_st->mesh.st;
    xSemaphoreGiveRecursive(s_mt_mutex);
    return true;
}

int xprslora_mt_node(int i, mt_node_t *out)
{
    if (!s_st) return 0;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    const mt_node_t *n = NULL;
    int count = mt_mesh_node(&s_st->mesh, i, &n);
    if (n && out) *out = *n;
    xSemaphoreGiveRecursive(s_mt_mutex);
    return count;
}

void xprslora_mt_set_nick(const char *nick)
{
    if (!s_st) return;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    mt_mesh_set_nick(&s_st->mesh, nick);
    xSemaphoreGiveRecursive(s_mt_mutex);
}
