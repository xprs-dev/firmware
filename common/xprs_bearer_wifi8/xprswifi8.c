/*
 * XPRS over XPRS-WiFi — see xprswifi8.h for the bearer and the time-slice that
 * shares the one radio with 802.11. The queue, the identifier rings and the
 * §13.2.1 cancel are xprs_bearer, shared with the LAN and ESP-NOW; what is here
 * is the radio (through the xwifi station) and the scheduler that lends it out.
 *
 * Unlike ESP-NOW, nothing here is instant. air() hands a whole packet to the
 * station and the task plays it out over seconds, a block at a time, inside the
 * slots the scheduler opens. So this file owns a task; the ESP-NOW bearer did
 * not, because its air() finished on the caller's thread.
 */

#include "xprswifi8.h"
#include "xprsbearer.h"
#include "xprs.h"
#include "xprs_psram.h"   /* xprs_big_calloc: the bearer state off the .bss */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"

#include "xwifi.h"
#if CONFIG_IDF_TARGET_ESP32C3
#include "xw_port_esp32c3.h"
#include "heap_memory_layout.h"
#define RADIO_SUSPEND xw_esp32c3_radio_suspend
#define RADIO_RESUME  xw_esp32c3_radio_resume
#define RADIO_START   xw_esp32c3_radio_start
#define PORT_INIT(port, amp, looks) c3_port_init(port, amp, looks)
#elif CONFIG_IDF_TARGET_ESP32S3
#include "xw_port_esp32s3.h"
#define RADIO_SUSPEND xw_esp32s3_radio_suspend
#define RADIO_RESUME  xw_esp32s3_radio_resume
#define RADIO_START   xw_esp32s3_radio_start
#define PORT_INIT(port, amp, looks) s3_port_init(port, amp, looks)
#endif

static const char *TAG = "xprswifi8";

/* The mode every outbound packet takes. 3s is the balance the README measures:
 * near-100 % delivery at 33..250 bytes, a packet in hand about 1 s in, against
 * 1.5s (misses long packets on a window receiver) and 12s (6.7 s to deliver).
 * Not yet a per-packet choice — §6.5's mode logic is xw_choose_mode's, and
 * wiring the peer table into it is a later step. */
#define TX_MODE  XW_MODE_3S

typedef struct {
    char wire[XPRSWIFI8_WIRE_MAX + 1];
    int  len;
} tx_item_t;

typedef struct {
    char wire[XPRSWIFI8_WIRE_MAX + 1];
    int  len;
    int  snr_db;
} rx_item_t;

/* Was a 3.4 KB static; xprs_big_calloc'd in start so the .bss stays under
 * the RF-bank ceiling (xb_* entries are NULL-safe before start). */
static xb_t             *s_bearer;
static xprswifi8_cfg_t   s_cfg;

/*
 * The station state is ~58 KB (window, demodulator, reassembly) and the chips
 * that carry this bearer cannot spare that much .bss: their static data has to
 * stay clear of the RF sample banks, and a full XPRS station already fills what
 * is left. So the state lives at a fixed address instead of in .bss:
 *
 *   ESP32-S3: in RF bank 2 (0x3fcd0000, 64 KB). The port's engines use bank k
 *   per look for receiving and banks 0/1 for transmitting, and the S3 hands
 *   banks to the RF block one bit each (BANK_OWNER) — with looks <= 2, bank 2
 *   stays the CPU's at all times. start() clamps looks to 2 for this reason.
 *   The bank is inside the port's existing SOC_RESERVE, so the heap stays off.
 *
 *   ESP32-C3: just above the port's bank (0x3fcc0000), under its own reserve
 *   below — the C3's one bank has no idle half (ring in the upper, the port's
 *   staging and second-look window in the lower).
 *
 * xw_station_init memsets the struct, so placement needs no zeroed .bss.
 */
#if CONFIG_IDF_TARGET_ESP32S3
static xw_station_t *const s_sta = (xw_station_t *)0x3fcd0000;
/* The task's stack lives in the bank's tail, after the station: the chips
 * that need this bearer have no 8 KB of contiguous heap left by the time it
 * starts, and bank 2 is the CPU's at all times with looks <= 2. */
#define S3_STACK_OFF   ((sizeof(xw_station_t) + 15u) & ~15u)
#define S3_STACK_SIZE  (((0x10000u - S3_STACK_OFF) & ~15u) - 16u)
static StackType_t *const s_task_stack = (StackType_t *)(0x3fcd0000 + S3_STACK_OFF);
static StaticTask_t s_task_tcb;
_Static_assert(sizeof(xw_station_t) <= 0x10000, "xw_station_t outgrew RF bank 2");
_Static_assert(0x10000 - ((sizeof(xw_station_t) + 15) & ~15) >= 4096,
               "no room for the task stack behind the station in bank 2");
