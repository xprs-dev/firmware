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

#include "mc.h"
#include "mc_mesh.h"
#include "xprs_core.h"
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
static int8_t   s_tx_power = 14;       /* what start() was given */
static char     s_region_want[12];     /* the region name the operator chose */
static bool     s_far;                 /* the `far` profile, xprs mode only */
/*
 * A frequency the operator set by hand, which outranks the region's.
 *
 * NOT EVERY BOARD IS AN 868 MHz BOARD. The same SX1262 is sold matched for
 * 433, 868 and 915 MHz, MeshCore's 433 communities each pick their own
 * channel, and a band this firmware has no preset for is still a band
 * somebody is on. So the frequency is a setting of its own (`lora_freq_hz`,
 * `cfg freq`, the Settings panel, an owner's `cmd:set freq:`), applied the
 * moment it is given. Picking a region afterwards clears it: choosing a
 * preset means taking its channel.
 */
static uint32_t s_freq_want;
static uint8_t  s_sf_want;             /* lora_sf, 0 = the mode's own */
static uint16_t s_bw_want;             /* lora_bw_khz, 0 = the mode's own */

/*
 * The regions, per mode (see the header).
 *
 * xprs: ERC 70-03 band g3 (869.4-869.65 MHz, 10%, 500 mW e.r.p.) and g1
 * (868.0-868.6 MHz, 1%, 25 mW) for the EU rows, FCC 15.247 / AU LIPD for
 * the 900 MHz rows, where the constraint is a 400 ms dwell rather than an
 * hourly budget. The reserve is about fifteen full packets at SF7.
 *
 * meshtastic: the frequency is Meshtastic's LongFast slot (mt_slot_freq_hz,
 * checked against the published 869.525 / 906.875 / 919.875 MHz in the
 * host test), the EU budget band g3's, and the reserve ten full SF11 frames:
 * enough that an emergency is never what a spent budget silences, small
 * enough that it cannot BE the budget.
 */
static const xprslora_region_t k_regions_xprs[] = {
    { "eu",    869500000u, 360000u, 6000u,    0, 27 },
    { "eu-g1", 868200000u,  36000u, 6000u,    0, 14 },
    /* 433 MHz, for the modules sold for that band: ERC 70-03's
     * 433.05-434.79 MHz, 10% and 10 mW e.r.p. The channel is ours to
     * choose and 433.900 is far enough from Meshtastic's 433 slot
     * (433.875) not to sit on top of it. */
    { "eu-433", 433900000u, 360000u, 6000u,   0, 10 },
    { "us",    903900000u,       0,     0, 400u, 30 },
    { "au",    917000000u,       0,     0, 400u, 30 },
};
static const xprslora_region_t k_regions_mt[] = {
    { "eu", 869525000u, 360000u, 21000u, 0, 27 },
    /* Meshtastic's EU_433 band (433.0-434.0, 10%, 10 dBm); the LongFast
     * slot in it is 433.875 by Meshtastic's own rule, which the host test
     * checks with mt_slot_freq_hz rather than trusting this number. */
    { "eu-433", 433875000u, 360000u, 21000u, 0, 10 },
    { "us", 906875000u,       0,     0, 0, 30 },
    { "au", 919875000u,       0,     0, 0, 30 },
};
/* meshcore: a channel of its own, measured off a stock node rather than
 * assumed (mc.h). 869.618 MHz is band g3, so the hourly budget is the same
 * 10% the other g3 rows get; the reserve is ten full frames, which at
 * SF8/62.5 kHz is a smaller number of seconds than LongFast needs. The US
 * and AU rows are what MeshCore's own builds use for those regions and
 * have NOT been heard on this bench. */
static const xprslora_region_t k_regions_mc[] = {
    { "eu", 869618000u, 360000u, 12000u, 0, 27 },
    { "us", 910525000u,       0,     0, 0, 30 },
    { "au", 915800000u,       0,     0, 0, 30 },
};

/* Which network's frames this mode speaks: it picks the wrap, the unwrap
 * and the engine, and `none` means the wire goes on the air bare. */
typedef enum { LR_NET_NONE = 0, LR_NET_MT, LR_NET_MC } lr_net_t;

/* One row per mode: what the radio is set to, and whose frames it carries.
 * The airtime ledger is built from the same row the radio is configured
 * from, so it cannot drift. */
typedef struct {
    const char *name;
    bool available;
    lr_net_t net;
    sx1262_sf_t sf, sf_far;
    sx1262_bw_t bw;
    uint32_t bw_hz;
    uint16_t preamble;
    uint8_t sync_word;
    uint32_t pace_ms;
    uint32_t slot_ms;          /* backoff unit while the channel is busy */
    /* How long auto-detect waits here before calling it empty. Each
     * network sets its own, because each makes a relay wait differently
     * (docs/lora.md, "Auto-detect"): MeshCore answers inside about a
     * second and a half, while Meshtastic's contention rule deliberately
     * makes the CLOSEST node wait longest -- up to 7.6 s at LongFast -- so
     * a short dwell there would miss exactly the repeater in the room. A
     * network that IS there is found long before this: the sweep leaves a
     * mode the moment it has an answer. */
    uint32_t detect_ms;
    const xprslora_region_t *regions;
    int nregions;
} lr_mode_def_t;

static const lr_mode_def_t k_modes[XPRSLORA_MODE_COUNT] = {
    [XPRSLORA_MODE_XPRS] = {
        .name = "xprs", .available = true, .net = LR_NET_NONE,
        .sf = SX1262_SF7, .sf_far = SX1262_SF9, .bw = SX1262_BW_125,
        .bw_hz = 125000u, .preamble = 8, .sync_word = 0x12,
        .pace_ms = 6000u, .slot_ms = 10u,
        .detect_ms = 10000u,       /* no probe: its stations beacon often */
        .regions = k_regions_xprs,
        .nregions = (int)(sizeof k_regions_xprs / sizeof k_regions_xprs[0]),
    },
    [XPRSLORA_MODE_MESHTASTIC] = {
        .name = "meshtastic", .available = true, .net = LR_NET_MT,
        .sf = SX1262_SF11, .sf_far = SX1262_SF11, .bw = SX1262_BW_250,
        .bw_hz = MT_LF_BW_HZ, .preamble = MT_LF_PREAMBLE,
        .sync_word = MT_LF_SYNC, .pace_ms = 10000u, .slot_ms = 28u,
        .detect_ms = 20000u,       /* 7.6 s of contention, and room to spare */
        .regions = k_regions_mt,
        .nregions = (int)(sizeof k_regions_mt / sizeof k_regions_mt[0]),
    },
    [XPRSLORA_MODE_MESHCORE] = {
        .name = "meshcore", .available = true, .net = LR_NET_MC,
        .sf = SX1262_SF8, .sf_far = SX1262_SF8, .bw = SX1262_BW_62_5,
        .bw_hz = MC_BW_HZ, .preamble = MC_PREAMBLE,
        .sync_word = MC_SYNC, .pace_ms = 10000u, .slot_ms = 28u,
        .detect_ms = 8000u,        /* it answers inside about 1.3 s */
        .regions = k_regions_mc,
        .nregions = (int)(sizeof k_regions_mc / sizeof k_regions_mc[0]),
    },
};

static xprslora_mode_t s_mode = XPRSLORA_MODE_DEFAULT;
static const lr_mode_def_t *s_def = &k_modes[XPRSLORA_MODE_DEFAULT];

