/*
 * An XPRS station on a SenseCAP Card Tracker T1000-E: a card a person carries.
 *
 * What it is for. It goes where its person goes, listens on Bluetooth for the
 * XPRS phones around it and on LoRa for whichever mesh is in the area
 * (Meshtastic or MeshCore), carries XPRS between the two, beacons where it
 * is, and holds mail for the people it meets. It runs on 700 mAh and has to
 * last three days, which shapes every number in this file.
 *
 * HOW IT SHARES WITH THE REST OF THE FLEET. The wire format, the relay
 * decision and the signing are common/ (xprs_codec, xprs_bearer, xprs_sig,
 * xprs_id, xprs_auth), symlinked into lib/ exactly as on the P1-Pro, the one
 * other nRF52840 board (models/sensecap-p1-pro, whose station code this file
 * started from). XPRS rides on LoRa inside the framing the local mesh already
 * carries: common/xprs_meshtastic (mt_xprs_wrap: a clear Data frame on
 * portnum 0x158) or common/xprs_meshcore (mc_xprs_wrap: a RAW_CUSTOM flood),
 * the same code the ESP32 stations run in their meshtastic, meshcore and
 * both modes (docs/lora.md).
 */

#include <Arduino.h>
#include <time.h>
#include <SPI.h>
#include <RadioLib.h>

#include "board.h"

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

extern "C" {
#include "xprs.h"
#include "xprsbearer.h"
#include "xb_airtime.h"
#include "xprssig.h"
#include "xprsid.h"
#include "xprs_auth.h"
#include "xprs_blob.h"
#include "bech32.h"
#include "tinynimble.h"
#include "tn_att.h"
#include "mt.h"
#include "mc.h"
#include "xlc.h"
#include "lr_detect.h"
#include "lr_probe.h"
#include "lr_repeat.h"
#include "lr_worth.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
}
#include "update.h"
#include "nrf_station.h"
#include "sensors.h"
#include "gnss.h"
#include "mail.h"
#include <math.h>
using namespace Adafruit_LittleFS_Namespace;

#define FW_BOARD "sensecap-t1000e"

/* ── The LoRa networks this card can live on ────────────────────────────
 *
 * Two, because those are the meshes people actually carry: Meshtastic's
 * LongFast and MeshCore's default. XPRS travels inside each one's own
 * framing, so stock nodes of that network carry it (Meshtastic routers flood
 * it by header) or at least do not trip over it, and XPRS stations in any
 * mode that serves that network unwrap it (docs/lora.md, rule 15).
 *
 * The figures are the ones xprslora.c uses, so this card and an ESP32
 * station on the same network are on the same channel:
 *   meshtastic  LongFast: SF11, 250 kHz, preamble 16, sync 0x2B, frequency
 *               from Meshtastic's own slot rule (mt_slot_freq_hz)
 *   meshcore    SF8, 62.5 kHz, preamble 16, sync 0x12, 869.618 MHz in EU
 *               (mc.h, measured off a stock node)
 * Coding rate 4/5 on both; RadioLib spells it as its denominator, 5. */
enum { NET_MT = 0, NET_MC = 1, NET_N = 2 };
typedef struct {
    const char *name;
    float       freq_mhz;
    float       bw_khz;
    uint8_t     sf;
    uint8_t     sync;
    uint16_t    preamble;
} lora_net_t;
static lora_net_t s_nets[NET_N] = {
    { "meshtastic", 869.525f, MT_LF_BW_HZ / 1000.0f, MT_LF_SF, MT_LF_SYNC, MT_LF_PREAMBLE },
    { "meshcore",   869.618f, MC_BW_HZ / 1000.0f,    MC_SF,    MC_SYNC,    MC_PREAMBLE    },
};
static int s_net = NET_MT;          /* the one we are on; detection decides */
static_assert((int)NET_MT == (int)LRD_MT && (int)NET_MC == (int)LRD_MC && (int)NET_N == (int)LRD_N,
              "lr_detect numbers the networks as this card does");

/* +20 dBm on the LR1110's high-power PA: the user's call (range from a
 * body-worn antenna). 869.4-869.65 MHz is ERC 70-03 band g3, 10% duty and
 * 27 dBm e.r.p., so this is within it; the duty ledger holds the 10%. */
#define LORA_POWER_DBM   20
#define LORA_CR          5
#define LORA_PACE_MS     3000

/* Duty-cycled receive: the LR1110 wakes, looks for a preamble for this many
 * symbols, and sleeps again for the rest of a sender's 16-symbol preamble.
 * RadioLib's default (0, meaning 8) against a 16-symbol preamble leaves a
 * sleep shorter than the radio's own transition time, and startReceive-
 * DutyCycleAuto then quietly falls back to continuous receive: the card
 * would burn the whole RX current and nothing would say so. 4 keeps it
 * awake about 38% of the time on either network. */
#ifndef LORA_RX_MIN_SYMBOLS
#define LORA_RX_MIN_SYMBOLS 4
#endif

/* Which network is around (lr_detect.h): at boot Meshtastic for up to 20 s
 * then MeshCore for up to 8 s (a MeshCore repeater answers within 2 s, a
 * Meshtastic one within 7.6 s, docs/lora.md section 4); then every hour the
 * other network and back, moving only after two sweeps agree. */
#define DETECT_SETTLE_MS  1200
#define DETECT_EVERY_MS   3600000UL

/* How long the station task sleeps when nothing is due. Every event that
 * matters wakes it sooner: a LoRa packet, a SoftDevice event. */
#define LOOP_IDLE_MS      250
#define LOOP_BUSY_MS      20

#ifndef BEACON_EVERY_SEC
#define BEACON_EVERY_SEC 300
#endif
#ifndef BEACON_JITTER_SEC
#define BEACON_JITTER_SEC 30
#endif

/* ── BLE5 framing (common/xprs_bearer_ble/xprsble.h) ──────────────────── */
#define BLE_COMPANY_LO 0xFF
#define BLE_COMPANY_HI 0xFF
#define BLE_MARKER     0x3E
#define BLE_SUB_XPRS   0x58
#define BLE_WIRE_MAX   (TN_ADV_DATA_MAX - 6)

static xb_t     s_ble;
static bool     s_ble_up;
static uint8_t  s_peer_addr[6], s_peer_addr_type;
static bool     s_peer_known;
static char     s_peer_call[16];

/* ── The radio ───────────────────────────────────────────────────────────
 *
 * The LR1110 drives its own antenna switch from DIO5..DIO8; the table is
 * Seeed's, as Meshtastic carries it (variants/.../tracker-t1000-e/
 * rfswitch.h). A wrong row here is silent: the radio reports success and
 * nothing leaves the antenna. */
static const uint32_t k_rf_pins[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
    RADIOLIB_LR11X0_DIO7, RADIOLIB_LR11X0_DIO8, RADIOLIB_NC,
};
static const Module::RfSwitchMode_t k_rf_table[] = {
    { LR11x0::MODE_STBY,  { LOW,  LOW,  LOW,  LOW  } },
    { LR11x0::MODE_RX,    { HIGH, LOW,  LOW,  HIGH } },
    { LR11x0::MODE_TX,    { HIGH, HIGH, LOW,  HIGH } },
    { LR11x0::MODE_TX_HP, { LOW,  HIGH, LOW,  HIGH } },
    { LR11x0::MODE_TX_HF, { LOW,  LOW,  LOW,  LOW  } },
    { LR11x0::MODE_GNSS,  { LOW,  LOW,  HIGH, LOW  } },
    { LR11x0::MODE_WIFI,  { LOW,  LOW,  LOW,  LOW  } },
    END_OF_MODE_TABLE,
};

static SPIClass        s_spi(NRF_SPIM2, T1_LR_MISO, T1_LR_SCK, T1_LR_MOSI);
static LR1110          s_radio = new Module(T1_LR_NSS, T1_LR_IRQ, T1_LR_RESET,
                                            T1_LR_BUSY, s_spi);
static xb_t            s_lora;
static volatile bool   s_rx_pending;
static bool            s_radio_up;
static uint32_t        s_heard;
static uint32_t        s_self_node;      /* our Meshtastic node number */
static mt_reasm_t      s_mt_reasm;
static mc_reasm_t      s_mc_reasm;
static uint32_t        s_native_frames;  /* frames of the mesh itself, not XPRS */
static TaskHandle_t    s_task;           /* the station task, woken by events */

/* The native repeater (lr_repeat.h), one per network: repeat only, no
 * bridge. The card carries the mesh it lives on; translating it into XPRS is
 * the job of a station on mains, with the flash for mt_mesh / mc_mesh and
 * the keys they need (X25519 and Ed25519 alone would not fit the slot). */
static lrr_t           s_rep[NET_N];
static bool            s_mesh_up;

static lrd_t           s_det;
static lrd_pick_t      s_pick = { NET_MT, NET_MT, 0 };
static lrp_probe_t     s_probe;
static bool            s_det_first = true;
static uint32_t        s_det_last_ms;
static lrd_ev_t        s_hour_ev[NET_N]; /* heard between sweeps, per network */

/* ── The key, the callsign, the config and the clock ────────────────────
 *
 * common/xprs_nrf52 (nrf_station.h), shared with the P1-Pro. The callsign
 * prefix is X2, a MOVABLE station (XPRS 3): "a ship, an aircraft, a bus, a
 * car... its position is a reading that expires". This card goes where a
 * person goes; X3 would tell every receiver its position is a place.
 *
 * format_if_full: a card fresh from the factory or back from Meshtastic
 * still holds that firmware's LittleFS in the same place and format. The
 * user's decision (2026-10-07) is to clear it when it will not take our key,
 * loudly; the Meshtastic identity is then gone if the card goes back. */