#elif CONFIG_IDF_TARGET_ESP32C3
SOC_RESERVE_MEMORY_REGION(0x3fcc0000, 0x3fcc0000 + sizeof(xw_station_t), xwifi_station_state);
static xw_station_t *const s_sta = (xw_station_t *)0x3fcc0000;
#else
static xw_station_t      s_station;    /* no bank layout known for this chip */
static xw_station_t *const s_sta = &s_station;
#endif
static xw_port_t         s_port;
static TaskHandle_t      s_task;
static QueueHandle_t     s_txq;         /* whole packets waiting for a slot */
static QueueHandle_t     s_rxq;         /* complete packets the station surfaced */
static SemaphoreHandle_t s_tx_mtx;      /* air() vs the task, around xw_station_send */
static volatile bool     s_started;
static volatile bool     s_run;
static bool              s_trace;

static volatile uint32_t s_tx_issued, s_tx_done, s_tx_failed;
static uint32_t          s_dropped;     /* rx packets with nowhere to go */
static xprswifi8_rx_cb_t s_rx_cb;

/* ── The clock and the jitter xprs_bearer runs on ───────────────────────── */

static uint32_t p_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t p_random(void) { return esp_random(); }
static void p_lock(void *ctx)   { (void)ctx; if (s_tx_mtx) xSemaphoreTake(s_tx_mtx, portMAX_DELAY); }
static void p_unlock(void *ctx) { (void)ctx; if (s_tx_mtx) xSemaphoreGive(s_tx_mtx); }

/*
 * xprs_bearer calls this to put a packet on the medium. On XPRS-WiFi that takes
 * seconds, so we do NOT block the caller: queue the whole packet and let the
 * task play it out in the slots. Returns true once it is accepted, false when
 * the queue is full — the same contract ESP-NOW's asynchronous air() has, only
 * the latency is seconds not milliseconds.
 */
static bool p_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_started || len <= 0 || len > XPRSWIFI8_WIRE_MAX) return false;
    tx_item_t it;
    memcpy(it.wire, wire, (size_t)len);
    it.wire[len] = 0;
    it.len = len;
    if (!s_txq || xQueueSend(s_txq, &it, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full (%d packets already waiting a slot); dropped %d bytes",
                 XPRSWIFI8_TX_QUEUE, len);
        return false;
    }
    s_tx_issued++;
    /* wake the task so a send does not wait out the receive period */
    if (s_task) xTaskNotifyGive(s_task);
    return true;
}

/* ── Receiving: the station hands up complete packets ────────────────────── */

/*
 * Called by xw_station_poll (on the bearer task — xwifi has no WiFi-task
 * callback of its own, unlike ESP-NOW) with every COMPLETE packet the window
 * demodulator reassembled. Copy into the rx queue; the drain turns it into
 * xb_on_wire on this same task, so the SHA-256 the §5 identifier costs is not
 * on any radio-critical path.
 */
static void on_packet(void *ctx, const uint8_t *packet, size_t len, uint8_t channel,
                      const xw_hit_t *hit)
{
    (void)ctx; (void)channel;
    if (!packet || len == 0 || len > XPRSWIFI8_WIRE_MAX) return;
    if (!xprs_looks_like(packet, (int)len)) return;
    rx_item_t it;
    memcpy(it.wire, packet, len);
    it.wire[len] = 0;
    it.len = (int)len;
    it.snr_db = hit ? hit->snr_db : 0;
    if (!s_rxq || xQueueSend(s_rxq, &it, 0) != pdTRUE) s_dropped++;
}

static void drain_rx(void *ctx)
{
    (void)ctx;
    if (!s_rxq) return;
    rx_item_t it;
    while (xQueueReceive(s_rxq, &it, 0) == pdTRUE) {
        if (s_trace)
            ESP_LOGW(TAG, "heard %d bytes at ~%d dB: %.56s", it.len, it.snr_db, it.wire);
        /* XPRS-WiFi has no sender address; peer 0 means "the medium cannot say",
         * which is exactly xb_on_wire's contract for a radio with no MAC. The
         * SNR stands in for RSSI: both are "how well we heard it" for §13.2.1. */
        xb_on_wire(s_bearer, it.wire, it.len, 0, it.snr_db);
    }
}