const char *xprslora_mode_name(xprslora_mode_t mode)
{
    return mode < XPRSLORA_MODE_COUNT ? k_modes[mode].name : "?";
}

bool xprslora_mode_parse(const char *word, xprslora_mode_t *out)
{
    if (!word) return false;
    for (int i = 0; i < XPRSLORA_MODE_COUNT; i++)
        if (strcasecmp(word, k_modes[i].name) == 0) {
            if (out) *out = (xprslora_mode_t)i;
            return true;
        }
    return false;
}

bool xprslora_mode_available(xprslora_mode_t mode)
{
    return mode < XPRSLORA_MODE_COUNT && k_modes[mode].available;
}

xprslora_mode_t xprslora_mode(void)
{
    return s_mode;
}

const xprslora_region_t *xprslora_regions(xprslora_mode_t mode, int *count)
{
    const lr_mode_def_t *d = mode < XPRSLORA_MODE_COUNT ? &k_modes[mode] : NULL;
    if (count) *count = d ? d->nregions : 0;
    return d ? d->regions : NULL;
}

const xprslora_region_t *xprslora_region(void)
{
    return s_region ? s_region : &s_def->regions[0];
}

/* What an XPRS wire of [len] bytes costs here: the wire itself, or, on a
 * network whose frames we wear, its frame or its two. */
static uint32_t lr_airtime(int len, void *ctx)
{
    (void)ctx;
    uint32_t ms = 0;
    switch (s_def->net) {
    case LR_NET_MT:
        for (int part = 0; part < mt_xprs_frames_for(len); part++)
            ms += xb_lora_airtime_ms(&s_air, mt_xprs_frame_len(len, part));
        return ms;
    case LR_NET_MC:
        for (int part = 0; part < mc_xprs_frames_for(len); part++)
            ms += xb_lora_airtime_ms(&s_air, mc_xprs_frame_len(len, part));
        return ms;
    case LR_NET_NONE:
    default:
        return xb_lora_airtime_ms(&s_air, len);
    }
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

/* What `meshcore` mode needs: the reassembly of two-part XPRS wires and,
 * when the bridge is started, the repeater and the bridge themselves. One
 * block, claimed on the way into the mode and released on the way out
 * UNLESS it landed in PSRAM, where keeping it costs nothing and claiming
 * it again might fail. A board without PSRAM cannot hold this and the
 * Meshtastic bridge at once, which is why it is freed there (docs/esp32.md,
 * and docs/lora.md "One radio, three networks"). The survey never reassembles, so a
 * rotation does not touch the heap. */
typedef struct {
    mc_mesh_t  mesh;
    mc_reasm_t reasm;
    bool       mesh_on;
} lr_mc_state_t;
static lr_mc_state_t *s_mc;

/* MeshCore's curve arithmetic has a task of its own, on core 1, because it
 * does not fit anywhere else: an Ed25519 verification is about 3.9 KB of
 * stack (measured with -fstack-usage on the target compiler) and this
 * bearer's task has roughly two to spare (docs/esp32.md, "Task stacks are
 * heap, and these are the measured floors" and "The two processors"). On a
 * single-core chip it runs unpinned, which XPRS_WORK_CORE says for us.
 * Started with the bridge, stopped before its state is freed. */
static TaskHandle_t s_mc_worker;
static volatile bool s_mc_worker_stop, s_mc_worker_live;

static uint8_t s_rxbuf[MT_FRAME_MAX + 1];
static uint8_t s_txbuf[MT_FRAME_MAX];
static uint8_t s_frames[2][MT_FRAME_MAX];
static char    s_wire[XB_WIRE_MAX + 1];
static uint32_t s_hdr_since;           /* first saw "a header is arriving" */

/* The survey: which networks are out there, listened for one at a time,
 * and -- when it is an auto-detect -- asked. */
static struct {
    bool            active, done, probe;
    uint32_t        per_ms, started_ms;
    uint32_t        probe_ms;        /* airtime our own probes cost */
    uint32_t        probe_from, probe_id;   /* the Meshtastic one */
    uint8_t         probes;          /* how many we have aired in this mode */
    uint32_t        probe_hash;              /* the MeshCore one */
    xprslora_mode_t home;
    xprslora_survey_mode_t m[XPRSLORA_MODE_COUNT];
} s_survey;
static uint32_t s_cad_busy, s_cad_waits;

/* The DIO1 interrupt only raises this flag; every SPI byte moves on the
 * bearer task in lr_drain(). An ISR that touched the bus would collide with
 * whatever transfer the display has in flight. */
static volatile bool s_rx_pending;

/* How long a mode stands still before its probe goes out. */
#define MC_SETTLE_MS 1200u

static void survey_frame(const uint8_t *frame, int len);
static void survey_tick(void);
static bool survey_echo(const uint8_t *frame, int len);
static void survey_probe(void);
static esp_err_t lr_claim_for(const lr_mode_def_t *d);
static void lr_mc_release(void);
static bool mc_worker_stop(void);
static bool lr_bw_of(uint16_t khz, sx1262_bw_t *bw, uint32_t *hz);
static sx1262_lora_config_t lr_modem(const lr_mode_def_t *d,
                                     const xprslora_region_t *reg, int *sf_n);

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

/* An XPRS wire: the frame itself in `xprs` mode, wrapped in `meshtastic`
 * mode. Waits for a clear channel: the xb pump that calls this has already
 * charged the ledger and dropped the packet from its queue, so "not now" is
 * not an answer it can take. */
static bool lr_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_radio || len <= 0 || len > XB_WIRE_MAX) return false;
    int fl[2];
    int n;
    switch (s_def->net) {
    case LR_NET_MT:
        n = mt_xprs_wrap(wire, len, s_self, s_frames, fl);
        break;
    case LR_NET_MC:
        n = mc_xprs_wrap(wire, len, s_frames, fl);
        break;
    case LR_NET_NONE:
    default:
        memcpy(s_frames[0], wire, (size_t)len);
        fl[0] = len;
        n = 1;
        break;
    }
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
            vTaskDelay(pdMS_TO_TICKS(s_def->slot_ms * (1 + esp_random() % 8)));
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
    /* Not this radio's network any more: the bridge keeps the frame and
     * asks again, which is what it does for a busy channel, so a switch
     * back finds its queue where it was. A station in `xprs` mode airing
     * LongFast was the first thing the live switch got wrong. */
    if (s_def->net != LR_NET_MT || s_survey.active) return false;
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

/* A MeshCore frame from its bridge, on the same terms as a Meshtastic one:
 * the ledger first, then the channel, and "not now" is an answer. */