#define CALL_PREFIX "X2"

/* ── AES for the LoRa networks (common/xprs_loracrypto/xlc.h) ────────────
 *
 * Meshtastic's default channel is AES-128-CTR under a published key, which
 * is all detection needs (the probe and the names it hears). The nRF52840's
 * ECB block does AES-128 in hardware; through the SoftDevice once it is up,
 * directly before. 256-bit keys and the decrypt direction (MeshCore direct
 * messages) are not done here: this card relays those, it does not open
 * them, so it says no rather than pretend. */
extern "C" bool xlc_aes_encrypt_block(const uint8_t *key, int key_len,
                                      const uint8_t in[16], uint8_t out[16])
{
    if (key_len != 16) return false;
    nrf_ecb_hal_data_t e;
    memcpy(e.key, key, 16);
    memcpy(e.cleartext, in, 16);
    uint8_t sd_on = 0;
    sd_softdevice_is_enabled(&sd_on);
    if (sd_on) {
        if (sd_ecb_block_encrypt(&e) != NRF_SUCCESS) return false;
    } else {
        NRF_ECB->ECBDATAPTR = (uint32_t)&e;
        NRF_ECB->EVENTS_ENDECB = 0;
        NRF_ECB->EVENTS_ERRORECB = 0;
        NRF_ECB->TASKS_STARTECB = 1;
        while (!NRF_ECB->EVENTS_ENDECB && !NRF_ECB->EVENTS_ERRORECB) { }
        if (NRF_ECB->EVENTS_ERRORECB) return false;
    }
    memcpy(out, e.ciphertext, 16);
    return true;
}

extern "C" bool xlc_aes_decrypt_block(const uint8_t *key, int key_len,
                                      const uint8_t in[16], uint8_t out[16])
{
    (void)key; (void)key_len; (void)in; (void)out;
    return false;
}

/* ── Who we hear directly (15.6.3) ───────────────────────────────────── */
#define HEARS_MAX      12
#define HEARS_FRESH_MS 600000UL
static struct { char call[10]; uint32_t t_ms, ble_ms, lora_ms; } s_hears[HEARS_MAX];

static uint64_t peer_of(const char *wire, int len)
{
    xprs_t p; char from[10] = "";
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "f", from, sizeof from) || !from[0]) return 0;
    uint64_t h = 1469598103934665603ULL;
    for (const char *c = from; *c; c++) { h ^= (uint8_t)*c; h *= 1099511628211ULL; }
    return h ? h : 1;
}

static void hears_touch(const char *wire, int len, bool ble)
{
    xprs_t p; char from[10] = "";
    if (!xprs_parse(wire, len, &p) || xprs_via_count(&p) != 0) return;
    if (!xprs_get_str(&p, "f", from, sizeof from) || !from[0] || strcmp(from, nst_call()) == 0) return;
    uint32_t now = millis();
    int slot = -1, oldest = 0;
    for (int i = 0; i < HEARS_MAX; i++) {
        if (strcmp(s_hears[i].call, from) == 0 || !s_hears[i].call[0]) { slot = i; break; }
        if ((int32_t)(s_hears[i].t_ms - s_hears[oldest].t_ms) < 0) oldest = i;
    }
    if (slot < 0) slot = oldest;
    if (strcmp(s_hears[slot].call, from) != 0) s_hears[slot].ble_ms = s_hears[slot].lora_ms = 0;
    snprintf(s_hears[slot].call, sizeof s_hears[slot].call, "%s", from);
    s_hears[slot].t_ms = now;
    if (ble) s_hears[slot].ble_ms = now ? now : 1;
    else     s_hears[slot].lora_ms = now ? now : 1;
}

/* Heard directly, on any link, within [ms]. */
static bool heard_directly(const char *call, uint32_t ms)
{
    uint32_t now = millis();
    for (int i = 0; i < HEARS_MAX; i++)
        if (s_hears[i].call[0] && !strcmp(s_hears[i].call, call) && now - s_hears[i].t_ms <= ms)
            return true;
    return false;
}

/* Heard directly on that link in the last ten minutes. */
static bool hears_on(const char *call, bool ble)
{
    uint32_t now = millis();
    for (int i = 0; i < HEARS_MAX; i++) {
        uint32_t t = ble ? s_hears[i].ble_ms : s_hears[i].lora_ms;
        if (t && strcmp(s_hears[i].call, call) == 0 && now - t <= HEARS_FRESH_MS) return true;
    }
    return false;
}

static int hears_count(void)
{
    uint32_t now = millis(); int n = 0;
    for (int i = 0; i < HEARS_MAX; i++)
        if (s_hears[i].call[0] && now - s_hears[i].t_ms <= HEARS_FRESH_MS) n++;
    return n;
}

/* Most recent first, so a truncated list keeps the useful half (15.6.3). */
static int hears_render(char *out, int cap)
{
    uint32_t now = millis();
    bool used[HEARS_MAX] = {false};
    int n = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < HEARS_MAX; i++) {
            if (used[i] || !s_hears[i].call[0] || now - s_hears[i].t_ms > HEARS_FRESH_MS) continue;
            if (best < 0 || (int32_t)(s_hears[i].t_ms - s_hears[best].t_ms) > 0) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        int w = snprintf(out + n, (size_t)(cap - n), "%s%s", n ? "," : " hears:", s_hears[best].call);
        if (w <= 0 || n + w >= cap) { out[n] = 0; break; }
        n += w;
    }
    return n;
}

/* ── The LED: lit HIGH, used in blips only ───────────────────────────── */
static void led_blip(uint16_t ms)
{
    digitalWrite(T1_LED, HIGH);
    delay(ms);
    digitalWrite(T1_LED, LOW);
}

/* ── LoRa: tune, air, receive ───────────────────────────────────────────── */

static void station_wake_isr(void)
{
    if (!s_task) return;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static volatile uint32_t s_irqs;
static uint32_t s_rx_bad;
static void lr_isr(void) { s_irqs++; s_rx_pending = true; station_wake_isr(); }

/* tinynimble's hook: the SoftDevice has an event for the pump. */
extern "C" void tn_evt_isr(void) { station_wake_isr(); }

static bool s_rx_continuous;          /* console 'r': for measuring */
static void lora_listen(void)
{
    if (s_rx_continuous) { s_radio.startReceive(); return; }
    int st = s_radio.startReceiveDutyCycleAuto(s_nets[s_net].preamble, LORA_RX_MIN_SYMBOLS);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: duty-cycled receive refused (%d), listening continuously\n", st);
        s_radio.startReceive();
    }
}

/* Meshtastic's slot rule for the region: djb2("LongFast") over the band. */
static void nets_for_region(const char *region)
{
    const mt_region_t *r = mt_region_find(region);
    if (r) s_nets[NET_MT].freq_mhz = mt_slot_freq_hz(r, MT_CH_NAME_LONGFAST, MT_LF_BW_HZ) / 1e6f;
}

static bool lora_tune(int net)
{
    const lora_net_t *n = &s_nets[net];
    s_radio.standby();
    int st = s_radio.setFrequency(n->freq_mhz);
    if (st == RADIOLIB_ERR_NONE) st = s_radio.setBandwidth(n->bw_khz);
    if (st == RADIOLIB_ERR_NONE) st = s_radio.setSpreadingFactor(n->sf);
    if (st == RADIOLIB_ERR_NONE) st = s_radio.setCodingRate(LORA_CR);
    if (st == RADIOLIB_ERR_NONE) st = s_radio.setSyncWord(n->sync);
    if (st == RADIOLIB_ERR_NONE) st = s_radio.setPreambleLength(n->preamble);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: tune to %s failed (%d)\n", n->name, st);
        return false;
    }
    s_net = net;
    s_rx_pending = false;
    lora_listen();
    Serial.printf("lora: on %s %.3f MHz SF%u BW%.1f\n", n->name, n->freq_mhz, n->sf, n->bw_khz);
    return true;
}

static bool lora_tx_frame(const uint8_t *f, int len)
{
    int st = s_radio.transmit(f, (size_t)len);
    /* transmit() writes the frame's length into the LR1110's packet
     * parameters, and RadioLib's receive staging re-sends them only in
     * implicit-header mode. The LR1110 then takes that length as the
     * longest frame it will receive: after our 6-byte MeshCore probe the
     * card was deaf to the 7-byte relay of it, and to everything else
     * (measured 2026-10-07: the T-Deck aired the relay, the card heard
     * nothing). setPreambleLength() re-sends them with the 255-byte limit. */
    s_radio.setPreambleLength(s_nets[s_net].preamble);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: transmit failed (%d)\n", st);
        return false;
    }
    return true;
}

/* One XPRS wire on the air, in the network's own framing: one frame, or
 * two for a wire longer than one carries (mt 233 B, mc 183 B). */
static bool lora_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_radio_up) return false;
    static uint8_t frames[2][MT_FRAME_MAX];
    int flen[2] = {0, 0}, nf;
    if (s_net == NET_MT)
        nf = mt_xprs_wrap(wire, len, s_self_node, frames, flen);
    else
        nf = mc_xprs_wrap(wire, len, frames, flen);
    if (nf <= 0) { lora_listen(); return false; }
    bool ok = true;
    for (int i = 0; i < nf && ok; i++) ok = lora_tx_frame(frames[i], flen[i]);
    /* transmit() leaves the radio in standby, and its TxDone raised the
     * same IRQ line the receive path watches: listen again, THEN clear the
     * flag (the P1-Pro learned this one the hard way, main.cpp lora_air). */
    lora_listen();
    s_rx_pending = false;
    if (!ok) return false;
    Serial.printf("lora: aired %dB in %d frame%s on %s: %.*s\n", len, nf, nf > 1 ? "s" : "",
                  s_nets[s_net].name, len > 60 ? 60 : len, wire);
    led_blip(5);
    return true;
}