static void rx_shim(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    if (s_rx_cb) s_rx_cb(wire, len, rssi);
}

/* ── The task: slots, and the station poll loop inside them ──────────────── */

/*
 * One slot: borrow the radio from 802.11, poll the station until the slot's
 * time is spent or a transmission in progress finishes, then give it back.
 *
 * xw_station_poll does one unit of work per call — the next block if a
 * transmission is in progress, otherwise one receive window — and returns true
 * while transmitting. So a slot is "poll until slot_ms is up", and a
 * transmission longer than one slot simply continues in the next one: blocks
 * are independent (§6.4), and receivers reassemble from whatever pieces arrive.
 */
static void run_slot(uint32_t budget_ms)
{
    const bool slice = s_cfg.slot_ms > 0;   /* 0: XPRS-WiFi owns the radio, never hand it back */
    if (slice && RADIO_SUSPEND() != 0) {
        /* could not take the radio from 802.11 this time — skip the slot rather
         * than fight the driver for it; the next slot tries again */
        return;
    }
    const uint32_t start = p_now_ms();
    bool was_tx = false;
    do {
        p_lock(NULL);
        const bool tx = xw_station_poll(s_sta);
        p_unlock(NULL);
        if (was_tx && !tx) { s_tx_done++; }   /* a transmission just finished */
        was_tx = tx;
    } while (s_run && (p_now_ms() - start) < budget_ms);
    if (slice) RADIO_RESUME();
}

/*
 * When nothing is queued the slot is a receive duty: one slot every period_ms.
 * When a packet is waiting, start it (holding the tx mutex so air() and the
 * poll loop do not both touch the station) and keep slotting back to back until
 * it and anything behind it have played out.
 */