static bool lr_air_mc(void *ctx, const uint8_t *frame, int len, int prio)
{
    (void)ctx;
    if (!s_radio || !s_lora || len <= 0 || len > MC_FRAME_MAX) return false;
    if (s_def->net != LR_NET_MC || s_survey.active) return false;
    lr_lock(NULL);
    bool ok = false;
    if (!sx1262_tx_active(s_radio) && !lr_channel_busy()) {
        uint32_t ms = xb_lora_airtime_ms(&s_air, len);
        if (xb_spend(s_lora, ms, prio >= MC_PRIO_URGENT)) {
            ok = lr_start(frame, len);
            mc_pkt_t p;
            if (ok && mc_parse(frame, len, &p))
                ESP_LOGI(TAG, "mc tx type %02x route %d hop %d %dB", p.type,
                         p.route, p.hops, len);
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

static void lr_mc_tick(void)
{
    if (s_def->net != LR_NET_MC) return;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    if (s_mc && s_mc->mesh_on) mc_mesh_tick(&s_mc->mesh, lr_now_ms());
    xSemaphoreGiveRecursive(s_mt_mutex);
}

static void lr_mt_tick(void)
{
    if (!s_st || !s_st->mesh_on || s_def->net != LR_NET_MT) return;
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
        if (got == ESP_OK && info.len > 0 && s_survey.active) {
            /* Surveying: counted and named, handed to nobody. */
            survey_frame(s_rxbuf, info.len);
        } else if (got == ESP_OK && info.len > 0 &&
                   s_def->net == LR_NET_NONE) {
            /* `xprs` mode: the frame is the wire, or it is not ours. */
            if (xprs_looks_like(s_rxbuf, info.len) && info.len <= XB_WIRE_MAX) {
                memcpy(s_wire, s_rxbuf, info.len);
                s_wire[info.len] = 0;
                ESP_LOGI(TAG, "RX %u bytes at %d dBm SNR %d: %.48s",
                         (unsigned)info.len, info.rssi, info.snr, s_wire);
                xb_on_wire(s_lora, s_wire, info.len, 0, info.rssi);
            } else {
                ESP_LOGI(TAG, "heard %u bytes that were not XPRS (%d dBm)",
                         (unsigned)info.len, info.rssi);
            }
        } else if (got == ESP_OK && info.len > 0 &&
                   s_def->net == LR_NET_MC) {
            /* `meshcore` mode: ours is a RAW_CUSTOM payload, and
             * everything else on the channel is MeshCore's own, for the
             * repeater and the bridge (mc_mesh.c).
             *
             * The whole branch is under the bridge's lock, because the
             * reassembly buffer and the bridge are one block that a mode
             * change on another task may free (lr_mc_release), and the
             * lock is where that is settled. */
            xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
            int n = mc_xprs_unwrap(s_mc ? &s_mc->reasm : NULL, s_rxbuf,
                                   info.len, lr_now_ms(), s_wire, sizeof s_wire);
            if (n > 0) {
                ESP_LOGI(TAG, "RX %d bytes at %d dBm SNR %d: %.48s", n,
                         info.rssi, info.snr, s_wire);
                if (s_mc) mc_mesh_note_xprs_frame(&s_mc->mesh, s_rxbuf, info.len);
                xSemaphoreGiveRecursive(s_mt_mutex);
                xb_on_wire(s_lora, s_wire, n, 0, info.rssi);
            } else if (n < 0) {
                mc_pkt_t p;
                if (mc_parse(s_rxbuf, info.len, &p))
                    ESP_LOGI(TAG, "mc type %02x route %d hop %d %uB %d dBm",
                             p.type, p.route, p.hops, (unsigned)info.len,
                             info.rssi);
                else
                    ESP_LOGI(TAG, "heard %u bytes that were not MeshCore"
                             " (%d dBm)", (unsigned)info.len, info.rssi);
                if (s_mc && s_mc->mesh_on)
                    mc_mesh_on_frame(&s_mc->mesh, s_rxbuf, info.len, info.rssi,
                                     info.snr);
                xSemaphoreGiveRecursive(s_mt_mutex);
            } else {
                /* n == 0: half a wire, waiting for its sibling. */
                xSemaphoreGiveRecursive(s_mt_mutex);
            }
        } else if (got == ESP_OK && info.len > 0) {
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
                if (s_st && s_st->mesh_on && s_def->net == LR_NET_MT) {
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
    survey_tick();
    if (!s_survey.active) {
        lr_mt_tick();
        lr_mc_tick();
    }

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

    if (!xprslora_mode_available(cfg->mode)) {
        ESP_LOGE(TAG, "LoRa mode %s is not in this firmware",
                 xprslora_mode_name(cfg->mode));
        sx1262_delete(s_radio);
        s_radio = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_mode = cfg->mode;
    s_def = &k_modes[s_mode];
    err = lr_claim_for(s_def);
    if (err == ESP_ERR_NO_MEM && s_mode != XPRSLORA_MODE_DEFAULT) {
        /* A radio that does not start is worse than a radio on another
         * network: this board cannot afford the mode it was asked for, so
         * it comes up in the default one and says so in a line nobody can
         * miss. The setting is left alone, so a board with more room -- or
         * a smaller build -- takes it next time. */
        ESP_LOGE(TAG, "coming up in %s mode instead of %s, which this board "
                      "has no room for",
                 xprslora_mode_name(XPRSLORA_MODE_DEFAULT),
                 xprslora_mode_name(s_mode));
        s_mode = XPRSLORA_MODE_DEFAULT;
        s_def = &k_modes[s_mode];
        err = lr_claim_for(s_def);
    }
    if (err != ESP_OK) {
        sx1262_delete(s_radio);
        s_radio = NULL;
        return err;
    }
    /* Kept for a live mode change: the power the operator set and the region
     * they named, which is matched by NAME in the new mode's table (`eu`
     * stays `eu`). A `lora_freq_hz` override is a start-time thing and is
     * not carried across: another mode is another channel. */
    s_tx_power = cfg->tx_power_dbm ? cfg->tx_power_dbm : 14;
    s_freq_want = cfg->freq_hz;
    snprintf(s_region_want, sizeof s_region_want, "%s",
             cfg->region ? cfg->region : "");
    s_far = cfg->far;
    s_region = &s_def->regions[0];
    if (cfg->region && cfg->region[0])
        for (int i = 0; i < s_def->nregions; i++)
            if (strcasecmp(s_def->regions[i].name, cfg->region) == 0)
                s_region = &s_def->regions[i];

    /* The mode's modulation, which every station and node on the channel
     * shares, unless the operator has had to follow a neighbour onto
     * another one (cfg->sf, cfg->bw_khz). The two ends of a link agree by
     * construction because both run these lines. The `far` profile (SF9)
     * exists only in `xprs` mode. */
    s_sf_want = cfg->sf;
    s_bw_want = cfg->bw_khz;
    int sf_n = 0;
    sx1262_lora_config_t lora = lr_modem(s_def, s_region, &sf_n);
    if (cfg->freq_hz) lora.frequency_hz = cfg->freq_hz;
    lora.tx_power_dbm = cfg->tx_power_dbm ? cfg->tx_power_dbm : 14;
    lora.use_tcxo = cfg->use_tcxo;
    lora.use_dio2_rf_switch = cfg->use_dio2_rf_switch;
    uint32_t bw_hz = s_def->bw_hz;
    { sx1262_bw_t bw_tmp; lr_bw_of(s_bw_want, &bw_tmp, &bw_hz); }
    /* The airtime table is built from the SAME values the radio was just
     * given, so the ledger cannot drift from the modem. */
    s_air = (xb_lora_air_t){ .bw_hz = bw_hz, .sf = (uint8_t)sf_n,
                             .cr = 1, .preamble = s_def->preamble,
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
    xb_set_pace(s_lora, s_def->pace_ms);
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

    ESP_LOGI(TAG, "up in %s mode: %lu Hz SF%d/%luk, sync 0x%02X, %d dBm as %s"
             " -- %s: %lus of airtime an hour, %lus reserved",
             s_def->name, (unsigned long)lora.frequency_hz, sf_n,
             (unsigned long)(s_def->bw_hz / 1000u), s_def->sync_word,
             lora.tx_power_dbm, callsign, s_region->name,
             (unsigned long)(s_region->duty_ms / 1000u),
             (unsigned long)(s_region->reserve_ms / 1000u));
    if (lora.tx_power_dbm > s_region->max_dbm)
        ESP_LOGW(TAG, "%d dBm exceeds the %s region's %d dBm e.r.p. ceiling"
                 " -- the operator owns that call", lora.tx_power_dbm,
                 s_region->name, s_region->max_dbm);
    return ESP_OK;
}

/* The bandwidth an operator asked for, as the chip counts it, and what it
 * is in Hz. Anything unknown leaves the mode's own. */
static bool lr_bw_of(uint16_t khz, sx1262_bw_t *bw, uint32_t *hz)
{
    switch (khz) {
    case 62:  *bw = SX1262_BW_62_5; *hz = 62500u;  return true;
    case 125: *bw = SX1262_BW_125;  *hz = 125000u; return true;
    case 250: *bw = SX1262_BW_250;  *hz = 250000u; return true;
    case 500: *bw = SX1262_BW_500;  *hz = 500000u; return true;
    default:  return false;
    }
}

/* The spreading factor this mode runs at, the operator's override first. */
static sx1262_sf_t lr_sf_of(const lr_mode_def_t *d)
{
    if (s_sf_want >= 7 && s_sf_want <= 12) return (sx1262_sf_t)s_sf_want;
    return (s_far && d == &k_modes[XPRSLORA_MODE_XPRS]) ? d->sf_far : d->sf;
}

/* The modem settings of [d] on [reg], built from one place so the live
 * retune and the first tuning cannot drift apart. */
static sx1262_lora_config_t lr_modem(const lr_mode_def_t *d,
                                     const xprslora_region_t *reg, int *sf_n)
{
    sx1262_sf_t sf = lr_sf_of(d);
    sx1262_bw_t bw = d->bw;
    uint32_t bw_hz = d->bw_hz;
    lr_bw_of(s_bw_want, &bw, &bw_hz);
    if (sf_n) *sf_n = (int)sf;
    sx1262_lora_config_t lora = {
        .frequency_hz = s_freq_want ? s_freq_want : reg->freq_hz,
        .sf = sf,
        .bw = bw,
        .cr = SX1262_CR_4_5,
        .tx_power_dbm = s_tx_power,
        .preamble_len = d->preamble,
        .crc_on = true,
        .sync_word = d->sync_word,
    };
    return lora;
}

/* The region of [d] the operator's `lora_region` names, else its first. */
static const xprslora_region_t *lr_region_of(const lr_mode_def_t *d)
{
    for (int i = 0; i < d->nregions; i++)
        if (s_region_want[0] && strcasecmp(d->regions[i].name, s_region_want) == 0)
            return &d->regions[i];
    return &d->regions[0];
}

/* The buffers a mode needs before it is entered. Claimed BEFORE the radio
 * moves, so a station that cannot afford the new mode stays in the one it
 * is in and says so, rather than sitting on a channel it cannot read. */
/*
 * What `meshcore` mode needs of the INTERNAL heap, over and above what it
 * claims: the worker's 6 KB stack cannot live in PSRAM, the UI task asks
 * for 8 KB of its own, and docs/esp32.md sets the floor this board is
 * expected to keep at 4 KB. On the Heltec V3, which has no PSRAM, taking
 * the state and the stack without checking left 1,920 bytes free and a
 * reboot loop: the UI task could not start, and neither could anything
 * else (measured 2026-09-20).
 */
#define LR_MC_SPARE_INTERNAL 16384u

static esp_err_t lr_claim_for(const lr_mode_def_t *d)
{
    if (d->net != LR_NET_MC || s_survey.active || s_mc) return ESP_OK;
    /* A reading of the free heap HERE cannot answer the question: this
     * runs early in the boot, before the screen's task, the index and the
     * web server have taken theirs. The Heltec V3 had 48 KB free at this
     * point, passed any such test, and ended the boot with 1,920 bytes and
     * a reboot loop. What the board has is decided by the board, so the
     * rule is the board's: without PSRAM there is no room for MeshCore's
     * state AND a 6 KB stack that cannot live anywhere else. */
    bool psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= sizeof(lr_mc_state_t);
    if (!psram) {
        ESP_LOGE(TAG, "MeshCore wants %u bytes of state and a %u byte stack, "
                      "and this board has no PSRAM to put either in: "
                      "staying in %s mode (docs/lora.md, \"What it "
                      "costs\")",
                 (unsigned)sizeof(lr_mc_state_t), 6144u, s_def->name);
        return ESP_ERR_NO_MEM;
    }
    size_t have = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (have < LR_MC_SPARE_INTERNAL + 6144u) {
        ESP_LOGE(TAG, "MeshCore's worker needs a %u byte stack and only %u "
                      "bytes of internal heap are free: staying in %s mode",
                 6144u, (unsigned)have, s_def->name);
        return ESP_ERR_NO_MEM;
    }
    s_mc = heap_caps_calloc(1, sizeof *s_mc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mc) s_mc = heap_caps_calloc(1, sizeof *s_mc, MALLOC_CAP_8BIT);
    if (!s_mc) {
        ESP_LOGE(TAG, "no room for MeshCore (%u bytes)", (unsigned)sizeof *s_mc);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MeshCore: %u bytes %s", (unsigned)sizeof *s_mc,
             esp_ptr_external_ram(s_mc) ? "in PSRAM" : "internal");
    return ESP_OK;
}

/*
 * Leaving the mode does NOT hand the state back.
 *
 * It used to, on a board whose internal heap held it, and that is the
 * pattern docs/esp32.md warns about in as many words: "freeing memory does
 * not fix an over-committed board -- it moves the victim". A board that
 * cannot hold this and everything else does not enter the mode at all
 * (lr_claim_for), and a board that can keeps the block, as the Meshtastic
 * bridge's is kept, so a switch back finds it where it was and a 10 KB
 * hole is not opened and closed in a heap this size.
 *
 * The worker task is stopped, though: it is 6 KB of stack doing nothing on
 * another network's channel, and an install wants it back
 * (xprslora_mc_pause).
 */
static void lr_mc_release(void)
{
    if (s_survey.active || !s_mc) return;
    if (s_def->net == LR_NET_MC) return;
    if (mc_worker_stop())
        ESP_LOGI(TAG, "MeshCore: worker stopped, %s mode has no use for it",
                 s_def->name);
}

/* Point the radio at [mode]. The radio lock is held by the caller. */
static esp_err_t lr_tune_mode(xprslora_mode_t mode, bool say)
{
    const lr_mode_def_t *d = &k_modes[mode];
    esp_err_t claimed = lr_claim_for(d);
    if (claimed != ESP_OK) {
        ESP_LOGE(TAG, "staying in %s mode", s_def->name);
        return claimed;
    }
    const xprslora_region_t *reg = lr_region_of(d);
    int sf_n = 0;
    sx1262_lora_config_t lora = lr_modem(d, reg, &sf_n);
    lr_tx_wait_idle();                 /* never retune under a transmission */
    esp_err_t err = sx1262_retune(s_radio, &lora);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not retune to %s: %s", d->name,
                 esp_err_to_name(err));
        lr_listen();
        return err;
    }
    s_mode = mode;
    s_def = d;
    s_region = reg;
    /* The ledger is built from what the radio was JUST given, overrides
     * included, so it cannot drift from the modem. */
    s_air = (xb_lora_air_t){ .bw_hz = lora.bw == SX1262_BW_62_5  ? 62500u
                                    : lora.bw == SX1262_BW_125   ? 125000u
                                    : lora.bw == SX1262_BW_250   ? 250000u
                                    : lora.bw == SX1262_BW_500   ? 500000u
                                                                 : d->bw_hz,
                             .sf = (uint8_t)sf_n,
                             .cr = 1, .preamble = d->preamble,
                             .crc = true, .implicit_header = false };
    s_hdr_since = 0;
    lr_listen();
    if (say)
        ESP_LOGI(TAG, "now in %s mode: %lu Hz SF%d/%luk, sync 0x%02X (%s)",
                 d->name, (unsigned long)lora.frequency_hz, sf_n,
                 (unsigned long)(d->bw_hz / 1000u), lora.sync_word, reg->name);
    return ESP_OK;
}

/* What the SX1262 can be tuned to at all (its datasheet: 150-960 MHz).
 * Whether a frequency is LEGAL where the station stands is the operator's
 * to answer, as the power ceiling already is, and the log says what was
 * asked for so the answer is on the record. */
#define LR_FREQ_MIN 150000000u
#define LR_FREQ_MAX 960000000u

esp_err_t xprslora_set_freq(uint32_t hz)
{
    if (!s_radio || !s_lora) return ESP_ERR_INVALID_STATE;
    if (s_survey.active) return ESP_ERR_INVALID_STATE;
    if (hz && (hz < LR_FREQ_MIN || hz > LR_FREQ_MAX)) return ESP_ERR_INVALID_ARG;

    uint32_t was = s_freq_want;
    s_freq_want = hz;
    lr_lock(NULL);
    esp_err_t err = lr_tune_mode(s_mode, true);
    lr_unlock(NULL);
    if (err != ESP_OK) {
        s_freq_want = was;                 /* nothing moved, nothing kept */
        return err;
    }
    const xprslora_region_t *reg = xprslora_region();
    if (hz) {
        /* A frequency away from the region's own channel is not refused --
         * a 433 MHz board on a 433 community's channel is exactly what
         * this exists for -- but the hour and the ceiling this station
         * meters against are still that region's, and they were written
         * for another band. Say it plainly rather than imply that 27 dBm
         * is fine at 433 MHz. */
        uint32_t d = hz > reg->freq_hz ? hz - reg->freq_hz : reg->freq_hz - hz;
        if (d > 10000000u)
            ESP_LOGW(TAG, "LoRa frequency set by hand: %lu Hz, which is not "
                          "in the %s preset's band. Its hour (%lus) and its "
                          "%d dBm ceiling are what this station still meters "
                          "against, and they were written for %lu Hz: what "
                          "is allowed here is the operator's to answer",
                     (unsigned long)hz, reg->name,
                     (unsigned long)(reg->duty_ms / 1000u), reg->max_dbm,
                     (unsigned long)reg->freq_hz);
        else
            ESP_LOGW(TAG, "LoRa frequency set by hand: %lu Hz (the %s "
                          "region's hour and its %d dBm ceiling apply)",
                     (unsigned long)hz, reg->name, reg->max_dbm);
    } else {
        ESP_LOGI(TAG, "LoRa frequency back to the %s preset: %lu Hz",
                 reg->name, (unsigned long)reg->freq_hz);
    }
    return ESP_OK;
}

uint32_t xprslora_freq(void)
{
    if (!s_radio) return 0;
    return s_freq_want ? s_freq_want : xprslora_region()->freq_hz;
}

esp_err_t xprslora_set_region(const char *name)
{
    if (!s_radio || !s_lora || !name || !name[0]) return ESP_ERR_INVALID_STATE;
    if (s_survey.active) return ESP_ERR_INVALID_STATE;
    bool known = false;
    for (int i = 0; i < s_def->nregions; i++)
        if (strcasecmp(s_def->regions[i].name, name) == 0) known = true;
    if (!known) return ESP_ERR_NOT_FOUND;

    char was[sizeof s_region_want];
    snprintf(was, sizeof was, "%s", s_region_want);
    uint32_t was_freq = s_freq_want;
    snprintf(s_region_want, sizeof s_region_want, "%s", name);
    s_freq_want = 0;                     /* a preset brings its own channel */
    lr_lock(NULL);
    esp_err_t err = lr_tune_mode(s_mode, true);
    if (err == ESP_OK) {
        xb_set_duty(s_lora, &s_duty, lr_airtime, NULL, s_region->duty_ms,
                    s_region->reserve_ms, s_region->dwell_ms);
    } else {
        snprintf(s_region_want, sizeof s_region_want, "%s", was);
        s_freq_want = was_freq;
    }
    lr_unlock(NULL);
    return err;
}

esp_err_t xprslora_set_mode(xprslora_mode_t mode)
{
    if (!s_radio || !s_lora) return ESP_ERR_INVALID_STATE;
    if (!xprslora_mode_available(mode)) return ESP_ERR_NOT_SUPPORTED;
    if (s_survey.active) return ESP_ERR_INVALID_STATE;
    if (mode == s_mode) return ESP_OK;

    lr_lock(NULL);
    esp_err_t err = lr_tune_mode(mode, true);
    if (err == ESP_OK) {
        /* The pace and the ledger belong to the channel, not to the radio:
         * a frame at SF11 is five times a frame at SF7, and the hour is the
         * region's. */
        xb_set_pace(s_lora, s_def->pace_ms);
        xb_set_duty(s_lora, &s_duty, lr_airtime, NULL, s_region->duty_ms,
                    s_region->reserve_ms, s_region->dwell_ms);
    }
    lr_unlock(NULL);
    if (err == ESP_OK) lr_mc_release();
    /* What was in flight on the old channel is gone: a mode change is an
     * operator's decision and costs its neighbours whatever they were
     * saying. The bridge's state is NOT freed -- a switch back finds it
     * where it was, and a 7 KB block freed and claimed again is how a heap
     * this size fragments (docs/esp32.md). */
    return err;
}

/* ── The survey ──────────────────────────────────────────────────────────
 *
 * Which of the three networks is actually out there is a question a station
 * can answer for itself: listen on each in turn and say what was heard. It
 * never transmits while it does (the ledger is spent for the duration), it
 * hands nothing to the bearer or to a bridge, and it puts the radio back
 * where it found it. */

static void survey_note(const char *name, int len)
{
    xprslora_survey_mode_t *m = &s_survey.m[s_mode];
    if (len <= 0 || m->names >= XPRSLORA_SURVEY_NAMES) return;
    char buf[XPRSLORA_SURVEY_NAME_LEN];
    int n = len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1;
    memcpy(buf, name, (size_t)n);
    buf[n] = 0;
    for (int i = 0; i < m->names; i++)
        if (strcmp(m->name[i], buf) == 0) return;
    snprintf(m->name[m->names++], sizeof m->name[0], "%s", buf);
}

/* One frame, while surveying: counted, and named where the name is cheap. */
static void survey_frame(const uint8_t *frame, int len)
{
    s_survey.m[s_mode].frames++;
    if (survey_echo(frame, len)) {
        s_survey.m[s_mode].relayed = true;
        ESP_LOGI(TAG, "survey: %s carried our packet -- a repeater is in "
                      "reach", s_def->name);
        return;
    }
    if (s_mode == XPRSLORA_MODE_XPRS) {
        if (!xprs_looks_like(frame, len)) return;
        for (int i = 0; i + 3 < len; i++) {
            if (frame[i] != ' ' || frame[i + 1] != 'f' || frame[i + 2] != ':')
                continue;
            int j = i + 3, n = 0;
            while (j + n < len && frame[j + n] != ' ') n++;
            survey_note((const char *)frame + j, n);
            return;
        }
        return;
    }
    if (s_mode == XPRSLORA_MODE_MESHTASTIC) {
        mt_hdr_t h;
        if (!mt_hdr_parse(frame, len, &h)) return;
        if (h.channel != mt_longfast_hash() || len <= MT_HDR_LEN) return;
        int pn = len - MT_HDR_LEN;
        memcpy(s_txbuf, frame + MT_HDR_LEN, (size_t)pn);
        mt_data_t d;
        if (!mt_crypt(mt_default_key, 16, h.from, h.id, s_txbuf, pn) ||
            !mt_data_decode(s_txbuf, pn, &d) || d.portnum != MT_PORT_NODEINFO)
            return;
        mt_user_t u;
        if (mt_user_decode(d.payload, d.payload_len, &u) && u.long_name[0])
            survey_note(u.long_name, (int)strlen(u.long_name));
        return;
    }
    if (s_mode == XPRSLORA_MODE_MESHCORE) {
        /* An advert is the one MeshCore frame that carries a name, and it
         * is signed, so a name read from one is a name somebody stands
         * behind. Everything else is counted and left alone. */
        mc_pkt_t p;
        mc_advert_t a;
        if (!mc_parse(frame, len, &p) || p.type != MC_PT_ADVERT) return;
        if (mc_advert_open(p.payload, p.payload_len, &a) && a.name[0])
            survey_note(a.name, (int)strlen(a.name));
        return;
    }
}

/*
 * One small packet of the kind this network floods, so that a repeater
 * within reach answers by re-airing it. Cheap on purpose: no signature and
 * no key exchange, because this runs on the bearer's task between retunes
 * (docs/esp32.md, "Task stacks are heap").
 *
 *   meshtastic  a Data frame on XPRS's own portnum, one byte of payload.
 *               Every router relays by the header, not by what it can
 *               read, which is the same property XPRS rides on there.
 *   meshcore    an ACK with a checksum that matches nothing. Four bytes,
 *               no crypto, and the type is one their repeaters carry.
 *   xprs        nothing: our own stations beacon every few seconds.
 */
static void survey_probe(void)
{
    if (!s_survey.probe || !s_radio) return;
    xprslora_survey_mode_t *m = &s_survey.m[s_mode];
    int n = 0;
    if (s_def->net == LR_NET_MT) {
        mt_hdr_t h = { 0 };
        h.to = MT_BROADCAST;
        h.from = s_self;
        h.id = lr_random() | 1u;
        h.hop_limit = MT_HOP_DEFAULT;
        h.hop_start = MT_HOP_DEFAULT;
        /* LongFast's channel, NOT the XPRS one: a station running this
         * firmware unwraps anything on the XPRS channel as a piece of an
         * XPRS wire, parks the piece that never completes and so never
         * repeats it. On the bench that made every XPRS neighbour deaf to
         * the probe while stock routers would have carried it (measured
         * 2026-09-20). The portnum stays private, so nobody reads it. */
        h.channel = mt_longfast_hash();
        h.relay_node = (uint8_t)s_self;
        mt_hdr_build(&h, s_frames[0]);
        mt_data_t d = { 0 };
        uint8_t one = 0x01;            /* not a wrapped wire: nobody reads it */
        d.portnum = MT_PORT_XPRS;
        d.payload = &one;
        d.payload_len = 1;
        int dn = mt_data_encode(&d, s_frames[0] + MT_HDR_LEN,
                                MT_FRAME_MAX - MT_HDR_LEN);
        if (dn < 0) return;
        n = MT_HDR_LEN + dn;
        s_survey.probe_from = h.from;
        s_survey.probe_id = h.id;
    } else if (s_def->net == LR_NET_MC) {
        uint8_t pl[4];
        uint32_t r = lr_random();
        for (int i = 0; i < 4; i++) pl[i] = (uint8_t)(r >> (8 * i));
        mc_pkt_t p;
        memset(&p, 0, sizeof p);
        p.route = MC_ROUTE_FLOOD;
        p.type = MC_PT_ACK;
        p.hash_size = 1;
        p.payload = pl;
        p.payload_len = 4;
        n = mc_build(&p, s_frames[0], MC_FRAME_MAX);
        if (!n) return;
        s_survey.probe_hash = mc_packet_hash(&p);
    } else {
        return;                        /* XPRS: its stations speak often */
    }
    lr_tx_wait_idle();
    for (int t = 0; t < 4 && lr_channel_busy(); t++) {
        lr_listen();
        vTaskDelay(pdMS_TO_TICKS(s_def->slot_ms * (1 + esp_random() % 8)));
    }
    if (lr_start(s_frames[0], n)) {
        m->probed = true;
        s_survey.probe_ms += xb_lora_airtime_ms(&s_air, n);
        ESP_LOGI(TAG, "survey: asked %s with %d bytes", s_def->name, n);
    }
}

/* Is this frame the probe we just aired, carried by somebody else? */
static bool survey_echo(const uint8_t *frame, int len)
{
    if (!s_survey.probe) return false;
    if (s_def->net == LR_NET_MT) {
        mt_hdr_t h;
        if (!mt_hdr_parse(frame, len, &h)) return false;
        return h.from == s_survey.probe_from && h.id == s_survey.probe_id &&
               h.hop_limit < h.hop_start;
    }
    if (s_def->net == LR_NET_MC) {
        mc_pkt_t p;
        if (!mc_parse(frame, len, &p)) return false;
        return mc_packet_hash(&p) == s_survey.probe_hash && p.hops > 0;
    }
    return false;
}

/* The next mode worth listening on, or COUNT when the sweep is done. */
static xprslora_mode_t survey_next(xprslora_mode_t from)
{
    for (int m = (int)from + 1; m < XPRSLORA_MODE_COUNT; m++)
        if (xprslora_mode_available((xprslora_mode_t)m))
            return (xprslora_mode_t)m;
    return XPRSLORA_MODE_COUNT;
}

static esp_err_t survey_begin(uint32_t per_mode_s, bool probe)
{
    if (!s_radio || !s_lora) return ESP_ERR_INVALID_STATE;
    if (s_survey.active) return ESP_ERR_INVALID_STATE;
    /* 0 is allowed for an auto-detect and means "each network's own
     * ceiling", which is what an operator who has not thought about it
     * should get. A survey has no per-mode figure to fall back on. */
    if (per_mode_s || !probe) {
        if (per_mode_s < 5) per_mode_s = 5;
        if (per_mode_s > 300) per_mode_s = 300;
    }

    lr_lock(NULL);
    memset(&s_survey, 0, sizeof s_survey);
    s_survey.probe = probe;
    s_survey.home = s_mode;
    s_survey.per_ms = per_mode_s * 1000u;
    s_survey.started_ms = lr_now_ms();
    s_survey.active = true;
    /* Nothing leaves while we are off our own channel: an hour of one
     * millisecond is a budget nothing fits in, and our own packets WAIT
     * rather than being dropped (xb_send_ex's deferred path). */
    xb_set_duty(s_lora, &s_duty, lr_airtime, NULL, 1u, 0u, 0u);
    xprslora_mode_t first = xprslora_mode_available(XPRSLORA_MODE_XPRS)
                                ? XPRSLORA_MODE_XPRS
                                : survey_next(XPRSLORA_MODE_XPRS);
    esp_err_t err = lr_tune_mode(first, false);
    lr_unlock(NULL);
    if (err != ESP_OK) {
        s_survey.active = false;
        xb_set_duty(s_lora, &s_duty, lr_airtime, NULL, s_region->duty_ms,
                    s_region->reserve_ms, s_region->dwell_ms);
        return err;
    }
    if (per_mode_s)
        ESP_LOGI(TAG, "%s: %lus on each mode, starting with %s",
                 probe ? "auto-detect" : "survey", (unsigned long)per_mode_s,
                 xprslora_mode_name(first));
    else
        ESP_LOGI(TAG, "auto-detect: each network's own wait (xprs %lus, "
                      "meshtastic %lus, meshcore %lus), and each ends the "
                      "moment it answers",
                 (unsigned long)(k_modes[XPRSLORA_MODE_XPRS].detect_ms / 1000u),
                 (unsigned long)(k_modes[XPRSLORA_MODE_MESHTASTIC].detect_ms / 1000u),
                 (unsigned long)(k_modes[XPRSLORA_MODE_MESHCORE].detect_ms / 1000u));
    return ESP_OK;
}

esp_err_t xprslora_survey_start(uint32_t per_mode_s)
{
    return survey_begin(per_mode_s, false);
}

esp_err_t xprslora_detect_start(uint32_t per_mode_s)
{
    return survey_begin(per_mode_s, true);
}

/* On the bearer tick: move to the next mode when this one's time is up, and
 * go home when there is none left. */
/* How long this mode gets: what the operator asked for, else the mode's
 * own ceiling. */
static uint32_t survey_dwell_ms(void)
{
    if (s_survey.per_ms) return s_survey.per_ms;
    return s_def->detect_ms ? s_def->detect_ms : 20000u;
}

static void survey_tick(void)
{
    if (!s_survey.active) return;
    uint32_t now = lr_now_ms();
    const xprslora_survey_mode_t *cur = &s_survey.m[s_mode];
    /* A repeater carrying our probe ends this mode: that is the whole
     * question asked, and the rest of the dwell adds nothing. Hearing
     * somebody ELSE's frame does not end it, though it proves the network
     * is alive: on the bench a stray Meshtastic frame arrived 3.5 s in and
     * cut the dwell off while the repeater was still inside its own
     * backoff, which at LongFast reaches 7.6 s for a CLOSE node and so
     * threw away the answer that mattered (measured 2026-09-20). Only
     * auto-detect leaves early -- a survey counts traffic over a window,
     * and its number means nothing if the window moves. */
    bool answered = s_survey.probe && cur->relayed;
    uint32_t in_mode = now - s_survey.started_ms;
    /* The probe is not aired the instant the modem is retuned: the chip is
     * settling and the neighbour's receiver is where the answer has to
     * come from. It goes out once the mode has stood still for a moment,
     * and once more halfway through, because one packet can always meet
     * another one. */
    if (s_survey.probe && !answered && s_def->net != LR_NET_NONE) {
        uint32_t dwell = survey_dwell_ms();
        if ((s_survey.probes == 0 && in_mode >= MC_SETTLE_MS) ||
            (s_survey.probes == 1 && in_mode >= dwell / 2)) {
            lr_lock(NULL);
            survey_probe();
            lr_unlock(NULL);
            s_survey.probes++;
        }
    }
    if (!answered && in_mode < survey_dwell_ms()) return;

    xprslora_survey_mode_t *m = &s_survey.m[s_mode];
    ESP_LOGI(TAG, "survey: %s %lu frame%s, %d named%s%s%s", s_def->name,
             (unsigned long)m->frames, m->frames == 1 ? "" : "s", m->names,
             m->names ? " -- " : "", m->names ? m->name[0] : "",
             m->relayed ? ", and a repeater carried ours"
                        : m->probed ? ", nobody carried ours" : "");
    xprslora_mode_t next = survey_next(s_mode);
    lr_lock(NULL);
    if (next < XPRSLORA_MODE_COUNT) {
        s_survey.started_ms = now;
        s_survey.probes = 0;
        lr_tune_mode(next, false);
        lr_unlock(NULL);
        return;
    }
    /* Done: back where we were, and the ledger with it. */
    lr_tune_mode(s_survey.home, true);
    xb_set_pace(s_lora, s_def->pace_ms);
    xb_set_duty(s_lora, &s_duty, lr_airtime, NULL, s_region->duty_ms,
                s_region->reserve_ms, s_region->dwell_ms);
    s_survey.active = false;
    s_survey.done = true;
    /* What the asking cost, charged to the hour now that the ledger the
     * band owns is back: a probe is our transmission like any other. */
    if (s_survey.probe_ms) {
        xb_spend(s_lora, s_survey.probe_ms, false);
        ESP_LOGI(TAG, "auto-detect: %lums of our own airtime, charged",
                 (unsigned long)s_survey.probe_ms);
    }
    lr_unlock(NULL);
    /* The sweep passed through MeshCore, which may have claimed its block
     * on the way; the mode we came home to decides whether it is kept.
     * Outside the radio lock, like every other release. */
    lr_mc_release();
    ESP_LOGI(TAG, "survey: done, back in %s mode", s_def->name);
}

bool xprslora_survey_active(void)
{
    return s_survey.active;
}

int xprslora_survey_json(char *buf, size_t cap)
{
    if (!buf || cap < 32 || (!s_survey.active && !s_survey.done)) return 0;
    int n = snprintf(buf, cap, "{\"running\":%s,\"modes\":{",
                     s_survey.active ? "true" : "false");
    bool first = true;
    for (int i = 0; i < XPRSLORA_MODE_COUNT && n > 0 && (size_t)n < cap; i++) {
        if (!xprslora_mode_available((xprslora_mode_t)i)) continue;
        n += snprintf(buf + n, cap - (size_t)n, "%s\"%s\":{\"frames\":%lu,\"heard\":[",
                      first ? "" : ",", k_modes[i].name,
                      (unsigned long)s_survey.m[i].frames);
        first = false;
        for (int j = 0; j < s_survey.m[i].names && (size_t)n < cap; j++)
            n += snprintf(buf + n, cap - (size_t)n, "%s\"%s\"", j ? "," : "",
                          s_survey.m[i].name[j]);
        if ((size_t)n < cap)
            n += snprintf(buf + n, cap - (size_t)n,
                          "],\"asked\":%s,\"relayed\":%s}",
                          s_survey.m[i].probed ? "true" : "false",
                          s_survey.m[i].relayed ? "true" : "false");
    }
    if (n > 0 && (size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, "}}");
    return n;
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

void xprslora_modem(int *sf, uint32_t *bw_hz)
{
    if (sf) *sf = s_radio ? (int)s_air.sf : 0;
    if (bw_hz) *bw_hz = s_radio ? s_air.bw_hz : 0;
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
    if (s_def->net != LR_NET_MT) return ESP_ERR_NOT_SUPPORTED; /* not allocated */
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
    if (!s_st || !s_st->mesh_on || s_def->net != LR_NET_MT) return;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    mt_mesh_on_xprs(&s_st->mesh, wire, len, origin);
    xSemaphoreGiveRecursive(s_mt_mutex);
}

bool xprslora_mt_stats(mt_mesh_stats_t *out)
{
    /* False when Meshtastic is not the running mode, so what reads this --
     * the `serve:` word, the status block -- says what is true now. */
    if (!s_st || !out || s_def->net != LR_NET_MT) return false;
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

/* ── MeshCore ────────────────────────────────────────────────────────── */

/* The same hooks the Meshtastic bridge is given (xprslora_mt_hooks_t), and
 * the same NVS shape, under a namespace of its own: a MeshCore contact is
 * a key, a Meshtastic one is a node number, and the two must never be read
 * as each other. */
static int mc_blob_load(const char *key, void *buf, int cap)
{
    nvs_handle_t h;
    if (nvs_open("xprsmc", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t n = (size_t)cap;
    esp_err_t err = nvs_get_blob(h, key, buf, &n);
    nvs_close(h);
    return err == ESP_OK ? (int)n : 0;
}

static void mc_blob_save(const char *key, const void *buf, int len)
{
    nvs_handle_t h;
    if (nvs_open("xprsmc", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, key, buf, (size_t)len) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static int mc_keys_load(void *ctx, void *buf, int cap)
{
    (void)ctx;
    return mc_blob_load("keys", buf, cap);
}

static void mc_keys_save(void *ctx, const void *buf, int len)
{
    (void)ctx;
    mc_blob_save("keys", buf, len);
}

static int mc_vnodes_load(void *ctx, void *buf, int cap)
{
    (void)ctx;
    return mc_blob_load("vnodes", buf, cap);
}

static void mc_vnodes_save(void *ctx, const void *buf, int len)
{
    (void)ctx;
    mc_blob_save("vnodes", buf, len);
}

/* The worker: the half of the bridge that costs stack (mc_mesh_work).
 * Fifty milliseconds is well inside what MeshCore's own timers want and
 * far below a frame's airtime at SF11. */
static void mc_worker(void *arg)
{
    (void)arg;
    s_mc_worker_live = true;
    while (!s_mc_worker_stop) {
        xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
        if (s_mc && s_mc->mesh_on) mc_mesh_work(&s_mc->mesh, lr_now_ms());
        xSemaphoreGiveRecursive(s_mt_mutex);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* Said before the flag drops: whoever is waiting frees the state the
     * moment it does. */
    ESP_LOGI(TAG, "MeshCore worker stopped");
    s_mc_worker_live = false;
    vTaskDelete(NULL);
}

/* Stop it and get its stack back. True when it is gone (or never ran);
 * false when it would not leave, which means its state must be kept. */
static bool mc_worker_stop(void)
{
    if (!s_mc_worker) return true;
    s_mc_worker_stop = true;
    for (int i = 0; i < 40 && s_mc_worker_live; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_mc_worker_live) {
        ESP_LOGW(TAG, "MeshCore worker did not stop");
        s_mc_worker_stop = false;
        return false;
    }
    s_mc_worker = NULL;
    return true;
}

static bool mc_worker_start(void)
{
    if (s_mc_worker) return true;
    s_mc_worker_stop = false;
    /* XPRS_WORK_CORE, not a bare 1: on the C3 there is no core 1 and
     * asking for it is an abort before the station says a word
     * (docs/esp32.md, "The ESP32-C3"). */
    if (xTaskCreatePinnedToCore(mc_worker, "mcwork", 6144, NULL, 2,
                                &s_mc_worker, XPRS_WORK_CORE) == pdPASS)
        return true;
    s_mc_worker = NULL;
    /* A stack is one contiguous piece of INTERNAL heap, and a mode changed
     * hours into a run asks for it from a heap that is no longer whole.
     * Say what was there, because "the bridge did not start" on its own
     * sends the next person looking at the radio (docs/esp32.md, "The boot
     * order is the allocator"). */
    ESP_LOGE(TAG, "MeshCore worker: 6144 bytes of stack refused, internal "
                  "free %u, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
}

esp_err_t xprslora_mc_start(const xprslora_mt_hooks_t *hooks,
                            const mc_mesh_cfg_t *cfg, const char *nick)
{
    if (!s_lora || !hooks || !cfg) return ESP_ERR_INVALID_STATE;
    if (s_def->net != LR_NET_MC) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    bool have = s_mc != NULL, already = have && s_mc->mesh_on;
    xSemaphoreGiveRecursive(s_mt_mutex);
    if (!have) return ESP_ERR_NOT_SUPPORTED;
    if (already) return ESP_OK;
    s_hooks = *hooks;
    mc_mesh_ops_t ops = {
        .air = lr_air_mc, .now_ms = lr_now_ms, .random = lr_random,
        .deliver = mt_deliver, .stamp = mt_stamp, .nick_of = mt_nick,
        .log = mt_log, .keys_load = mc_keys_load, .keys_save = mc_keys_save,
        .vnodes_load = mc_vnodes_load, .vnodes_save = mc_vnodes_save,
        .utc_now = mt_utc,
        .ctx = NULL,
    };
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    mc_mesh_init(&s_mc->mesh, &ops, cfg, s_lora->call, nick);
    s_mc->mesh_on = true;
    xSemaphoreGiveRecursive(s_mt_mutex);
    /* And the task that does the arithmetic. A task that fails to start
     * says nothing unless it is asked (docs/esp32.md), and a bridge
     * without this one would repeat nothing and sign nothing, so the
     * failure is the bridge's failure. */
    if (!mc_worker_start()) {
        xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
        s_mc->mesh_on = false;
        xSemaphoreGiveRecursive(s_mt_mutex);
        ESP_LOGE(TAG, "no room for the MeshCore worker (6 KB of stack) -- "
                      "XPRS still runs on this radio, MeshCore does not");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MeshCore: repeater %s, bridge %s, %u broadcasts an hour, "
                  "advert every %u min",
             cfg->repeat ? "on" : "off", cfg->bridge ? "on" : "off",
             (unsigned)cfg->bcast_per_hour, (unsigned)cfg->advert_min);
    return ESP_OK;
}

void xprslora_mc_offer(const char *wire, int len, int origin)
{
    if (s_def->net != LR_NET_MC) return;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    if (s_mc && s_mc->mesh_on) mc_mesh_on_xprs(&s_mc->mesh, wire, len, origin);
    xSemaphoreGiveRecursive(s_mt_mutex);
}

bool xprslora_mc_stats(mc_mesh_stats_t *out)
{
    if (!out || s_def->net != LR_NET_MC) return false;
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    bool on = s_mc != NULL;
    if (on) *out = s_mc->mesh.st;
    xSemaphoreGiveRecursive(s_mt_mutex);
    return on;
}

int xprslora_mc_node(int i, mc_node_t *out)
{
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    const mc_node_t *n = NULL;
    int count = s_mc ? mc_mesh_node(&s_mc->mesh, i, &n) : 0;
    if (n && out) *out = *n;
    xSemaphoreGiveRecursive(s_mt_mutex);
    return count;
}

void xprslora_mc_pause(bool quiet)
{
    /* An install wants the RAM and the cache more than MeshCore wants its
     * adverts on time (docs/esp32.md, "Quiesce means hand resources back").
     * Six kilobytes of stack go back for the length of the transfer; the
     * bridge's own state stays, so nothing is forgotten. */
    if (!s_mc || !s_mc->mesh_on) return;
    if (quiet) {
        if (mc_worker_stop())
            ESP_LOGW(TAG, "MeshCore worker stood down for the install");
    } else if (mc_worker_start()) {
        ESP_LOGI(TAG, "MeshCore worker back");
    }
}

void xprslora_mc_set_nick(const char *nick)
{
    xSemaphoreTakeRecursive(s_mt_mutex, portMAX_DELAY);
    if (s_mc && s_mc->mesh_on) mc_mesh_set_nick(&s_mc->mesh, nick);
    xSemaphoreGiveRecursive(s_mt_mutex);
}