static uint32_t now_ms_fn(void) { return millis(); }
static uint32_t random_fn(void) { return (uint32_t)random(0x7FFFFFFF); }

static const xb_ops_t k_lora_ops = {
    .air = lora_air, .now_ms = now_ms_fn, .random = random_fn,
    .lock = NULL, .unlock = NULL, .drain = NULL, .ctx = NULL, .name = "lora",
};

/* What one XPRS wire costs in airtime on the current network: the sum of
 * its frames, header and framing included, so the ledger charges what the
 * channel actually carried. */
static xb_lora_air_t s_air;
static uint32_t lora_airtime_fn(int len, void *ctx)
{
    (void)ctx;
    uint32_t ms = 0;
    if (s_net == NET_MT) {
        for (int i = 0; i < mt_xprs_frames_for(len); i++)
            ms += xb_lora_airtime_ms(&s_air, mt_xprs_frame_len(len, i));
    } else {
        for (int i = 0; i < mc_xprs_frames_for(len); i++)
            ms += xb_lora_airtime_ms(&s_air, mc_xprs_frame_len(len, i));
    }
    return ms;
}

static void air_model_for(int net)
{
    s_air.bw_hz = (uint32_t)(s_nets[net].bw_khz * 1000.0f);
    s_air.sf = s_nets[net].sf;
    s_air.cr = LORA_CR - 4;
    s_air.preamble = s_nets[net].preamble;
    s_air.crc = true;
    s_air.implicit_header = false;
}

/* A frame off the air: XPRS in either wrapping (a station in `both` mode
 * may use either), or the mesh's own traffic, which is repeated. Every frame
 * is also evidence for detection: during a sweep it goes to lr_detect,
 * between sweeps it is kept for the hourly verdict. */
static void lora_rx_frame(const uint8_t *f, int len, int rssi, int snr)
{
    static char wire[XPRS_MAX_WIRE + 8];
    bool echo = s_net == NET_MT ? lrp_mt_echo(&s_probe, f, len) : lrp_mc_echo(&s_probe, f, len);
    int n = mt_xprs_unwrap(&s_mt_reasm, f, len, millis(), wire, sizeof wire);
    if (n < 0) n = mc_xprs_unwrap(&s_mc_reasm, f, len, millis(), wire, sizeof wire);
    bool xprs = n >= 0;

    /* One of our own XPRS frames, carried on by a Meshtastic router: the
     * passive form of a relayed probe. */
    bool carried = echo;
    if (!carried && xprs && s_net == NET_MT) {
        mt_hdr_t h;
        carried = mt_hdr_parse(f, len, &h) && h.from == s_self_node && h.hop_limit < h.hop_start;
    }
    if (s_det.active) {
        lrd_on_frame(&s_det, carried, xprs);
    } else {
        lrd_ev_t *e = &s_hour_ev[s_net];
        if (e->frames < 0xFFFF) e->frames++;
        if (xprs && e->xprs < 0xFFFF) e->xprs++;
        if (carried) e->relayed = true;
    }
    if (!xprs || s_det.active || echo)
        Serial.printf("lora: %s frame %dB %d dBm SNR %d on %s%s\n", xprs ? "xprs" : "native", len,
                      rssi, snr, s_nets[s_net].name, echo ? " -- our probe, carried" : "");
    if (echo) return;

    if (!xprs) s_native_frames++;
    if (s_mesh_up && !s_det.active) {
        lrr_t *r = &s_rep[s_net];
        if (xprs) lrr_note(r, f, len, millis());
        else      lrr_on_frame(r, f, len, snr, xb_lora_airtime_ms(&s_air, len), millis(), random_fn());
    }
    if (n > 0) {
        wire[n] = 0;
        xb_on_wire(&s_lora, wire, n, peer_of(wire, n), rssi);
    }
}

/* ── The native mesh: repeat what it floods ─────────────────────────── */

/* The repeater's frame due now, on our network only. The channel is looked
 * at first and the ledger is shared with XPRS (xb_spend), so the card has
 * one airtime budget whoever is talking; a native repeat never spends the
 * sos reserve. "Not now" leaves the frame queued for a little later. */
static void mesh_tick(void)
{
    if (!s_mesh_up || !s_radio_up) return;
    lrr_t *r = &s_rep[s_net];
    uint32_t now = millis();
    int i = lrr_due(r, now);
    if (i < 0) return;
    const uint8_t *f = r->q[i].frame;
    int len = r->q[i].len;
    bool ok = false;
    bool free = s_radio.scanChannel() == RADIOLIB_CHANNEL_FREE;
    s_rx_pending = false;               /* CAD done raised the same IRQ line */
    if (free &&
        xb_spend(&s_lora, xb_lora_airtime_ms(&s_air, len), false)) {
        ok = lora_tx_frame(f, len);
        if (ok) lrr_note(r, f, len, now);
    }
    lora_listen();
    s_rx_pending = false;
    lrr_aired(r, i, ok, millis(), random_fn());
}

static void mesh_begin(void)
{
    /* Our MeshCore path byte: stable, ours, and never 0x00 or 0xFF, which
     * MeshCore keeps for itself. */
    uint8_t h[3] = { (uint8_t)s_self_node, (uint8_t)(s_self_node >> 8), (uint8_t)(s_self_node >> 16) };
    if (h[0] == 0x00 || h[0] == 0xFF) h[0] ^= 0x5A;
    lrr_init(&s_rep[NET_MT], NET_MT, s_self_node, NULL);
    lrr_init(&s_rep[NET_MC], NET_MC, s_self_node, h);
    s_mesh_up = true;
}

static bool mesh_busy(uint32_t now)
{
    return s_mesh_up && lrr_busy(&s_rep[s_net], now);
}

/* ── Which network: the sweep and the verdict (lr_detect.h) ─────────── */
static const lrd_timing_t k_det_timing = { DETECT_SETTLE_MS, { 20000, 8000 } };

static void net_go(int net)
{
    if (s_mesh_up && net != s_net) lrr_flush(&s_rep[s_net]);
    lora_tune(net);
    air_model_for(net);
}

static void detect_start(bool first)
{
    uint8_t order[LRD_N];
    int n = 0;
    if (first) {
        order[n++] = NET_MT;
        order[n++] = NET_MC;
    } else {
        /* The other network, then back to ours, so ours is asked too: an
         * hour of its traffic says it is alive, not that it carries us. */
        order[n++] = s_pick.current == NET_MT ? NET_MC : NET_MT;
        order[n++] = s_pick.current;
    }
    s_det_first = first;
    memset(&s_probe, 0, sizeof s_probe);
    lrd_begin(&s_det, order, n, &k_det_timing, millis());
    Serial.printf("detect: %s sweep\n", first ? "boot" : "hourly");
}

static void detect_probe(void)
{
    uint8_t f[MT_FRAME_MAX > MC_FRAME_MAX ? MT_FRAME_MAX : MC_FRAME_MAX];
    int n = s_net == NET_MT ? lrp_mt_build(&s_probe, s_self_node, random_fn(), f, sizeof f)
                            : lrp_mc_build(&s_probe, random_fn(), f, sizeof f);
    if (n <= 0) { lrd_probed(&s_det); return; }
    /* A busy channel: ask again on the next tick, it is still due. */
    int cad = s_radio.scanChannel();
    s_rx_pending = false;               /* CAD done raised the same IRQ line */
    if (cad != RADIOLIB_CHANNEL_FREE) {
        static uint32_t s_cad_busy;
        if ((s_cad_busy++ % 50) == 0) Serial.printf("detect: channel busy (%d)\n", cad);
        lora_listen();
        return;
    }
    lrd_probed(&s_det);
    if (!xb_spend(&s_lora, xb_lora_airtime_ms(&s_air, n), false)) { lora_listen(); return; }
    lora_tx_frame(f, n);
    lora_listen();
    s_rx_pending = false;
    Serial.printf("detect: asked %s with %d bytes\n", s_nets[s_net].name, n);
}

static void detect_done(void)
{
    lrd_ev_t ev[LRD_N];
    memcpy(ev, s_det.ev, sizeof ev);
    if (!s_det_first) {
        /* What the hour on our own network showed counts too. */
        lrd_ev_t *e = &ev[s_pick.current], *h = &s_hour_ev[s_pick.current];
        e->frames = (uint16_t)(e->frames + h->frames < 0xFFFF ? e->frames + h->frames : 0xFFFF);
        e->xprs = (uint16_t)(e->xprs + h->xprs < 0xFFFF ? e->xprs + h->xprs : 0xFFFF);
        e->relayed = e->relayed || h->relayed;
    }
    uint8_t was = s_pick.current;
    uint8_t net = lrd_adopt(&s_pick, ev, s_det_first);
    for (int i = 0; i < NET_N; i++)
        Serial.printf("detect: %s frames=%u xprs=%u probes=%u relayed=%d rank=%d\n",
                      s_nets[i].name, ev[i].frames, ev[i].xprs, ev[i].probes,
                      (int)ev[i].relayed, lrd_rank(&ev[i]));
    if (net != was || s_det_first)
        Serial.printf("detect: on %s%s\n", s_nets[net].name, net != was ? " (moved)" : "");
    else if (s_pick.strikes)
        Serial.printf("detect: staying on %s, %s looked better %u time(s)\n",
                      s_nets[net].name, s_nets[s_pick.wanted].name, s_pick.strikes);
    memset(s_hour_ev, 0, sizeof s_hour_ev);
    mail_net_set(net);
    s_det_last_ms = millis();
    s_det_first = false;
    net_go(net);
}