static void station_task(void *arg)
{
    (void)arg;
    while (s_run) {
        tx_item_t it;
        if (xQueueReceive(s_txq, &it, 0) == pdTRUE) {
            p_lock(NULL);
            const int r = xw_station_send(s_sta, (const uint8_t *)it.wire,
                                          (size_t)it.len, TX_MODE);
            p_unlock(NULL);
            if (r == -2) {
                /* §7.3: the channel carries somebody else's transmission.
                 * That is a wait, not a failure -- a 3s-mode packet holds the
                 * air for seconds -- so put the packet back, keep receiving
                 * (the slot hears the end of what is airing), and try again
                 * after a random fraction of one. The queue's depth bounds
                 * how much can wait. */
                xQueueSendToFront(s_txq, &it, 0);
                run_slot(300 + (esp_random() % 900));
                drain_rx(NULL);
                xb_tick(s_bearer, p_now_ms());
                xb_tick_all(p_now_ms());
                continue;
            }
            if (r != 0) {
                s_tx_failed++;
                ESP_LOGW(TAG, "xw_station_send(%d bytes) refused: %d", it.len, r);
            }
            /* play it (and anything already queued) to the end, slot by slot */
            do {
                run_slot(s_cfg.slot_ms ? s_cfg.slot_ms : 7840 /* one block span */);
                drain_rx(NULL);
                xb_tick(s_bearer, p_now_ms());
                xb_tick_all(p_now_ms());
                if (s_cfg.gap_ms) vTaskDelay(pdMS_TO_TICKS(s_cfg.gap_ms));
            } while (s_run && s_sta->tx.active);
            continue;
        }
        /* idle: one receive slot, then wait out the period (or a send wake) */
        run_slot(s_cfg.slot_ms ? s_cfg.slot_ms : s_cfg.period_ms);
        drain_rx(NULL);
        xb_tick(s_bearer, p_now_ms());
        /* And the shared registry: normally the LAN bearer's task is the pump
         * every ticked bearer rides (LoRa, BLE, ESP-NOW). On a board running
         * XPRS-WiFi with 802.11 off there is no LAN task, so this task is the
         * one that is awake on a cadence -- double-ticking when both run is
         * harmless (the queues drain on their own jitter deadlines). */
        xb_tick_all(p_now_ms());
        const uint32_t wait = s_cfg.period_ms > s_cfg.slot_ms
                              ? s_cfg.period_ms - s_cfg.slot_ms : 1;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
    vTaskDelete(NULL);
}

/* ── Wiring the station's port ───────────────────────────────────────────── */

/* Fill the port and return the clamped look count. The extra-window buffers are
 * wired to the station AFTER xw_station_init (which initialises the station),
 * in wire_looks() — the port headers require that order. */
#if CONFIG_IDF_TARGET_ESP32C3
static uint8_t c3_port_init(xw_port_t *port, uint16_t amp, uint8_t looks)
{
    static xw_esp32c3_config_t c = XW_ESP32C3_CONFIG_DEFAULT;
    c.dac_amplitude = amp;
    c.looks = looks < 1 ? 1 : looks > XW_SEGMENTS ? XW_SEGMENTS : looks;
    xw_esp32c3_port_init(port, &c);
    return c.looks;
}
static void wire_looks(uint8_t looks)
{
    for (int k = 1; k < looks; k++) s_sta->extra_window[k - 1] = xw_esp32c3_look_buffer(k);
}
#elif CONFIG_IDF_TARGET_ESP32S3
static uint8_t s3_port_init(xw_port_t *port, uint16_t amp, uint8_t looks)
{
    static xw_esp32s3_config_t c = XW_ESP32S3_CONFIG_DEFAULT;
    c.dac_amplitude = amp;
    c.looks = looks < 1 ? 1 : looks > XW_SEGMENTS ? XW_SEGMENTS : looks;
    xw_esp32s3_port_init(port, &c);
    return c.looks;
}
static void wire_looks(uint8_t looks)
{
    for (int k = 1; k < looks; k++) s_sta->extra_window[k - 1] = xw_esp32s3_look_buffer(k);
}
#endif

static const xb_ops_t k_ops = {
    .air = p_air,
    .now_ms = p_now_ms,
    .random = p_random,
    .lock = p_lock,
    .unlock = p_unlock,
    .drain = NULL,              /* this bearer delivers on its own task (drain_rx) */
    .ctx = NULL,
    .name = "wifi8",
};

/* ── The public bearer, mapped onto xprs_bearer like xprsnow ─────────────── */

void xprswifi8_offer(const char *wire, int len)    { xb_offer(s_bearer, wire, len); }
void xprswifi8_digipeat(const char *wire, int len) { xb_digipeat(s_bearer, wire, len); }
void xprswifi8_echo(const char *wire, int len)     { xb_echo(s_bearer, wire, len); }
uint32_t xprswifi8_idle_ms(uint32_t now_ms)        { return xb_idle_ms(s_bearer, now_ms); }
bool xprswifi8_send(const char *wire, int len)     { return xb_send(s_bearer, wire, len); }
bool xprswifi8_is_active(void)                     { return xb_is_active(s_bearer); }
int  xprswifi8_peer_count(uint32_t max_age_sec)    { return xb_peer_count(s_bearer, max_age_sec); }
uint8_t xprswifi8_channel(void)                    { return s_cfg.channel; }
void xprswifi8_set_trace(bool on)                  { s_trace = on; }
void xprswifi8_set_heard_cb(xprswifi8_heard_cb_t cb) { xb_set_heard_cb(s_bearer, cb); }

void xprswifi8_set_rx_cb(xprswifi8_rx_cb_t cb)
{
    s_rx_cb = cb;
    xb_set_rx_cb(s_bearer, cb ? rx_shim : NULL);
}

void xprswifi8_set_beacon(xprswifi8_beacon_cb_t cb, uint32_t interval_sec,
                          uint32_t first_delay_sec)
{
    xb_set_beacon(s_bearer, cb, interval_sec, first_delay_sec);
}

bool xprswifi8_settle(uint32_t timeout_ms)
{
    if (!s_started) return false;
    const uint32_t start = p_now_ms();
    while (uxQueueMessagesWaiting(s_txq) > 0 || s_sta->tx.active) {
        if (p_now_ms() - start >= timeout_ms) {
            ESP_LOGW(TAG, "still airing after %ums", (unsigned)timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void xprswifi8_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled, uint32_t *dropped)
{
    xb_stats(s_bearer, rx, tx, cancelled);
    if (dropped) *dropped = s_dropped;
}

void xprswifi8_tx_stats(uint32_t *issued, uint32_t *done, uint32_t *failed)
{
    if (issued) *issued = s_tx_issued;
    if (done)   *done   = s_tx_done;
    if (failed) *failed = s_tx_failed;
}

esp_err_t xprswifi8_start(const char *callsign, const xprswifi8_cfg_t *cfg)
{
    if (s_started) return ESP_OK;
#if !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)
    (void)callsign; (void)cfg;
    ESP_LOGE(TAG, "no xwifi port for this chip");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_bearer) s_bearer = xprs_big_calloc(1, sizeof *s_bearer);
    if (!s_bearer) return ESP_ERR_NO_MEM;

    s_cfg = cfg ? *cfg : (xprswifi8_cfg_t)XPRSWIFI8_CFG_DEFAULT;
    if (s_cfg.channel < 1 || s_cfg.channel > XW_CHANNELS) s_cfg.channel = 2;
#if CONFIG_IDF_TARGET_ESP32S3
    /* Bank 2 holds the station state (above), so only banks 0 and 1 may carry
     * looks; the third look's +1 dB is what that placement costs. */
    if (s_cfg.looks > 2) s_cfg.looks = 2;
#endif

    s_txq = xQueueCreate(XPRSWIFI8_TX_QUEUE, sizeof(tx_item_t));
    s_rxq = xQueueCreate(XPRSWIFI8_RX_QUEUE, sizeof(rx_item_t));
    if (!s_tx_mtx) s_tx_mtx = xSemaphoreCreateMutex();
    if (!s_txq || !s_rxq || !s_tx_mtx) {
        if (s_txq) vQueueDelete(s_txq);
        if (s_rxq) vQueueDelete(s_rxq);
        s_txq = s_rxq = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* The station: profile E10 (10 dBm, the only one the C3 port calibrates for
     * — listen-before-talk energy sensing for E18 is not in the port yet), and
     * the one XPRS-WiFi channel this bearer uses. */
    const xw_config_t xc = {
        .profile = XW_PROFILE_E10,
        .eirp_dbm = 10,
        .channel_mask = (uint8_t)(1u << (s_cfg.channel - 1)),
        .on_packet = on_packet,
        .on_packet_ctx = NULL,
    };
    /* Calibrating the transmitter drives the replay engine, which must not race
     * 802.11 beaconing; quiesce the stack around init when we are sharing the
     * radio. Full-time mode (slot_ms == 0) owns the radio instead — but nobody
     * else has brought it up then, so this is where the port's radio_start
     * belongs: NVS, the event loop, WiFi promiscuous with no interface. */
    const bool slice = s_cfg.slot_ms > 0;
    if (!slice && RADIO_START() != 0) {
        ESP_LOGE(TAG, "radio start failed");
        vQueueDelete(s_txq); vQueueDelete(s_rxq);
        s_txq = s_rxq = NULL;
        return ESP_FAIL;
    }
    if (slice) RADIO_SUSPEND();
    const uint8_t looks = PORT_INIT(&s_port, s_cfg.dac_amplitude, s_cfg.looks);
    xw_station_init(s_sta, &s_port, &xc);
    /* The demod's working arrays (xw_scratch_t, ~6.4 KB): given by the
     * application like extra_window, so they stop being .bss under the bank
     * ceiling. PSRAM where the board has it, internal heap where it does
     * not; a later placement may move them into the port's reserved bank. */
    if (!s_sta->demod.scratch)
        s_sta->demod.scratch = xprs_big_calloc(1, sizeof(xw_scratch_t));
    if (!s_sta->demod.scratch) { if (slice) RADIO_RESUME(); return ESP_ERR_NO_MEM; }
    wire_looks(looks);   /* after init: the port headers require this order */
    if (slice) RADIO_RESUME();

    s_started = true;
    s_run = true;
    xb_init(s_bearer, &k_ops, callsign);
    /* This bearer drives its OWN tick (xb_tick in station_task), because its
     * task is already awake on the slot cadence — it does not register with the
     * shared bearer driver the way ESP-NOW and the LAN share one. On the S3 the
     * stack is static, in bank 2 behind the station state (see the top). */
#if CONFIG_IDF_TARGET_ESP32S3
    s_task = xTaskCreateStatic(station_task, "xwifi", S3_STACK_SIZE, NULL, 5,
                               s_task_stack, &s_task_tcb);
    if (!s_task) {
        s_started = s_run = false;
        return ESP_ERR_NO_MEM;
    }
#else
    if (xTaskCreate(station_task, "xwifi", 8192, NULL, 5, &s_task) != pdPASS) {
        s_started = s_run = false;
        return ESP_ERR_NO_MEM;
    }
#endif
    ESP_LOGI(TAG, "XPRS-WiFi up on channel %u, %s, callsign %s",
             s_cfg.channel,
             s_cfg.slot_ms ? "time-sliced with 802.11" : "radio to itself",
             callsign ? callsign : "");
    return ESP_OK;
#endif
}

void xprswifi8_stop(void)
{
    if (!s_started) return;
    s_run = false;
    if (s_task) { xTaskNotifyGive(s_task); vTaskDelay(pdMS_TO_TICKS(50)); s_task = NULL; }
    xb_stop(s_bearer);
    s_started = false;
}