static void detect_tick(void)
{
    if (!s_radio_up) return;
    uint32_t now = millis();
    if (!s_det.active) {
        if (lrd_due(now, s_det_last_ms, DETECT_EVERY_MS, mesh_busy(now)))
            detect_start(false);
        return;
    }
    uint8_t net = 0;
    switch (lrd_tick(&s_det, now, &net)) {
    case LRD_TUNE:  net_go(net); break;
    case LRD_PROBE: detect_probe(); break;
    case LRD_DONE:  detect_done(); break;
    default: break;
    }
}

/* ── What we hear ───────────────────────────────────────────────────── */
static void heard(const char *link, const char *wire, int len, int rssi)
{
    s_heard++;
    Serial.printf("%-6s rx %4d dBm %3dB %.*s\n", link, rssi, len, len > 90 ? 90 : len, wire);
}

static bool command_for_us(xb_t *b, const char *wire, int len)
{
    xprs_t p; char t[12] = "", d[16] = "";
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "t", t, sizeof t) || strcmp(t, "command") != 0) return false;
    if (!xprs_get_str(&p, "d", d, sizeof d) || strcmp(d, nst_call()) != 0) return false;
    nst_clock_learn(&p);
    xfw_handle(b, &p);
    return true;
}

/* ── Where the card is ───────────────────────────────────────────────
 *
 * A phone in the pocket next to the card already knows where they both are,
 * and asking it costs a few milliseconds of Bluetooth; the card's own
 * receiver costs about 30 mA for as long as a fix takes. So: ask an owner's
 * phone in reach first (t:request q:pos, section 8), and take the answer
 * only from that phone, signed by it, within two minutes, while the phone
 * itself is heard directly on Bluetooth (so it is really next to us; the
 * answer may come back through a station that re-aired it). Only when there is no such
 * phone, or it does not answer, the receiver is powered for one fix.
 *
 * A position older than its freshness is not said at all: absence of pos:
 * means unknown (15.1), and there is no key for "this is how old it is". Two
 * fixes in the same place make the card "still", which stretches both the
 * freshness and the receiver's period; any position elsewhere ends it. */
#define POS_REFRESH_MS      (15 * 60000UL)
#define POS_STILL_REFRESH_MS (60 * 60000UL)
#define POS_FRESH_MS        (20 * 60000UL)
#define POS_STILL_FRESH_MS  (75 * 60000UL)
/* A phone advertises 5 s of every minute (the app's Ble5.kt), so its answer
 * can wait up to a minute for its next window, plus the fix: measured
 * 87 s on the C61 (2026-10-07). */
#define POS_ASK_WAIT_MS     120000UL
/* After boot the scan has not met the phone yet; give it this long before
 * paying for the receiver. */
#define POS_BOOT_GRACE_MS   120000UL
#define POS_ASK_EVERY_MS    (10 * 60000UL)
#define PHONE_NEAR_MS       180000UL       /* heard on Bluetooth this recently */
#define GNSS_HOT_MS         45000UL
#define GNSS_COLD_MS        120000UL
#define GNSS_GOOD_FIXES     3              /* valid sentences before taking one */
#define STILL_M             50

static char     s_pos[32], s_acc[12];
static uint32_t s_pos_ms;
static bool     s_still;
static char     s_ask_to[12];
static uint32_t s_ask_ms, s_last_ask_ms;
static uint32_t s_gnss_on_ms, s_gnss_last_ms;
static uint16_t s_gnss_good;
static bool     s_gnss_fixed_once;
static int32_t  s_last_lat, s_last_lon;
static bool     s_have_last;

static bool pos_fresh(void)
{
    return s_pos[0] && millis() - s_pos_ms < (s_still ? POS_STILL_FRESH_MS : POS_FRESH_MS);
}

/* An owner of this card heard directly on Bluetooth lately. */
static const char *owner_in_reach(void)
{
    uint32_t now = millis();
    for (int i = 0; i < HEARS_MAX; i++)
        if (s_hears[i].ble_ms && now - s_hears[i].ble_ms < PHONE_NEAR_MS &&
            s_hears[i].call[0] == 'X' && s_hears[i].call[1] == '1' && xauth_is_owner(s_hears[i].call))
            return s_hears[i].call;
    return NULL;
}

static void ask_pos(const char *who)
{
    char wire[XPRS_MAX_WIRE + 1], tf[32];
    nst_time_field(tf, sizeof tf);
    int n = snprintf(wire, sizeof wire, "t:request f:%s d:%s %s q:pos", nst_call(), who, tf);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = nst_sign(wire, n, (int)sizeof wire);
    if (!s_ble_up || n <= 0) return;
    xb_send(&s_ble, wire, n);
    snprintf(s_ask_to, sizeof s_ask_to, "%s", who);
    s_ask_ms = s_last_ask_ms = millis();
    Serial.printf("pos: asked %s\n", who);
}

/* Is this the answer we asked for? Taken if so. */
static bool pos_answer(const char *wire, int len)
{
    xprs_t p;
    char t[16] = "", f[12] = "", sv[24] = "", pos[32] = "", acc[12] = "";
    if (!s_ask_to[0] || !xprs_parse(wire, len, &p)) return false;
    if (!xprs_get_str(&p, "t", t, sizeof t) || strcmp(t, "observation")) return false;
    if (!xprs_get_str(&p, "f", f, sizeof f) || strcmp(f, s_ask_to)) return false;
    if (!xprs_get_str(&p, "s", sv, sizeof sv) || !strstr(sv, "pos")) return false;
    /* An answer from the phone we asked: every refusal from here says why. */
    const char *why = NULL;
    uint8_t pub[32];
    if (!s_ask_ms || millis() - s_ask_ms > POS_ASK_WAIT_MS) why = "too late";
    /* A copy a station re-aired is still the owner's signed words. What
     * makes it OUR position is that the owner is next to us: heard directly
     * on Bluetooth these last three minutes, which is also when we asked. */
    else if (xprs_via_count(&p) != 0 && !(owner_in_reach() && !strcmp(owner_in_reach(), f)))
        why = "relayed, and the phone is not in reach";
    else if (!xprs_get_str(&p, "pos", pos, sizeof pos) || !strchr(pos, ',')) why = "no pos:";
    else if (!xauth_owner_key_of(f, pub)) why = "not an owner's key";
    else if (!xprsid_verify(&p, pub)) why = "does not verify";
    if (why) {
        Serial.printf("pos: answer from %s ignored: %s\n", f, why);
        return false;
    }
    xprs_get_str(&p, "acc", acc, sizeof acc);
    memcpy(s_pos, pos, sizeof s_pos);
    memcpy(s_acc, acc, sizeof s_acc);
    s_pos_ms = millis();
    s_ask_ms = 0;
    s_still = false;          /* the phone moves with its person; no claim of stillness */
    nst_clock_learn(&p);
    Serial.printf("pos: %s acc %s from %s\n", s_pos, s_acc[0] ? s_acc : "-", f);
    return true;
}

static void gnss_take(const nmea_fix_t *f)
{
    int acc = nmea_acc_m(f);
    int dec = acc && acc < 5 ? 5 : 4;
    int32_t div = dec == 5 ? 10 : 100;
    int32_t la = (f->lat_e6 + (f->lat_e6 < 0 ? -div / 2 : div / 2)) / div;
    int32_t lo = (f->lon_e6 + (f->lon_e6 < 0 ? -div / 2 : div / 2)) / div;
    int32_t scale = dec == 5 ? 100000 : 10000;
    snprintf(s_pos, sizeof s_pos, "%s%ld.%0*ld,%s%ld.%0*ld",
             la < 0 ? "-" : "", (long)(labs(la) / scale), dec, (long)(labs(la) % scale),
             lo < 0 ? "-" : "", (long)(labs(lo) / scale), dec, (long)(labs(lo) % scale));
    if (acc) snprintf(s_acc, sizeof s_acc, "%dm", acc);
    else s_acc[0] = 0;
    s_pos_ms = millis();
    if (s_have_last) {
        float dy = (float)(f->lat_e6 - s_last_lat) * 0.111f;
        float dx = (float)(f->lon_e6 - s_last_lon) * 0.111f * cosf((float)f->lat_e6 * 1.745329e-8f);
        s_still = sqrtf(dx * dx + dy * dy) < STILL_M;
    }
    s_last_lat = f->lat_e6;
    s_last_lon = f->lon_e6;
    s_have_last = true;
    s_gnss_fixed_once = true;
    if (f->utc) nst_clock_set(f->utc, "gnss");
    Serial.printf("pos: %s acc %s from the receiver, %u satellites, %lu s%s\n", s_pos,
                  s_acc[0] ? s_acc : "-", f->sats, (unsigned long)((millis() - s_gnss_on_ms) / 1000),
                  s_still ? ", still" : "");
}

static void gnss_start(void)
{
    gnss_power(true);
    s_gnss_on_ms = millis();
    s_gnss_good = 0;
    Serial.println("pos: receiver on");
}

static void gnss_stop(bool got)
{
    gnss_power(false);
    s_gnss_last_ms = millis();
    if (!got) Serial.printf("pos: no fix in %lu s, receiver off\n",
                            (unsigned long)((millis() - s_gnss_on_ms) / 1000));
}

static void pos_tick(void)
{
    uint32_t now = millis();
    if (gnss_is_on()) {
        gnss_poll();
        const nmea_fix_t *f = gnss_fix();
        static uint32_t seen;
        if (f->valid) {
            /* Count fresh sentences, not loop turns: RMC carries the time. */
            if (f->utc != seen) { seen = f->utc; s_gnss_good++; }
            if (s_gnss_good >= GNSS_GOOD_FIXES) { gnss_take(f); gnss_stop(true); }
        } else if (now - s_gnss_on_ms > (s_gnss_fixed_once ? GNSS_HOT_MS : GNSS_COLD_MS)) {
            gnss_stop(false);
        }
        return;
    }
    uint32_t every = s_still ? POS_STILL_REFRESH_MS : POS_REFRESH_MS;
    if (s_pos[0] && now - s_pos_ms < every) return;
    if (s_ask_ms && now - s_ask_ms < POS_ASK_WAIT_MS) return;      /* waiting */
    const char *owner = owner_in_reach();
    if (owner && (!s_last_ask_ms || now - s_last_ask_ms >= POS_ASK_EVERY_MS)) {
        ask_pos(owner);
        return;
    }
    if (now < POS_BOOT_GRACE_MS) return;
    if (!s_gnss_last_ms || now - s_gnss_last_ms >= every) gnss_start();
}

/* What Bluetooth hands to LoRa. A card in a room full of stations hears
 * their whole conversation, most of which is no business of a shared,
 * slow channel and every frame of which costs the battery a second of
 * transmitter. So, in this order:
 *  - only what is worth LoRa (docs/lora.md rule 11, the ESP32 stations'
 *    own list) and never what its author kept to the room (scope:local,
 *    XPRS.md 9.11.1);
 *  - not a packet for somebody heard directly on Bluetooth: it reached
 *    them already, the way it reached us;
 *  - a command, a result or a request only for somebody heard directly on
 *    LoRa: that is a phone here asking a station only LoRa reaches, which
 *    is the card's job. The rest is stations and phones keeping their
 *    archives in step, which they do on media of their own;
 *  - somebody's identity once in three hours, as our own goes out: a
 *    phone re-signs it every half hour, and the keys in it do not change;
 *  - not old news: a ts: more than half an hour behind now (or, with no
 *    clock, behind the newest ts: heard lately) is a carousel or a
 *    history replay, which stations re-say on their own media;
 *  - not twice in half an hour: the bearer forgets what it aired after a
 *    minute, and carousels come round again after that. */
#define LORA_FRESH_S      1800u
#define LORA_SENT_RING    24
#define LORA_SENT_MS      1800000UL
#define LORA_IDENT_MS     (3 * 3600000UL)
static struct { char call[10]; uint32_t t_ms; } s_lora_ident[8];
static struct { char id[XB_ID_LEN]; uint32_t t_ms; } s_lora_sent[LORA_SENT_RING];
static int      s_lora_sent_pos;
static uint32_t s_newest_ts, s_newest_ts_ms;

static bool ble_to_lora(const char *wire, int len)
{
    if (!lr_worth(wire, len, false) || lr_scope_local(wire, len)) return false;
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return false;
    char d[16] = "", ts[24] = "", t[16] = "", f[10] = "";
    xprs_get_str(&p, "t", t, sizeof t);
    xprs_get_str(&p, "d", d, sizeof d);
    if (d[0] && hears_on(d, true)) return false;
    if ((!strcmp(t, "command") || !strcmp(t, "result") || !strcmp(t, "request")) &&
        !(d[0] && hears_on(d, false)))
        return false;
    if (!strcmp(t, "identity") && xprs_get_str(&p, "f", f, sizeof f)) {
        uint32_t ms = millis();
        int slot = 0;
        for (int i = 0; i < 8; i++) {
            if (!strcmp(s_lora_ident[i].call, f)) {
                if (ms - s_lora_ident[i].t_ms < LORA_IDENT_MS) return false;
                slot = i;
                break;
            }
            if ((int32_t)(s_lora_ident[i].t_ms - s_lora_ident[slot].t_ms) < 0) slot = i;
        }
        snprintf(s_lora_ident[slot].call, sizeof s_lora_ident[slot].call, "%s", f);
        s_lora_ident[slot].t_ms = ms ? ms : 1;
    }
    if (xprs_get_str(&p, "ts", ts, sizeof ts)) {
        uint32_t t = nst_ts_to_epoch(ts), ms = millis();
        if (t && (!s_newest_ts || (int32_t)(t - s_newest_ts) > 0 ||
                  ms - s_newest_ts_ms > HEARS_FRESH_MS)) {
            s_newest_ts = t;
            s_newest_ts_ms = ms;
        }
        uint32_t now = nst_now();
        if (!now && s_newest_ts) now = s_newest_ts + (ms - s_newest_ts_ms) / 1000u;
        if (t && now && t + LORA_FRESH_S < now) return false;
    }
    char id[XB_ID_LEN];
    if (!xprs_id_of(wire, len, id)) return false;
    uint32_t ms = millis();
    for (int i = 0; i < LORA_SENT_RING; i++)
        if (s_lora_sent[i].t_ms && strcmp(s_lora_sent[i].id, id) == 0 &&
            ms - s_lora_sent[i].t_ms < LORA_SENT_MS)
            return false;
    snprintf(s_lora_sent[s_lora_sent_pos].id, XB_ID_LEN, "%s", id);
    s_lora_sent[s_lora_sent_pos].t_ms = ms ? ms : 1;
    s_lora_sent_pos = (s_lora_sent_pos + 1) % LORA_SENT_RING;
    return true;
}

static bool direct_of(const char *wire, int len)
{
    xprs_t p;
    return xprs_parse(wire, len, &p) && xprs_via_count(&p) == 0;
}

/* Questions for us (section 8): q:policy (11.9, answered to anybody: the
 * owners were set by cable, own1..own4) and our own readings. q:mail is
 * mail.cpp's. Answered on the bearer the question came on. */
static int readings(char *out, int cap, bool lean);
static bool answer_request(xb_t *b, const char *wire, int len)
{
    xprs_t p;
    char t[12] = "", d[16] = "", f[16] = "", q[48] = "";
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "t", t, sizeof t) || strcmp(t, "request") ||
        !xprs_get_str(&p, "d", d, sizeof d) || strcmp(d, nst_call()) ||
        !xprs_get_str(&p, "f", f, sizeof f) || !xprs_get_str(&p, "q", q, sizeof q))
        return false;
    /* An owner's signed question is as good a clock as its command: the
     * card has no other until a fix or a claim, and its packets say epoch:
     * meanwhile (nst_clock_learn verifies against the owner's key). */
    if (xauth_is_owner(f)) nst_clock_learn(&p);
    char out[XPRS_MAX_WIRE + 1], tf[32];
    nst_time_field(tf, sizeof tf);
    int n = 0;
    if (strstr(q, "policy")) {
        char owners[64] = "";
        int on = 0;
        /* ownN is the owner's npub (xprs_auth.c); ownNc, when set, the
         * callsign that goes with it, as the ESP32 keeps them. Without it the
         * callsign is the phone's own derivation, X1 and four key characters
         * (section 3). */
        for (int i = 1; i <= 4; i++) {
            char key[8], call[12];
            snprintf(key, sizeof key, "own%d", i);
            const char *npub = xcfg_get(key, "");
            if (!npub || strncmp(npub, "npub1", 5) || strlen(npub) < 9) continue;
            snprintf(key, sizeof key, "own%dc", i);
            const char *c = xcfg_get(key, "");
            if (c && c[0]) snprintf(call, sizeof call, "%s", c);
            else {
                snprintf(call, sizeof call, "X1%.4s", npub + 5);
                for (char *x = call; *x; x++) if (*x >= 'a' && *x <= 'z') *x -= 32;
            }
            on += snprintf(owners + on, sizeof owners - (size_t)on, "%s%s", on ? "," : "", call);
        }
        n = snprintf(out, sizeof out, "t:observation f:%s d:%s %s s:policy owner:%s use:all first:none serve:relay",
                     nst_call(), f, tf, owners[0] ? owners : "none");
    } else if (strstr(q, "pos") || strstr(q, "temp") || strstr(q, "batt")) {
        char r[96];
        int rn = readings(r, sizeof r, false);
        n = snprintf(out, sizeof out, "t:observation f:%s d:%s %s%.*s s:%s", nst_call(), f, tf, rn, r, q);
    } else {
        return false;
    }
    if (n <= 0 || n > XPRS_MAX_WIRE) return true;
    n = nst_sign(out, n, (int)sizeof out);
    if (n > 0) xb_send(b, out, n);
    return true;
}

static void on_lora(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    heard("lora", wire, len, rssi);
    hears_touch(wire, len, false);
    mail_heard(wire, len, &s_lora, direct_of(wire, len));
    if (command_for_us(&s_lora, wire, len)) return;
    if (answer_request(&s_lora, wire, len)) return;
    if (s_ble_up) xb_offer(&s_ble, wire, len);
}

static void on_ble(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    heard("ble", wire, len, rssi);
    hears_touch(wire, len, true);
    mail_heard(wire, len, &s_ble, direct_of(wire, len));
    if (command_for_us(&s_ble, wire, len)) return;
    if (answer_request(&s_ble, wire, len)) return;
    if (pos_answer(wire, len)) return;
    if (ble_to_lora(wire, len)) xb_offer(&s_lora, wire, len);
}

/* ── What we say ─────────────────────────────────────────────────────── */
/* The card's own readings, as section 15.3 spells them. */
static int readings(char *out, int cap, bool lean)
{
    int n = 0;
    if (pos_fresh()) {
        n += snprintf(out + n, (size_t)cap, " pos:%s", s_pos);
        if (s_acc[0] && n < cap) n += snprintf(out + n, (size_t)(cap - n), " acc:%s", s_acc);
    }
    float t;
    if (n < cap && sens_temp_c(&t)) {
        long d = lroundf(t * 10.0f);
        n += snprintf(out + n, (size_t)(cap - n), " temp:%s%ld.%ldC", d < 0 ? "-" : "",
                      labs(d) / 10, labs(d) % 10);
    }
    int mv = sens_batt_mv();
    if (n < cap) n += snprintf(out + n, (size_t)(cap - n), " batt:%d%%", sens_batt_pct(mv));
    if (!lean && n < cap) n += snprintf(out + n, (size_t)(cap - n), " volt:%d.%02dV", mv / 1000, mv % 1000 / 10);
    if (n < cap) n += snprintf(out + n, (size_t)(cap - n), " mail:%d", mail_count());
    return n < cap ? n : 0;
}

/* The beacon. On Bluetooth the whole of it; on LoRa a lean one, which a
 * shared channel hears from every card: no volt:, and only the most recent
 * few of hears: (they come most recent first, 15.6.3). link:ble is what
 * hears: was heard on in both: the card hears devices on Bluetooth. */
#define LEAN_HEARS_BYTES 48
static int beacon(char *out, int cap, bool lean)
{
    char tf[32];
    nst_time_field(tf, sizeof tf);
    int n = snprintf(out, (size_t)cap, "t:observation f:%s link:ble peers:%d %s",
                     nst_call(), hears_count(), tf);
    if (n <= 0 || n >= cap) return n;
    int room = cap - (5 + XPRSSIG_B85_LEN);
    n += readings(out + n, room - n, lean);
    int hcap = room - n;
    if (lean && hcap > LEAN_HEARS_BYTES) hcap = LEAN_HEARS_BYTES;
    n += hears_render(out + n, hcap);
    return nst_sign(out, n, cap);
}

static int lora_beacon(char *out, int cap) { return beacon(out, cap, true); }
static int ble_beacon(char *out, int cap)  { return beacon(out, cap, false); }

/* What the card is to the network (13, 14.3): a relay, carried by a person,
 * on its battery. Not archive: it cannot answer history asks (12.9.4). */
static void air_service(bool ble, bool lora)
{
    if (!nst_have_key()) return;
    char wire[XPRS_MAX_WIRE + 1], tf[32];
    nst_time_field(tf, sizeof tf);
    int n = snprintf(wire, sizeof wire, "t:service f:%s serve:relay site:portable source:battery fw:%s %s",
                     nst_call(), xfw_version(), tf);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = nst_sign(wire, n, (int)sizeof wire);
    if (lora && s_radio_up && !s_det.active) xb_send(&s_lora, wire, n);
    if (ble && s_ble_up) xb_send(&s_ble, wire, n);
}

static void air_identity(bool ble, bool lora)
{
    if (!nst_have_key()) return;
    char wire[XPRS_MAX_WIRE + 1], tf[32];
    nst_time_field(tf, sizeof tf);
    int n = snprintf(wire, sizeof wire, "t:identity f:%s %s k:%s", nst_call(), tf, nst_npub());
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = nst_sign(wire, n, (int)sizeof wire);
    if (lora && s_radio_up && !s_det.active) xb_send(&s_lora, wire, n);
    if (ble && s_ble_up) xb_send(&s_ble, wire, n);
}

/* ── BLE5 on the SoftDevice ─────────────────────────────────────────── */
static bool ble_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_ble_up || len > BLE_WIRE_MAX) return false;
    uint8_t ad[TN_ADV_DATA_MAX];
    int i = 0;
    ad[i++] = (uint8_t)(5 + len);
    ad[i++] = 0xFF;
    ad[i++] = BLE_COMPANY_LO; ad[i++] = BLE_COMPANY_HI;
    ad[i++] = BLE_MARKER;     ad[i++] = BLE_SUB_XPRS;
    memcpy(ad + i, wire, len); i += len;
    if (tn_adv_set_data(ad, (size_t)i) != TN_OK) return false;
    Serial.printf("ble: aired %dB %.*s\n", len, len > 60 ? 60 : len, wire);
    return true;
}

static const xb_ops_t k_ble_ops = {
    .air = ble_air, .now_ms = now_ms_fn, .random = random_fn,
    .lock = NULL, .unlock = NULL, .drain = NULL, .ctx = NULL, .name = "ble",
};

/* ── The scan: mostly asleep, wide awake when somebody is talking ──────
 *
 * The base scan listens 100 ms of every second (10%): an XPRS phone
 * advertises for 5 s of every minute (the app's Ble5.kt), so even the base
 * catches it within a minute or so. Hearing a phone boosts the scan to 90%
 * for 8 s, the rest of its window, and its minute repeats: the boost is
 * also scheduled a second before each next window, for as long as it stays
 * in reach. Stations do not boost it: they advertise all the time, the base
 * catches them, and a card among stations would otherwise never sleep.
 * Applied from the loop: the report callback only sets the time. */
#define SCAN_BASE_WINDOW   0x00A0     /* 100 ms of 1 s */
#define SCAN_BOOST_WINDOW  0x05A0     /* 900 ms of 1 s */
#define SCAN_BOOST_MS      8000UL
#define PHONE_PERIOD_MS    60000UL
#define PHONE_FRESH_MS     600000UL
static uint32_t s_boost_until, s_phone_ms, s_phone_next;
static bool     s_boosted;

static void scan_heard(const char *wire, int len)
{
    uint32_t now = millis();
    xprs_t x;
    char f[12] = "";
    /* The phone itself, not a station re-airing it (via:): only the phone's
     * own adverts keep its rhythm. */
    if (xprs_parse(wire, len, &x) && xprs_via_count(&x) == 0 &&
        xprs_get_str(&x, "f", f, sizeof f) && f[0] == 'X' && f[1] == '1') {
        /* A phone: its next window is a minute after this one began. Only
         * the first advert of a window sets the phase. */
        if (!s_phone_ms || now - s_phone_ms > 10000) {
            s_phone_next = now + PHONE_PERIOD_MS - 1000;
            s_boost_until = now + SCAN_BOOST_MS;
        }
        s_phone_ms = now;
    }
}

static void ble_report(const tn_adv_report_t *r, void *ctx)
{
    (void)ctx;
    const uint8_t *p = r->data, *end = r->data + r->data_len;
    while (p + 2 <= end) {
        uint8_t n = p[0];
        if (n == 0 || p + 1 + n > end) break;
        if (p[1] == 0xFF && n >= 5 && p[2] == BLE_COMPANY_LO && p[3] == BLE_COMPANY_HI &&
            p[4] == BLE_MARKER) {
            const char *wire = (const char *)p + 6;
            int len = n - 5;
            if (p[5] == BLE_SUB_XPRS && len > 0 && len <= XB_WIRE_MAX) {
                if (r->evt_type & 0x0001) {
                    s_peer_addr_type = r->addr_type;
                    memcpy(s_peer_addr, r->addr, 6);
                    s_peer_known = true;
                    xprs_t x;
                    if (xprs_parse(wire, len, &x)) xprs_get_str(&x, "f", s_peer_call, sizeof s_peer_call);
                }
                scan_heard(wire, len);
                xb_on_wire(&s_ble, wire, len, peer_of(wire, len), r->rssi);
            }
            return;
        }
        p += 1 + n;
    }
}

/* ── GATT and XBLOB: the owner's door and the signed OTA (P1-Pro's) ───── */
static uint32_t s_gatt_rx, s_gatt_tx;
static void blob_maybe_start(void);
static void gatt_connected(void *c, uint16_t conn, bool central)
{ (void)c; Serial.printf("gatt: link 0x%04x ready (%s), %d bytes per send\n",
                          conn, central ? "we dialled" : "dialled in", tn_gatt_mtu());
  if (central) blob_maybe_start(); }
static void gatt_disconnected(void *c, uint16_t conn, uint8_t reason)
{ (void)c; Serial.printf("gatt: link 0x%04x closed (0x%02x)\n", conn, reason); }
static void gatt_reply(const char *wire, int len)
{
    int rc = tn_gatt_send((const uint8_t *)wire, len);
    if (rc != 0) Serial.printf("gatt_reply: rc=%d len=%d\n", rc, len);
    s_gatt_tx++;
}

static int ble_scan_on(void);
static xblob_t s_blob;
static bool    s_blob_active;

static int blob_send(void *c, const uint8_t *f, int n)
{
    (void)c;
    if (!tn_gatt_connected()) return -1;
    int rc = tn_gatt_send((const uint8_t *)f, n);
    if (rc == 0) { s_gatt_tx++; return XBLOB_SEND_OK; }
    return XBLOB_SEND_BUSY;
}
static int blob_write(void *c, uint32_t off, const uint8_t *src, int len)
{ (void)c; return xfw_blob_write(off, src, len); }
static void blob_done(void *c, bool ok)
{
    (void)c;
    s_blob_active = false;
    ble_scan_on();
    if (ok) { Serial.println("xblob: complete -- installing"); xfw_blob_finish(s_blob.sig85); }
    else    Serial.println("xblob: gave up -- cmd:zfw fallback remains");
}
static const xblob_ops_t k_blob_ops = { NULL, blob_send, NULL, blob_write, blob_done };

static void blob_maybe_start(void)
{
    uint8_t sha[32]; uint32_t size;
    if (s_blob_active || !tn_gatt_connected()) return;
    if (!xfw_pending(sha, &size)) return;
    Serial.printf("xblob: requesting %lu-byte image\n", (unsigned long)size);
    tn_scan_stop();
    xfw_blob_reset();
    xblob_recv_start(&s_blob, &k_blob_ops, sha, size, millis());
    s_blob_active = true;
}

static void gatt_rx(void *c, const uint8_t *d, int n)
{
    (void)c; s_gatt_rx++;
    if (xblob_is_frame(d, n)) { xblob_rx(&s_blob, d, n, millis()); return; }
    xprs_t p; char t[12] = "", dst[16] = "";
    if (xprs_parse((const char *)d, n, &p) &&
        xprs_get_str(&p, "t", t, sizeof t) && strcmp(t, "command") == 0 &&
        xprs_get_str(&p, "d", dst, sizeof dst) && strcmp(dst, nst_call()) == 0) {
        nst_clock_learn(&p);
        xfw_gatt_rx((const char *)d, n, gatt_reply);
        blob_maybe_start();
        return;
    }
    Serial.printf("gatt rx %dB: %.*s\n", n, n > 60 ? 60 : n, (const char *)d);
}
static const tn_gatt_cb_t k_gatt_cb = { gatt_connected, gatt_disconnected, gatt_rx, NULL };

/* The scan: passive, extended, 1M, at the base or the boost duty. */
static int ble_scan_set(bool boost)
{
    tn_scan_cfg_t scan = { .own_addr_type = 0x01, .passive = 1, .itvl = 0x0640,
                           .window = (uint16_t)(boost ? SCAN_BOOST_WINDOW : SCAN_BASE_WINDOW),
                           .phy = TN_PHY_1M };
    if (s_ble_up) tn_scan_stop();
    s_boosted = boost;
    return tn_scan_start(&scan, ble_report, NULL);
}
static int ble_scan_on(void) { return ble_scan_set(false); }

static void scan_tick(void)
{
    if (!s_ble_up || s_blob_active) return;
    uint32_t now = millis();
    if (s_phone_ms && now - s_phone_ms < PHONE_FRESH_MS && (int32_t)(now - s_phone_next) >= 0) {
        s_boost_until = now + SCAN_BOOST_MS;
        s_phone_next += PHONE_PERIOD_MS;
    }
    bool want = (int32_t)(s_boost_until - now) > 0;
    if (want != s_boosted) ble_scan_set(want);
}

static int s_ble_err;
static void ble_begin(void)
{
    if (s_ble_up) return;
    s_ble_err = tn_start();
    if (s_ble_err != TN_OK) { Serial.printf("ble: SoftDevice would not start (%d)\n", s_ble_err); return; }
    uint8_t addr[6];
    uint32_t a = NRF_FICR->DEVICEADDR[0], b = NRF_FICR->DEVICEADDR[1];
    addr[0] = a; addr[1] = a >> 8; addr[2] = a >> 16; addr[3] = a >> 24;
    addr[4] = b; addr[5] = (b >> 8) | 0xC0;
    tn_set_random_addr(addr);
    /* One connectable set, a second apart, 0 dBm: a phone in the same
     * pocket or on the same table hears it, and it costs microamps. */
    tn_adv_cfg_t adv = {
        .handle = 0, .props = TN_ADV_PROP_CONNECTABLE, .itvl_min = 0x0640, .itvl_max = 0x0640,
        .chan_map = 0x07, .own_addr_type = 0x01, .tx_power = 0,
        .primary_phy = TN_PHY_1M, .secondary_phy = TN_PHY_1M, .sid = 0,
    };
    tn_adv_configure(&adv);
    s_ble_err = ble_scan_on();
    if (s_ble_err != TN_OK) return;
    s_ble_up = true;
    int sv = tn_gatt_serve(&k_gatt_cb);
    Serial.printf("ble: up -- extended adverts, scan 10%%, serve=%d\n", sv);
}

/* ── Console ─────────────────────────────────────────────────────────── */
static void read_line(char *line, int cap, uint32_t ms)
{
    int n = 0;
    for (uint32_t t0 = millis(); millis() - t0 < ms && n < cap - 1; ) {
        int ch = Serial.read();
        if (ch < 0) { delay(2); continue; }
        if (ch == '\r' || ch == '\n') break;
        line[n++] = (char)ch;
    }
    line[n] = 0;
}

static void health(void)
{
    LR11x0VersionInfo_t v;
    memset(&v, 0, sizeof v);
    int vst = s_radio_up ? s_radio.getVersionInfo(&v) : -1;
    Serial.printf("radio: up=%d version rc=%d hw=0x%02x device=0x%02x fw=%u.%u\n",
                  (int)s_radio_up, vst, v.hardware, v.device, v.fwMajor, v.fwMinor);
    Serial.printf("clock: LF source %s (LFCLKSRCCOPY=0x%08lx)\n",
                  (NRF_CLOCK->LFCLKSRCCOPY & 3) == 1 ? "XTAL" :
                  (NRF_CLOCK->LFCLKSRCCOPY & 3) == 0 ? "RC" : "synth",
                  (unsigned long)NRF_CLOCK->LFCLKSRCCOPY);
    Serial.printf("power: DCDCEN=%lu\n", (unsigned long)NRF_POWER->DCDCEN);
    Serial.printf("boot: bootloader at 0x%08lx, UICR NRFFW[1]=0x%08lx\n",
                  (unsigned long)NRF_UICR->NRFFW[0], (unsigned long)NRF_UICR->NRFFW[1]);
    uint32_t sp = NRF_UICR->NRFFW[1];
    if (sp >= 0x27000 && sp < 0x100000) {
        const volatile uint32_t *w = (const volatile uint32_t *)sp;
        Serial.printf("boot: settings page at 0x%08lx:", (unsigned long)sp);
        for (int i = 0; i < 12; i++) Serial.printf(" %08lx", (unsigned long)w[i]);
        Serial.println();
    }
    Serial.printf("fs: key %s, cfg %s\n", InternalFS.exists("/xprs/key") ? "present" : "MISSING",
                  InternalFS.exists("/xprs/cfg") ? "present" : "none");
}

static void console(int c)
{
    char line[128];
    switch (c) {
    case 'k':
        Serial.printf("call=%s npub=%s boots=%lu\n", nst_call(), nst_have_key() ? nst_npub() : "-",
                      (unsigned long)nst_boot_epoch());
        break;
    case 'K': {
        char nsec[80] = "-";
        if (nst_have_key()) bech32_encode("nsec", nst_priv(), 32, nsec, sizeof nsec);
        Serial.printf("nsec=%s\n", nsec);
        break; }
    case 'I': read_line(line, sizeof line, 5000); nst_key_import(line); break;
    case 'i': air_identity(true, true); break;
    case 'U': xfw_selftest(); break;
    case 'H': health(); break;
    case 'm': mail_report(); break;
    case 'T': { static bool on; on = !on; mail_trace(on); Serial.printf("mail trace %s\n", on ? "on" : "off"); break; }
    case 'X':
        /* A packet typed in, taken as heard on Bluetooth: for bench tests of
         * what the card does with traffic nobody is sending right now. */
        read_line(line, sizeof line, 10000);
        if (line[0]) on_ble(line, (int)strlen(line), 0, -50);
        break;
    case 'g': if (!gnss_is_on()) gnss_start(); break;
    case 'w': {
        const nmea_fix_t *f = gnss_fix();
        uint32_t gg, gb, gy;
        gnss_counts(&gg, &gb, &gy);
        Serial.printf("gnss: %lu bytes, %lu sentences, %lu bad\n", (unsigned long)gy,
                      (unsigned long)gg, (unsigned long)gb);
        Serial.printf("pos: %s acc %s age %lus%s%s | receiver %s, sats %u, valid %d, clock %lu\n",
                      s_pos[0] ? s_pos : "-", s_acc[0] ? s_acc : "-",
                      s_pos[0] ? (unsigned long)((millis() - s_pos_ms) / 1000) : 0UL,
                      pos_fresh() ? "" : " (stale)", s_still ? ", still" : "",
                      gnss_is_on() ? "on" : "off", f->sats, (int)f->valid, (unsigned long)nst_now());
        break; }
    case 'b': Serial.printf("scan: %s, phone %s\n", s_boosted ? "boosted" : "base",
                            s_phone_ms && millis() - s_phone_ms < PHONE_FRESH_MS ? "in reach" : "none"); break;
    case 'S': {
        float t = 0;
        bool ok = sens_temp_c(&t);
        int mv = sens_batt_mv();
        Serial.printf("sensors: temp %s%d.%dC (%s) batt %d mV %d%% ext=%d chg=%d\n", t < 0 ? "-" : "",
                      (int)fabsf(t), (int)(fabsf(t) * 10) % 10, ok ? "ok" : "out of range", mv,
                      sens_batt_pct(mv), (int)sens_ext_power(), (int)sens_charging());
        break; }
    case 'n':
        net_go(s_net == NET_MT ? NET_MC : NET_MT);
        s_pick.current = (uint8_t)s_net;
        s_pick.strikes = 0;
        mail_net_set(s_net);
        break;
    case 'd': if (!s_det.active) detect_start(false); break;
    case 'p': if (!s_det.active) detect_probe(); break;
    case 'r':
        s_rx_continuous = !s_rx_continuous;
        lora_listen();
        Serial.printf("lora: receive %s\n", s_rx_continuous ? "continuous" : "duty-cycled");
        break;
    case 'c':
        read_line(line, sizeof line, 10000);
        if (strncmp(line, "fg ", 3) == 0) nst_cfg_console(line + 3);
        else Serial.println("cfg set <key> <value> | cfg get <key> | cfg list");
        break;
    case 'D':
        Serial.println("rebooting into DFU");
        Serial.flush(); delay(50);
        sd_power_gpregret_set(0, 0x4E);
        sd_softdevice_disable();
        NVIC_SystemReset();
        break;
    case '?':
        Serial.printf("call=%s fw=%s%s clock=%lu key=%d lora=%d(%s) ble=%d(err %d) native=%lu aired=%lu detect=%s irqs=%lu bad=%lu rx=%s\n",
                      nst_call(), xfw_version(), xfw_probation() ? "(probation)" : "",
                      (unsigned long)nst_now(), (int)nst_have_key(),
                      (int)s_radio_up, s_nets[s_net].name, (int)s_ble_up, s_ble_err,
                      (unsigned long)s_native_frames, (unsigned long)s_rep[s_net].st.aired,
                      s_det.active ? "sweeping" : "idle", (unsigned long)s_irqs,
                      (unsigned long)s_rx_bad, s_rx_continuous ? "continuous" : "duty");
        break;
    default: break;
    }
}

/* ── Bring-up ───────────────────────────────────────────────────────── */
static bool lora_begin(void)
{
    s_spi.begin();
    s_radio.setRfSwitchTable(k_rf_pins, k_rf_table);
    const lora_net_t *n = &s_nets[s_net];
    int st = s_radio.begin(n->freq_mhz, n->bw_khz, n->sf, LORA_CR, n->sync,
                           LORA_POWER_DBM, n->preamble, T1_LR_TCXO_V);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: begin failed (%d) -- no radio this boot\n", st);
        return false;
    }
    s_radio.setRegulatorDCDC();
    s_radio.setPacketReceivedAction(lr_isr);
    s_radio_up = true;
    lora_listen();
    Serial.printf("lora: up on %s %.3f MHz SF%u BW%.1f, %d dBm\n",
                  n->name, n->freq_mhz, n->sf, n->bw_khz, LORA_POWER_DBM);
    return true;
}

static void bearers_flush(uint32_t ms)
{
    for (uint32_t t0 = millis(); millis() - t0 < ms; ) {
        xb_tick(&s_lora, millis());
        if (s_ble_up) { tn_gatt_pump(); xb_tick(&s_ble, millis()); }
        delay(5);
    }
}

#define STATION_STACK_WORDS 3072     /* 12 KB: signing on mbedtls */
static void station_setup(void);
static void station_loop(void);
static void station_task(void *arg)
{
    (void)arg;
    s_task = xTaskGetCurrentTaskHandle();
    station_setup();
    for (;;) station_loop();
}

void setup(void)
{
    xTaskCreate(station_task, "station", STATION_STACK_WORDS, NULL, TASK_PRIO_LOW, NULL);
}

void loop(void) { vTaskDelay(pdMS_TO_TICKS(1000)); }

static void station_setup(void)
{
    Serial.begin(115200);
    for (uint32_t t0 = millis(); !Serial && millis() - t0 < 2500; ) delay(10);
    delay(100);

    /* Flash before the SoftDevice (FIRMWARE.md section 7). */
    static const nst_cfg_t k_nst = { CALL_PREFIX, true };
    nst_init(&k_nst);
    nst_cfg_load();
    sens_begin();
    static const mail_cfg_t k_mail = { nst_call(), heard_directly };
    mail_begin(&k_mail);
    if (mail_net_get() == NET_MT || mail_net_get() == NET_MC) {
        s_net = mail_net_get();
        s_pick.current = (uint8_t)s_net;
    }
    nets_for_region(xcfg_get("region", "eu"));
    static const xfw_cfg_t k_xfw = {
        .board = FW_BOARD, .call = nst_call(), .sign = nst_sign, .flush = bearers_flush,
    };
    xfw_init(&k_xfw, nst_boot_epoch());
    ble_begin();

    NRF_WDT->CONFIG = WDT_CONFIG_SLEEP_Msk;
    NRF_WDT->CRV = 60 * 32768;
    NRF_WDT->RREN = 1;
    NRF_WDT->TASKS_START = 1;

    Serial.printf("\nXPRS station %s -- SenseCAP Card Tracker T1000-E (boot %lu)\n",
                  nst_call(), (unsigned long)nst_boot_epoch());

    /* From the hardware RNG, not the device ID: a seed that is the same at
     * every boot makes every boot's frames the same, and a repeater that
     * heard the last boot's probe ten minutes ago drops this one as a
     * duplicate (measured on the T1000-E, 2026-10-07). */
    uint32_t seed = 0;
    xprssig_platform_random((uint8_t *)&seed, sizeof seed);
    randomSeed(seed ^ NRF_FICR->DEVICEID[0]);
    s_self_node = mt_node_of_call(nst_call(), (int)strlen(nst_call()));

    s_radio_up = lora_begin();
    mesh_begin();

    xb_init(&s_lora, &k_lora_ops, nst_call());
    xb_set_rx_cb(&s_lora, on_lora);
    xb_set_beacon(&s_lora, lora_beacon, BEACON_EVERY_SEC * 3, BEACON_JITTER_SEC);
    xb_set_pace(&s_lora, LORA_PACE_MS);
    static xb_duty_t s_lora_duty;
    air_model_for(s_net);
    xb_set_duty(&s_lora, &s_lora_duty, lora_airtime_fn, NULL, 360000u, 6000u, 0u);
    xb_set_driver(s_radio_up);

    xb_init(&s_ble, &k_ble_ops, nst_call());
    xb_set_rx_cb(&s_ble, on_ble);
    xb_set_beacon(&s_ble, ble_beacon, BEACON_EVERY_SEC, BEACON_JITTER_SEC);
    xb_set_pace(&s_ble, 3000);

    for (int i = 0; i < 3 && s_radio_up; i++) { led_blip(60); delay(120); }
    if (s_radio_up) detect_start(true);
}

static void station_loop(void)
{
    if (s_rx_pending) {
        s_rx_pending = false;
        uint8_t buf[MT_FRAME_MAX + 1];
        size_t n = s_radio.getPacketLength();
        if (n > 0 && n <= MT_FRAME_MAX) {
            int st = s_radio.readData(buf, n);
            if (st == RADIOLIB_ERR_NONE)
                lora_rx_frame(buf, (int)n, (int)s_radio.getRSSI(), (int)s_radio.getSNR());
            else
                s_rx_bad++;
        }
        lora_listen();
    }

    /* While a sweep has the radio on another network, XPRS waits in the
     * queue (an air() refused would drop it) and the repeaters wait too. */
    detect_tick();
    if (!s_det.active) {
        xb_tick(&s_lora, millis());
        mesh_tick();
    }
    if (s_ble_up) { tn_gatt_pump(); xb_tick(&s_ble, millis()); scan_tick(); }
    pos_tick();
    mail_tick();
    if (s_blob_active) xblob_tick(&s_blob, millis());
    xfw_tick(millis(), s_radio_up);
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;

    int c = Serial.read();
    if (c > 0) console(c);

    /* Who we are: every half hour on Bluetooth, every three hours on LoRa
     * (a shared channel hears it from every card, and keys do not change);
     * never in the middle of a sweep, when the radio is on another network. */
    static uint32_t next_identity = 30000, next_lora_identity = 30000;
    static uint32_t next_service = 90000, next_lora_service = 90000;
    bool id_ble = (int32_t)(millis() - next_identity) >= 0;
    bool id_lora = (int32_t)(millis() - next_lora_identity) >= 0 && s_radio_up && !s_det.active;
    if (id_ble || id_lora) {
        air_identity(id_ble, id_lora);
        if (id_ble) next_identity = millis() + 1800000UL;
        if (id_lora) next_lora_identity = millis() + 3 * 3600000UL;
    }
    /* What we are: hourly on Bluetooth, every three hours on LoRa. */
    bool sv_ble = (int32_t)(millis() - next_service) >= 0;
    bool sv_lora = (int32_t)(millis() - next_lora_service) >= 0 && s_radio_up && !s_det.active;
    if (sv_ble || sv_lora) {
        air_service(sv_ble, sv_lora);
        if (sv_ble) next_service = millis() + 3600000UL;
        if (sv_lora) next_lora_service = millis() + 3 * 3600000UL;
    }

    static uint32_t next_alive;
    uint32_t now = millis();
    if ((int32_t)(now - next_alive) >= 0) {
        next_alive = now + 60000;
        uint32_t rx = 0, tx = 0, cancelled = 0, brx = 0, btx = 0, bcan = 0;
        xb_stats(&s_lora, &rx, &tx, &cancelled);
        xb_stats(&s_ble, &brx, &btx, &bcan);
        Serial.printf("alive %lus call=%s fw=%s lora[%s] rx=%lu tx=%lu native=%lu/%lu | ble rx=%lu tx=%lu | hears=%d\n",
                      (unsigned long)(now / 1000), nst_call(), xfw_version(), s_nets[s_net].name,
                      (unsigned long)rx, (unsigned long)tx, (unsigned long)s_native_frames,
                      (unsigned long)s_rep[s_net].st.aired,
                      (unsigned long)brx, (unsigned long)btx, hears_count());
    }

    /* Sleep until something happens: tickless idle puts the nRF52 down
     * between here and the next event or timeout. */
    bool busy = s_det.active || s_blob_active || gnss_is_on() || mail_busy() || s_rx_pending || mesh_busy(now);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? LOOP_BUSY_MS : LOOP_IDLE_MS));
}
