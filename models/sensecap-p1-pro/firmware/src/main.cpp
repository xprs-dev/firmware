/*
 * An XPRS station on a SenseCAP Solar Node P1-Pro.
 *
 * Headless, outdoors, one bearer: LoRa. It listens on 868 MHz, keeps what it
 * hears out of its own duplicate rings, digipeats within the hop budget with
 * itself appended to `via:` (XPRS 13.1), and beacons who it is.
 *
 * WHAT IS SHARED, AND WHAT THAT COST. The wire format and the relay decision
 * are common/xprs_codec and common/xprs_bearer -- the same two files the
 * T-Deck, the T-Dongle and the M5Stack run, symlinked into lib/ and compiled
 * by PlatformIO instead of by the IDF. They needed two changes to get here,
 * both small and both in the tree: xprs_codec gained xprs_sha256_sw.c so its
 * one hash has an implementation without mbedtls, and xprs_bearer's two log
 * lines learned that a target which is neither the IDF nor the host harness
 * should fall silent rather than fail to find esp_log.h. Nothing else was
 * touched, and the ESP32 boards build unchanged.
 *
 * WHAT IS NOT HERE YET, and why, in the order it should be added:
 *
 *   BLE5.  The chip can: nRF52840 with SoftDevice S140 v7.3.0 is a Bluetooth
 *          5 controller with extended advertising, and an XPRS beacon at
 *          112-173 bytes fits an extended AD with room to spare. What is
 *          missing is a path to it -- common/xprs_bearer_ble talks to the
 *          ESP32 controller over HCI through common/tinynimble, which does
 *          not exist here, and the Bluefruit API this core ships is built
 *          around the 31-byte legacy advert. Whether Bluefruit will drive an
 *          extended set, or whether this wants raw sd_ble_gap_adv_set_configure
 *          underneath it, is an open question and not one to guess at.
 *
 *   ~~Signing~~  Done. common/xprs_sig's device branch is mbedtls, and a
 *          cut-down mbedtls (bignum + ecp, secp256k1 only) now lives in
 *          lib/mbedtls_ecp, so the same xprssig.c runs here. The key is
 *          made once and kept in the internal LittleFS; the callsign is
 *          X3 + the first four characters of the key's npub, exactly as
 *          nostr_keys.c derives it on the ESP32s (XPRS 3), so a receiver
 *          can re-derive it. Beacons and t:identity go out signed.
 *
 *   GNSS.  Wired, powered through a load switch, and left OFF. It is a
 *          receiver, not a bearer, and on a solar node an unused one that is
 *          merely asleep rather than switched off is a real current draw.
 *
 * VALIDATED ON THE HARDWARE, against a T-Deck on the same bench, 2026-08-30.
 * Both directions, byte-exact, and the third line is the one that matters:
 *
 *   P1-Pro -> T-Deck   6 of 7 beacons, -30 dBm SNR 12
 *                      "xprslora: RX 40 bytes ... t:observation f:X54W6W"
 *   T-Deck -> P1-Pro   29 wires in 180 s at -31..-33 dBm, all parsed
 *   and onward         "xprs: lan 192.168.178.102 58B t:observation
 *                       f:X54W6W link:lora peers:0 via:X3GSLC,X3WWAJ"
 *
 * That last line is a packet this chip composed, carried on LoRa, picked up
 * by an ESP32, digipeated with two callsigns appended to via:, and put on the
 * LAN. The shared codec and the shared bearer produced a wire the rest of the
 * fleet treats as one of its own, which is the whole claim being tested.
 *
 * AND THE DIGIPEATER ITSELF, same bench, later the same day. The T-Deck put
 * "t:message f:X3GSLC m:digipeat test 1" on LoRa alone (POST /api/xprs/send,
 * bearer=lora), and this station said it again on both media, us in via:
 *
 *   lora   rx  -47 dBm  59B t:message f:X3GSLC ts:... m:digipeat test 1
 *   ble:   aired 70B       t:message f:X3GSLC ts:... via:X54W6W m:digip
 *   lora:  aired 70B       t:message f:X3GSLC ts:... via:X54W6W m:digip
 *
 * and the T-Deck heard both copies back: "xprslora: RX 70 bytes at -45 dBm
 * ... via:X54W6W" and "xprs: ble -73 dBm 70B ... via:X54W6W". f: untouched,
 * one callsign appended, the identifier (41bc2d) unchanged, as section 13
 * requires. (X54W6W was this board's callsign before it had a key; it is
 * X33ESX now, and the same test repeated under it reads via:X33ESX.)
 *
 * AND THE SIGNATURE. The t:identity this chip aired -- "t:identity f:X33ESX
 * epoch:1.67 k:npub13esx... sig:..." as the T-Deck logged it -- verifies on
 * the host through the OpenSSL branch of the same xprssig.c, and fails with
 * one character of the epoch changed. The callsign is X3 + "3esx", the
 * first four characters of that npub after "npub1", as section 3 asks.
 */

#include <Arduino.h>
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
#include "nrf_sdm.h"
#include "nrf_soc.h"
#include "mt.h"
#include "mt_mesh.h"
#include "lr_worth.h"
}
#include "update.h"
#include "nrf_station.h"
#include "nrf_flash.h"
using namespace Adafruit_LittleFS_Namespace;

/* ── What this radio is set to ───────────────────────────────────────────
 *
 * The module covers 862-930 MHz and the supplied antenna is specified
 * 868-915 MHz, so all three of the bands anyone asks about are in range.
 * 868.0 is the EU default and what the T-Deck already uses, so two boards on
 * one bench meet without either being re-flashed.
 *
 * 869.5 MHz is ERC 70-03 band g3 (869.40-869.65): 10% duty and up to
 * 27 dBm e.r.p., which makes this module's 22 dBm legal where it was over
 * the old 868.0 channel's g1 limit. The duty ledger below is what holds
 * the 10%; the operator still owns antenna gain and the final say.
 *
 * EVERY ONE OF THESE MUST MATCH common/xprs_bearer_lora/xprslora.c, or this
 * station is on the air and alone. Two LoRa radios on the same frequency
 * with different spreading factors are as deaf to each other as two radios
 * on different bands, and nothing reports it -- the symptom is a peer count
 * that stays at zero, which looks exactly like being out of range.
 *
 * The fleet's figures, from xprslora.c's sx1262_lora_config_t:
 *
 *     SF7, BW 125 kHz, CR 4/5, preamble 8, CRC on
 *
 * RadioLib spells the coding rate as its denominator, so 4/5 is 5.
 *
 * The sync word used to be the one thing neither side set. It is set on both
 * sides now: xprs_sx1262 writes it from the one-byte shorthand every time it
 * tunes or retunes, and `xprs` mode's shorthand is 0x12, which is the
 * register value 0x1424 the chip also happens to come up with -- the
 * private-network value, not LoRaWAN's public 0x3444. RadioLib writes the
 * same register from the same shorthand. Same bytes on the air, and now for
 * a stated reason rather than by both sides leaving it alone.
 */
#define LORA_FREQ_MHZ    869.5
#define LORA_BW_KHZ      125.0
#define LORA_SF          7
#define LORA_CR          5         /* 4/5, as the fleet uses */
#define LORA_SYNC_WORD   0x12      /* = 0x1424, the SX1262 private default */
#define LORA_POWER_DBM   22
#define LORA_PREAMBLE    8

/* ── Which LoRa network: Meshtastic's, unless told otherwise ────────────
 *
 * The figures above are the `xprs` mode's, and they are what this board ran
 * alone until 2026-11-25. By then every ESP32 station defaulted to
 * Meshtastic's LongFast (xprslora.h, XPRSLORA_MODE_DEFAULT), so a P1 left on
 * its own channel heard none of them, and a Meshtastic user near the pole
 * did not know it was there. The default is now `meshtastic`, the same as
 * the rest of the fleet:
 *
 *   LongFast: SF11, 250 kHz, CR 4/5, preamble 16, sync 0x2B, the frequency
 *   from Meshtastic's own slot rule (mt_slot_freq_hz: 869.525 MHz in EU)
 *
 * and on it the station is a Meshtastic node in its own right: XPRS rides
 * in Meshtastic frames on a private portnum (mt_xprs_wrap, so stock routers
 * flood it by header), and common/xprs_meshtastic's mt_mesh repeats the
 * mesh's own traffic, announces this station with a NodeInfo, and bridges
 * text both ways -- the code the ESP32 stations run, and the bridge the
 * T1000-E left to "a station on mains" (its main.cpp says why).
 *
 * `cfg set lora_mode xprs` puts the old channel back (a cfg set reboots);
 * `lora_region` picks Meshtastic's region (default eu). */
typedef struct {
    const char *name;
    float       freq_mhz;
    float       bw_khz;
    uint8_t     sf;
    uint8_t     sync;
    uint16_t    preamble;
} lora_net_t;
static lora_net_t s_net = { "xprs", LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF,
                            LORA_SYNC_WORD, LORA_PREAMBLE };
static bool        s_mt;                 /* on Meshtastic's channel */
static uint32_t    s_self_node;          /* our Meshtastic node number */
static mt_reasm_t  s_mt_reasm;
static mt_mesh_t   s_mesh;               /* the repeater and the bridge */
static bool        s_mesh_on;
static uint32_t    s_native_frames;      /* Meshtastic's own, not XPRS */
static xb_lora_air_t s_air;              /* the modem, for the ledger */

/* XPRS 31.1: what one packet owes this bearer in silence. A shared airwave
 * with no pacing is a bearer that talks over everybody, and this one is meant
 * to sit on a pole for a year. */
#define LORA_PACE_MS     2000

/* Five minutes on a pole, and overridable because five minutes is a long
 * time to sit at a bench waiting to find out whether the other station can
 * hear this one. -DBEACON_EVERY_SEC=15 turns a validation run into a minute
 * instead of half an hour; the default is what ships. */
#ifndef BEACON_EVERY_SEC
#define BEACON_EVERY_SEC 300
#endif
#ifndef BEACON_JITTER_SEC
#define BEACON_JITTER_SEC 30
#endif

/* ── BLE5, on the SoftDevice through tinynimble ──────────────────────────
 *
 * The same framing as every ESP32 station (common/xprs_bearer_ble/xprsble.h):
 * manufacturer data, company 0xFFFF, marker 0x3E, subtype 0x58, then the
 * XPRS wire verbatim. One advertising set on this chip, so the beacon is
 * what the set carries and a re-air replaces it for a while. */
#define BLE_COMPANY_LO 0xFF
#define BLE_COMPANY_HI 0xFF
#define BLE_MARKER     0x3E
#define BLE_SUB_XPRS   0x58
#define BLE_WIRE_MAX   (TN_ADV_DATA_MAX - 6)

static xb_t     s_ble;
static bool     s_ble_up;
/* The last connectable XPRS station heard, for 'd' (dial). */
static uint8_t  s_peer_addr[6], s_peer_addr_type;
static bool     s_peer_known;
static char     s_peer_call[16];

static SPIClass       &s_spi = SPI;
static SX1262          s_radio = new Module(P1_LORA_CS, P1_LORA_DIO1,
                                            P1_LORA_RST, P1_LORA_BUSY, s_spi);
static xb_t            s_lora;
static volatile bool   s_rx_pending;
static bool            s_radio_up;
static uint32_t        s_heard;

/* ── The key, the callsign, the config and the clock ────────────────────
 *
 * common/xprs_nrf52 (nrf_station.h), shared with the T1000-E: an X3
 * callsign from the key's npub (XPRS 3), the boot ordinal, /xprs/cfg behind
 * xcfg_get(), and the clock learned from signed owner commands. What used to
 * be here moved there unchanged; the history of each decision is in its
 * comments. */

/* ── Who we hear directly (10.6.3) ──────────────────────────────────────
 *
 * The hears: list on the beacon names stations heard with no via: -- a
 * neighbour a packet can reach in one hop. The ESP32 boards keep this in
 * xprs_station, which leans on FreeRTOS and is not ported; this is the
 * eight-row version of the same fact. A peer id for the bearer's own table
 * is derived from the callsign too, so peers: stops reading zero. */
#define HEARS_MAX     8
#define HEARS_FRESH_MS 600000UL
static struct { char call[10]; uint32_t t_ms; } s_hears[HEARS_MAX];

static uint64_t peer_of(const char *wire, int len, bool *direct)
{
    xprs_t p; char from[10] = "";
    if (direct) *direct = false;
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "f", from, sizeof from) || !from[0]) return 0;
    if (direct) *direct = xprs_via_count(&p) == 0;
    uint64_t h = 1469598103934665603ULL;               /* FNV-1a, never zero */
    for (const char *c = from; *c; c++) { h ^= (uint8_t)*c; h *= 1099511628211ULL; }
    return h ? h : 1;
}

static void hears_touch(const char *wire, int len)
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
    snprintf(s_hears[slot].call, sizeof s_hears[slot].call, "%s", from);
    s_hears[slot].t_ms = now;
}

static int hears_render(char *out, int cap)
{
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < HEARS_MAX; i++) {
        if (!s_hears[i].call[0] || now - s_hears[i].t_ms > HEARS_FRESH_MS) continue;
        int w = snprintf(out + n, (size_t)(cap - n), "%s%s", n ? "," : " hears:", s_hears[i].call);
        if (w <= 0 || n + w >= cap) { out[n] = 0; break; }
        n += w;
    }
    return n;
}

/* ── The LEDs ────────────────────────────────────────────────────────────
 *
 * Two, and both are pulled up on this carrier: LOW lights them. They are the
 * only thing this station can say to somebody standing under the pole, so
 * they are spent on the two facts worth having from the ground -- the radio
 * came up, and something is being heard. */
static inline void led(int pin, bool on) { digitalWrite(pin, on ? LOW : HIGH); }

static void led_blip(int pin, uint16_t ms)
{
    led(pin, true);
    delay(ms);
    led(pin, false);
}

/* ── The bearer's driver ─────────────────────────────────────────────────
 *
 * Five functions, and xprs_bearer does the rest: the queue, the two
 * duplicate rings, the §13.2.1 random wait, the cancel when somebody else
 * gets there first, and appending us to `via:`. That is the entire reason
 * for compiling somebody else's component onto this chip. */

static void dio1_isr(void) { s_rx_pending = true; }

/* Listen before talking (CAD). The CAD-done interrupt is on DIO1 too, so
 * the flag it raised is not a packet; and CAD leaves the radio in standby,
 * so whoever asked puts it back to receive or transmits. */
static bool channel_free(void)
{
    int r = s_radio.scanChannel();
    s_rx_pending = false;
    return r == RADIOLIB_CHANNEL_FREE;
}

/* One raw frame on the air, then back to receive (see the note in
 * lora_air on why the flag is cleared after). */
static bool radio_tx(const uint8_t *f, int len)
{
    int st = s_radio.transmit(f, (size_t)len);
    s_radio.startReceive();
    s_rx_pending = false;
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: transmit failed (%d)\n", st);
        return false;
    }
    led_blip(P1_LED_MESH, 20);
    return true;
}

/* An XPRS wire on Meshtastic's channel: one or two frames (mt_xprs_wrap),
 * each after a clear channel. The xb pump has already charged the ledger
 * for both and dropped the packet from its queue, so "not now" is not an
 * answer it can take: up to eight looks a slot or so apart, then go anyway,
 * as xprslora.c does. */
static bool lora_air_mt(const char *wire, int len)
{
    static uint8_t frames[2][MT_FRAME_MAX];
    int fl[2];
    int n = mt_xprs_wrap(wire, len, s_self_node, frames, fl);
    if (!n) {
        Serial.printf("lora: not wrappable: %.40s\n", wire);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        for (int t = 0; t < 8 && !channel_free(); t++)
            delay(mt_mesh_slot_ms() + (uint32_t)random(0, 200));
        ok = radio_tx(frames[i], fl[i]);
    }
    if (ok) Serial.printf("lora: aired %dB%s %.*s\n", len, n > 1 ? " (2 frames)" : "",
                          len > 60 ? 60 : len, wire);
    return ok;
}

static bool lora_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_radio_up) return false;
    if (s_mt) return lora_air_mt(wire, len);

    int st = s_radio.transmit((const uint8_t *)wire, (size_t)len);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: transmit failed (%d)\n", st);
        s_radio.startReceive();
        return false;
    }
    Serial.printf("lora: aired %dB %.*s\n", len, len > 60 ? 60 : len, wire);
    led_blip(P1_LED_MESH, 20);
    /* transmit() leaves the radio in standby; nothing is heard until this. */
    s_radio.startReceive();
    /*
     * AND THE FLAG IS CLEARED AFTER, NOT BEFORE. DIO1 is wired to TxDone as
     * well as RxDone, so the blocking transmit() above raises the same line
     * the receive path watches and dio1_isr sets s_rx_pending on its way out.
     * loop() then believed a packet had arrived, asked the radio how long it
     * was, and read a length belonging to one frame out of a FIFO holding
     * another.
     *
     * It was visible on the bench exactly once and looked like nothing:
     *
     *   lora rx -33 dBm 141B t:observation f:X54W6W link:lora peers:0hears:...
     *
     * -- this station's own 40-byte beacon with the tail of somebody's
     * 141-byte observation welded on at the missing space. A corrupt wire is
     * not a crash: it parses or it does not, and either way the next line of
     * the log looks fine, which is why this is worth a paragraph.
     */
    s_rx_pending = false;
    return true;
}

static uint32_t lora_now_ms(void) { return millis(); }
static uint32_t lora_random(void) { return (uint32_t)random(0x7FFFFFFF); }

static const xb_ops_t k_lora_ops = {
    .air     = lora_air,
    .now_ms  = lora_now_ms,
    .random  = lora_random,
    .lock    = NULL,      /* one task; loop() is the only caller */
    .unlock  = NULL,
    .drain   = NULL,      /* delivered inline from loop(), not a foreign task */
    .ctx     = NULL,
    .name    = "lora",
};

/* ── What we hear ────────────────────────────────────────────────────────
 *
 * xb_on_wire() has already decided this is new before we get here: the
 * duplicate rings, the §13 loop check and the hop budget are its business,
 * and it will have queued a re-air if one is owed. What is left is to say so
 * on the console and to count it. */
static void heard(const char *link, const char *wire, int len, int rssi)
{
    s_heard++;
    xprs_t p;
    bool parsed = xprs_parse(wire, len, &p);
    Serial.printf("%-6s rx %4d dBm %3dB %s%.*s\n", link, rssi, len,
                  parsed ? "" : "(unparsed) ", len > 90 ? 90 : len, wire);
    led_blip(P1_LED_USER, 20);
}

/* THE BRIDGE. What one bearer hears is offered to the other, and xprs_bearer
 * decides -- duplicate rings, hop budget, the random wait -- whether it goes
 * out there. Same rule as the T-Dongle between Bluetooth and the LAN. */
/* A t:command addressed to us: the updater's, and never carried further
 * (25.4: a command is acted on where it arrives; a thousand image chunks
 * digipeated three hops would be the channel gone for an afternoon). */
static bool command_for_us(xb_t *b, const char *wire, int len)
{
    xprs_t p; char t[12] = "", d[16] = "";
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "t", t, sizeof t) || strcmp(t, "command") != 0) return false;
    if (!xprs_get_str(&p, "d", d, sizeof d) || strcmp(d, nst_call()) != 0) return false;
    nst_clock_learn(&p);
    xfw_handle(b, &p);
    return true;
}

static void on_lora(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    heard("lora", wire, len, rssi);
    hears_touch(wire, len);
    if (command_for_us(&s_lora, wire, len)) return;
    /* THE DIGIPEATER. This station's whole reason to be on a pole: what it
     * hears on LoRa it says again on LoRa, so a small device at the edge of
     * its own range is heard beyond it (XPRS 13, "repeats a packet on the
     * medium it heard it"). xb_digipeat, not xb_offer: the same-medium
     * door, which treats hearing it here as the reason to repeat rather
     * than a reason not to. The hop budget, the own-callsign loop check,
     * the random wait and the cancel when another relay speaks first are
     * all the bearer's, as is appending us to via:. The ESP32 LoRa bearer
     * does exactly this (common/xprs_bearer_lora/xprslora.c); this line was
     * missing here, and the station bridged to BLE while repeating nothing
     * on the air. */
    /* On Meshtastic's channel only what somebody is waiting for (lr_worth:
     * messages, receipts, commands, identities...), the rule every ESP32
     * station applies there; at SF11 a frame is the best part of a second
     * of a channel the whole mesh shares. */
    if (lr_worth(wire, len, !s_mt)) xb_digipeat(&s_lora, wire, len);
    if (s_ble_up) xb_offer(&s_ble, wire, len);
    /* And to Meshtastic: the bridge decides what it can carry (a message,
     * not sealed, not local) and never takes back what came from it. */
    if (s_mesh_on) mt_mesh_on_xprs(&s_mesh, wire, len, MT_XPRS_HEARD);
}

static void on_ble(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    heard("ble", wire, len, rssi);
    hears_touch(wire, len);
    if (command_for_us(&s_ble, wire, len)) return;
    /* Bluetooth is busy and cheap, LoRa is neither: presence (observation,
     * service, status) stays on Bluetooth when we are on Meshtastic's
     * channel. Measured on the bench the day this changed: with no filter
     * the station spent its air re-airing every neighbour's history query. */
    if (lr_worth(wire, len, !s_mt)) xb_offer(&s_lora, wire, len);
    if (s_mesh_on) mt_mesh_on_xprs(&s_mesh, wire, len, MT_XPRS_HEARD);
}

/* ── Meshtastic: the repeater and the bridge (common/xprs_meshtastic) ───
 *
 * mt_mesh is platform-free; what is here is its five hooks, the same ones
 * xprslora.c and xprs_app.c give it on an ESP32, on this chip's terms. */

/* A Meshtastic frame from the bridge (a relay, a NodeInfo, a translated
 * text, an ack): the channel first, then the shared ledger, and "not now"
 * is an answer -- the bridge keeps the frame and asks again. */
static bool mt_air(void *ctx, const uint8_t *f, int len, int prio)
{
    (void)ctx;
    if (!s_radio_up || len <= 0 || len > MT_FRAME_MAX) return false;
    if (!channel_free()) { s_radio.startReceive(); s_rx_pending = false; return false; }
    if (!xb_spend(&s_lora, xb_lora_airtime_ms(&s_air, len), prio >= MT_PRIO_URGENT)) {
        s_radio.startReceive();
        s_rx_pending = false;
        return false;
    }
    bool ok = radio_tx(f, len);
    mt_hdr_t h;
    if (ok && mt_hdr_parse(f, len, &h))
        Serial.printf("mt tx %08lx>%08lx id %08lx hop %d/%d %dB\n", (unsigned long)h.from,
                      (unsigned long)h.to, (unsigned long)h.id, h.hop_limit, h.hop_start, len);
    return ok;
}

/* The bridge hands translations over from inside its receive and tick
 * paths; they go out from the loop, after it returns, the way xprs_app.c
 * parks them for idx_task. A receipt is signed as this station first. */
#define MESHQ_N 4
static struct { bool full, sign; int len; char wire[XPRS_MAX_WIRE + 1]; } s_meshq[MESHQ_N];

static void mt_deliver(void *ctx, const char *wire, int len, bool sign,
                       int rssi, int hops, const char *relay)
{
    (void)ctx; (void)rssi; (void)hops; (void)relay;
    if (len <= 0 || len > XPRS_MAX_WIRE) return;
    for (int i = 0; i < MESHQ_N; i++) {
        if (s_meshq[i].full) continue;
        memcpy(s_meshq[i].wire, wire, (size_t)len);
        s_meshq[i].wire[len] = 0;
        s_meshq[i].len = len;
        s_meshq[i].sign = sign;
        s_meshq[i].full = true;
        return;
    }
    Serial.printf("mesh: %s dropped, %d waiting\n", sign ? "receipt" : "translation", MESHQ_N);
}

/* A translation leaves the way our own packets leave (xprs_app.c's
 * mesh_out, and the bug that taught it): xb_send, not xb_offer -- its via:
 * already names us, and an offer would take that for a loop and drop it --
 * and never onto LoRa as XPRS, because every LoRa station on this channel
 * heard the Meshtastic frame and translates for itself. A gateway receipt is
 * ours, signed, and goes everywhere. */
static void mesh_drain(void)
{
    for (int i = 0; i < MESHQ_N; i++) {
        if (!s_meshq[i].full) continue;
        static char w[XPRS_MAX_WIRE + 1];
        int n = s_meshq[i].len;
        bool sign = s_meshq[i].sign;
        memcpy(w, s_meshq[i].wire, (size_t)n + 1);
        s_meshq[i].full = false;
        if (!sign) {
            s_heard++;
            Serial.printf("mesh   %3dB %.*s\n", n, n > 100 ? 100 : n, w);
            led_blip(P1_LED_USER, 20);
            if (s_ble_up) xb_send(&s_ble, w, n);
            continue;
        }
        if (n + 5 + XPRSSIG_B85_LEN <= XPRS_MAX_WIRE) n = nst_sign(w, n, (int)sizeof w);
        if (n <= 0 || n > XPRS_MAX_WIRE) continue;
        Serial.printf("mesh: gateway receipt %.60s\n", w);
        if (s_ble_up) xb_send(&s_ble, w, n);
        xb_send(&s_lora, w, n);
    }
}

/* To the minute, so two bridges hearing the same frame compose the same
 * packet (docs/lora.md); epoch: when there is no clock. */
static int mt_stamp(void *ctx, char *out, int cap, bool to_minute)
{
    (void)ctx;
    int n = nst_time_field(out, cap);
    if (to_minute && n == 22 && strncmp(out, "ts:", 3) == 0) { out[20] = '0'; out[21] = '0'; }
    return n;
}

/* The clock learned from owner commands. Without one the bridge carries
 * Meshtastic into XPRS but mirrors nothing heard the other way: it cannot
 * tell old news from new (mt_mesh.h, MT_MIRROR_SETTLE_MS). */
static uint32_t mt_utc(void *ctx)
{
    (void)ctx;
    uint32_t t = nst_now();
    return t > 1700000000u ? t : 0;
}

static void mt_log(void *ctx, const char *line) { (void)ctx; Serial.printf("mt: %s\n", line); }

/* What the bridge keeps across a restart: the X25519 keys it learned from
 * NodeInfos (a node answers a NodeInfo request once in 12 hours, so a bridge
 * that forgot would be unable to DM it for half a day) and the callsigns it
 * announced. One flash page, rewritten whole; the bridge saves at most once
 * a minute and only when something is new, from its tick on this task,
 * which is where nrf_flash may be called (nrf_flash.h).
 *
 * The page is the last of the 24 KB gap between the update's BACKUP slot
 * (ends 0xE7000) and the internal LittleFS (0xED000), update.cpp's flash
 * map. A cable flash writes the application region only, so it survives. */
#define MTP_ADDR   0xEC000u
#define MTP_MAGIC  0x3154584Du                      /* "MXT1" */
#define MTP_KEYS   (MT_KEYS * (int)sizeof(mt_keyrec_t))
#define MTP_VNODES (MT_VNODES * MT_CALL_LEN)
typedef struct {
    uint32_t magic;
    uint16_t keys_len, vnodes_len;
    uint8_t  keys[MTP_KEYS];
    uint8_t  vnodes[MTP_VNODES];
} mtp_page_t;
static_assert(sizeof(mtp_page_t) <= NRF_FLASH_PAGE, "the bridge's page");
static_assert(sizeof(mtp_page_t) % 4 == 0, "whole words");
static mtp_page_t s_mtp;                            /* what is on flash */

static void mtp_read(void)
{
    memcpy(&s_mtp, (const void *)MTP_ADDR, sizeof s_mtp);
    if (s_mtp.magic != MTP_MAGIC || s_mtp.keys_len > MTP_KEYS || s_mtp.vnodes_len > MTP_VNODES) {
        memset(&s_mtp, 0, sizeof s_mtp);
        s_mtp.magic = MTP_MAGIC;
    }
}

static void mtp_write(void)
{
    if (!nrf_flash_erase(MTP_ADDR) ||
        !nrf_flash_write(MTP_ADDR, (const uint32_t *)&s_mtp, sizeof s_mtp / 4))
        Serial.println("mt: could not save the bridge's keys");
}

static int mt_keys_load(void *ctx, void *buf, int cap)
{
    (void)ctx;
    int n = s_mtp.keys_len < cap ? s_mtp.keys_len : cap;
    memcpy(buf, s_mtp.keys, (size_t)n);
    return n;
}

static void mt_keys_save(void *ctx, const void *buf, int len)
{
    (void)ctx;
    if (len < 0 || len > MTP_KEYS) return;
    memcpy(s_mtp.keys, buf, (size_t)len);
    s_mtp.keys_len = (uint16_t)len;
    mtp_write();
}

static int mt_vnodes_load(void *ctx, void *buf, int cap)
{
    (void)ctx;
    int n = s_mtp.vnodes_len < cap ? s_mtp.vnodes_len : cap;
    memcpy(buf, s_mtp.vnodes, (size_t)n);
    return n;
}

static void mt_vnodes_save(void *ctx, const void *buf, int len)
{
    (void)ctx;
    if (len < 0 || len > MTP_VNODES) return;
    memcpy(s_mtp.vnodes, buf, (size_t)len);
    s_mtp.vnodes_len = (uint16_t)len;
    mtp_write();
}

static uint32_t mt_now_ms(void) { return millis(); }
static uint32_t mt_random(void) { return (uint32_t)random(0x7FFFFFFF); }

/* The repeater and the bridge, from the same config keys an ESP32 station
 * reads (xprs_app.c, lora_start_bridge). */
static bool cfg_bool(const char *key, bool def)
{
    const char *v = xcfg_get(key, NULL);
    if (!v || !v[0]) return def;
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0 || strcmp(v, "yes") == 0;
}

static void mesh_begin(void)
{
    mt_mesh_cfg_t cfg = {
        .repeat = cfg_bool("mt_repeat", true),
        .bridge = cfg_bool("mt_bridge", true),
        .bcast_per_hour = 12,
        .nodeinfo_min = 180,
    };
    const char *v;
    if ((v = xcfg_get("mt_bcast_hr", NULL)) && v[0]) cfg.bcast_per_hour = (uint16_t)strtoul(v, NULL, 10);
    if ((v = xcfg_get("mt_ni_min", NULL)) && v[0])   cfg.nodeinfo_min = (uint16_t)strtoul(v, NULL, 10);
    if (!cfg.repeat && !cfg.bridge) return;
    mtp_read();
    static const mt_mesh_ops_t ops = {
        .air = mt_air, .now_ms = mt_now_ms, .random = mt_random,
        .deliver = mt_deliver, .heard = NULL, .stamp = mt_stamp,
        .utc_now = mt_utc, .tz_offset = NULL, .nick_of = NULL, .log = mt_log,
        .keys_load = mt_keys_load, .keys_save = mt_keys_save,
        .vnodes_load = mt_vnodes_load, .vnodes_save = mt_vnodes_save,
        .ctx = NULL,
    };
    mt_mesh_init(&s_mesh, &ops, &cfg, nst_call(), xcfg_get("name", ""));
    s_mesh_on = true;
    Serial.printf("meshtastic: node !%08lx, repeater %s, bridge %s, %u broadcasts an hour, "
                  "%u keys kept, %u bytes\n",
                  (unsigned long)s_self_node, cfg.repeat ? "on" : "off", cfg.bridge ? "on" : "off",
                  (unsigned)cfg.bcast_per_hour, (unsigned)(s_mtp.keys_len / sizeof(mt_keyrec_t)),
                  (unsigned)sizeof s_mesh);
}

/* A frame off the air on Meshtastic's channel: XPRS inside it goes to the
 * bearer as any wire would; anything else is the mesh's own, for the
 * repeater and the bridge. */
static void lora_rx_mt(const uint8_t *f, int len, int rssi, int snr)
{
    static char wire[XPRS_MAX_WIRE + 8];
    int n = mt_xprs_unwrap(&s_mt_reasm, f, len, millis(), wire, sizeof wire);
    if (n > 0) {
        wire[n] = 0;
        if (s_mesh_on) mt_mesh_note_xprs_frame(&s_mesh, f, len);
        xb_on_wire(&s_lora, wire, n, peer_of(wire, n, NULL), rssi);
        return;
    }
    if (n == 0) return;                   /* half a wire, waiting for the rest */
    s_native_frames++;
    mt_hdr_t h;
    if (mt_hdr_parse(f, len, &h))
        Serial.printf("mt %08lx>%08lx id %08lx hop %d/%d ch %02x %dB %d dBm SNR %d\n",
                      (unsigned long)h.from, (unsigned long)h.to, (unsigned long)h.id,
                      h.hop_limit, h.hop_start, h.channel, len, rssi, snr);
    if (s_mesh_on) mt_mesh_on_frame(&s_mesh, f, len, rssi, snr);
}

/* 'n' on the console: the Meshtastic nodes heard. */
static void mesh_nodes(void)
{
    if (!s_mesh_on) { Serial.println("meshtastic: not on this channel"); return; }
    const mt_node_t *nd = NULL;
    int count = mt_mesh_node(&s_mesh, 0, &nd);
    Serial.printf("meshtastic: %d node(s) heard\n", count);
    for (int i = 0; i < count; i++) {
        mt_mesh_node(&s_mesh, i, &nd);
        if (!nd) continue;
        Serial.printf("  !%08lx %-5s %-24s snr %d, %lus ago%s\n", (unsigned long)nd->num,
                      nd->short_name, nd->long_name, nd->snr,
                      (unsigned long)((millis() - nd->heard_ms) / 1000), nd->xprs ? " (xprs)" : "");
    }
}

/* ── What we say ─────────────────────────────────────────────────────────
 *
 * The observation beacon of XPRS 10.6.1 with the hears: list of 10.6.3, and
 * signed (9.1) so a receiver can hold the callsign to the key it learned
 * from our t:identity. The signature is 65 bytes; an observation with eight
 * neighbours is still well inside a wire. */
static int beacon(char *out, int cap, const char *link, int peers)
{
    int n = snprintf(out, (size_t)cap, "t:observation f:%s link:%s peers:%d", nst_call(), link, peers);
    if (n <= 0 || n >= cap) return n;
    n += hears_render(out + n, cap - n - (5 + XPRSSIG_B85_LEN));
    return nst_sign(out, n, cap);
}

static int lora_beacon(char *out, int cap) { return beacon(out, cap, "lora", xb_peer_count(&s_lora, 600)); }
static int ble_beacon(char *out, int cap)  { return beacon(out, cap, "ble",  xb_peer_count(&s_ble, 600)); }

/* t:identity (9.3): the key behind the callsign, so a neighbour can verify
 * every signature after it. Once at 30 s so a station that just came up is
 * findable, then every 30 minutes (18.1) -- a binding that never changes is
 * not worth a fresh archive record a minute. Same cadence as xprs_app.c. */
static void air_identity(void)
{
    if (!nst_have_key()) return;
    char wire[XPRS_MAX_WIRE + 1], tf[32];
    nst_time_field(tf, sizeof tf);
    int n = snprintf(wire, sizeof wire, "t:identity f:%s %s k:%s", nst_call(), tf, nst_npub());
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = nst_sign(wire, n, (int)sizeof wire);
    if (s_radio_up) xb_send(&s_lora, wire, n);
    if (s_ble_up)   xb_send(&s_ble, wire, n);
}

static bool ble_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_ble_up || len > BLE_WIRE_MAX) return false;
    uint8_t ad[TN_ADV_DATA_MAX];
    int i = 0;
    ad[i++] = (uint8_t)(5 + len);          /* type + company(2) + marker + subtype + wire */
    ad[i++] = 0xFF;
    ad[i++] = BLE_COMPANY_LO; ad[i++] = BLE_COMPANY_HI;
    ad[i++] = BLE_MARKER;     ad[i++] = BLE_SUB_XPRS;
    memcpy(ad + i, wire, len); i += len;
    if (tn_adv_set_data(ad, (size_t)i) != TN_OK) return false;
    Serial.printf("ble: aired %dB %.*s\n", len, len > 60 ? 60 : len, wire);
    led_blip(P1_LED_MESH, 20);
    return true;
}

static const xb_ops_t k_ble_ops = {
    .air = ble_air, .now_ms = lora_now_ms, .random = lora_random,
    .lock = NULL, .unlock = NULL, .drain = NULL, .ctx = NULL, .name = "ble",
};

/* Delivered from tn_gatt_pump() on loop(), so this may do real work. */
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
                if (r->evt_type & 0x0001) {          /* connectable: remember who */
                    s_peer_addr_type = r->addr_type;
                    memcpy(s_peer_addr, r->addr, 6);
                    s_peer_known = true;
                    xprs_t x;
                    if (xprs_parse(wire, len, &x)) xprs_get_str(&x, "f", s_peer_call, sizeof s_peer_call);
                }
                xb_on_wire(&s_ble, wire, len, peer_of(wire, len, NULL), r->rssi);
            } else if ((r->evt_type & 0x0001) && p[5] == 0x41 && len > 0 && len <= 9) {
                /* The probe's APRS-subtype advert: connectable, callsign only. */
                s_peer_addr_type = r->addr_type;
                memcpy(s_peer_addr, r->addr, 6);
                s_peer_known = true;
                snprintf(s_peer_call, sizeof s_peer_call, "%.*s", len, wire);
            }
            return;
        }
        p += 1 + n;
    }
}

/* ── The mesh channel test: dial a deck, get our bytes echoed back ──── */

static uint32_t s_gatt_rx, s_gatt_tx;
static void blob_maybe_start(void);
static void gatt_connected(void *c, uint16_t conn, bool central)
{ (void)c; Serial.printf("gatt: link 0x%04x ready (%s), %d bytes per send\n",
                          conn, central ? "we dialled" : "dialled in", tn_gatt_mtu());
  if (central) blob_maybe_start(); }
static void gatt_disconnected(void *c, uint16_t conn, uint8_t reason)
{ (void)c; Serial.printf("gatt: link 0x%04x closed (0x%02x)\n", conn, reason); }
/* One answer wire, back over the same 1:1 link the command arrived on. */
static void gatt_reply(const char *wire, int len)
{
    int rc = tn_gatt_send((const uint8_t *)wire, len);
    if (rc != 0) Serial.printf("gatt_reply: rc=%d len=%d\n", rc, len);
    s_gatt_tx++;
}

/* Bytes off the GATT link (docs/ble5-gatt.md). A t:command addressed to us is
 * the OTA path on its private, connection-speed transport -- the image rides
 * here, 128-byte chunks at the link interval, not on the broadcast plane and
 * not through a gateway's re-air. Verified the same way: a connection is
 * private, not authentic, so the signature still decides. Anything else is
 * printed, as before. */
/* ── XBLOB: the fast 1:1 GATT image transfer (docs/ble5-gatt.md) ─────────
 *
 * We are the RECEIVER (central): we dial the station that holds the image and
 * pull it as raw parcels, verifying and re-requesting through xprs_blob. The
 * signed cmd:update has already opened the authenticated session in update.cpp
 * (xfw_pending gives us its sha/size); XBLOB just fills STAGE far faster than
 * the base85 cmd:zfw text lane, which stays as the fallback. */
static int p1_scan_on(void);
static xblob_t s_blob;
static bool    s_blob_active;

static int blob_send(void *c, const uint8_t *f, int n)
{
    (void)c;
    if (!tn_gatt_connected()) return -1;
    int rc = tn_gatt_send((const uint8_t *)f, n);
    if (rc == 0) { s_gatt_tx++; return XBLOB_SEND_OK; }
    static uint32_t fails;
    if ((fails++ % 8) == 0) Serial.printf("blob: send rc=%d len=%d\n", rc, n);
    return XBLOB_SEND_BUSY;   /* NRF_ERROR_RESOURCES: retry (rare on the small control frames we send) */
}
static int blob_write(void *c, uint32_t off, const uint8_t *src, int len)
{ (void)c; return xfw_blob_write(off, src, len); }
static void blob_done(void *c, bool ok)
{
    (void)c;
    s_blob_active = false;
    p1_scan_on();                          /* the beacon plane comes back */
    if (ok) { Serial.println("xblob: complete -- installing"); xfw_blob_finish(s_blob.sig85); }
    else    Serial.println("xblob: gave up -- cmd:zfw fallback remains");
}
static const xblob_ops_t k_blob_ops = { NULL, blob_send, NULL, blob_write, blob_done };

/* Start pulling the pending image over the link we just dialled. */
static void blob_maybe_start(void)
{
    uint8_t sha[32]; uint32_t size;
    if (s_blob_active || !tn_gatt_connected()) return;
    if (!xfw_pending(sha, &size)) return;
    Serial.printf("xblob: dialling done, requesting %lu-byte image\n", (unsigned long)size);
    /* The scanner runs at 83%% duty and starves the connection's events on
     * BOTH radios; a bulk transfer gets the leftovers -- measured at ~3
     * frames a second. Silence it for the transfer; blob_done puts it back. */
    tn_scan_stop();
    xfw_blob_reset();                      /* fresh pages for a fresh attempt */
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
        blob_maybe_start();   /* a cmd:update just opened the session -> pull it fast */
        return;
    }
    if (xprs_parse((const char *)d, n, &p)) {
        char from[16] = "";
        xprs_get_str(&p, "f", from, sizeof from);
        Serial.printf("gatt 1:1 rx %dB from %s: %.*s\n", n, from,
                      n > 90 ? 90 : n, (const char *)d);
        return;
    }
    Serial.printf("gatt rx %dB: %.*s\n", n, n > 60 ? 60 : n, (const char *)d);
}
static const tn_gatt_cb_t k_gatt_cb = { gatt_connected, gatt_disconnected, gatt_rx, NULL };

/* The scan bring-up, callable again after a bulk transfer paused it. */
static int p1_scan_on(void)
{
    tn_scan_cfg_t scan = { .own_addr_type = 0x01, .passive = 1,
                           .itvl = 0x0060, .window = 0x0050, .phy = TN_PHY_1M };
    return tn_scan_start(&scan, ble_report, NULL);
}

static int s_ble_err;     /* the last bring-up verdict, for '?' */
static void ble_begin(void)
{
    if (s_ble_up) return;
    s_ble_err = tn_start();
    if (s_ble_err != TN_OK) { Serial.printf("ble: SoftDevice would not start (%d)\n", s_ble_err); return; }
    uint8_t addr[6];
    uint32_t a = NRF_FICR->DEVICEADDR[0], b = NRF_FICR->DEVICEADDR[1];
    addr[0] = a; addr[1] = a >> 8; addr[2] = a >> 16; addr[3] = a >> 24;
    addr[4] = b; addr[5] = (b >> 8) | 0xC0;                /* static random */
    tn_set_random_addr(addr);
    Serial.printf("ble: our address %02X:%02X:%02X:%02X:%02X:%02X (static random)\n",
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    tn_adv_cfg_t adv = {
        /* CONNECTABLE: the beacon set is also this station's door. A phone
         * that hears us can dial in and push a firmware image over XBLOB or
         * hand us a 1:1 -- the serve below is what answers (ble5-gatt.md). */
        .handle = 0, .props = TN_ADV_PROP_CONNECTABLE, .itvl_min = 0x100, .itvl_max = 0x100,
        .chan_map = 0x07, .own_addr_type = 0x01, .tx_power = 127,
        .primary_phy = TN_PHY_1M, .secondary_phy = TN_PHY_1M, .sid = 0,
    };
    tn_adv_configure(&adv);
    s_ble_err = p1_scan_on();
    if (s_ble_err != TN_OK) return;
    s_ble_up = true;
    int sv = tn_gatt_serve(&k_gatt_cb);
    Serial.printf("ble: up -- BLE5 extended advertising (connectable), scanning, serve=%d\n", sv);
}

static void console(int c)
{
    switch (c) {
    case 'd':
        if (!s_peer_known) { Serial.println("dial: no connectable station heard yet"); break; }
        Serial.printf("dial: %s at %02x:%02x:%02x:%02x:%02x:%02x\n", s_peer_call,
                      s_peer_addr[5], s_peer_addr[4], s_peer_addr[3],
                      s_peer_addr[2], s_peer_addr[1], s_peer_addr[0]);
        Serial.printf("dial: %d\n", tn_gatt_dial(s_peer_addr_type, s_peer_addr, &k_gatt_cb));
        break;
    case 'm': {
        char msg[64];
        int n = snprintf(msg, sizeof msg, "hello from %s #%lu", nst_call(), (unsigned long)++s_gatt_tx);
        Serial.printf("gatt tx: %d\n", tn_gatt_send((const uint8_t *)msg, n));
        break; }
    case 'M': {   /* the biggest frame one send carries */
        int n = tn_gatt_mtu();
        static uint8_t big[TN_ATT_MTU_MAX];
        for (int i = 0; i < n; i++) big[i] = 'A' + (i % 26);
        Serial.printf("gatt tx %dB: %d\n", n, tn_gatt_send(big, n));
        s_gatt_tx++;
        break; }
    case 'x': Serial.printf("hangup: %d\n", tn_gatt_disconnect()); break;
    case 'b': ble_begin(); break;                /* bring-up, watched live */
    case 'k':                                    /* who we are, publicly */
        Serial.printf("call=%s npub=%s boots=%lu\n", nst_call(), nst_have_key() ? nst_npub() : "-",
                      (unsigned long)nst_boot_epoch());
        break;
    case 'K': {                                  /* the private half -- a backup, on request only */
        char nsec[80] = "-";
        if (nst_have_key()) bech32_encode("nsec", nst_priv(), 32, nsec, sizeof nsec);
        Serial.printf("nsec=%s\n", nsec);
        break; }
    case 'I': {                                  /* I<nsec>\n: adopt a key */
        char line[96]; int n = 0;
        for (uint32_t t0 = millis(); millis() - t0 < 5000 && n < (int)sizeof line - 1; ) {
            int ch = Serial.read();
            if (ch < 0) { delay(2); continue; }
            if (ch == '\r' || ch == '\n') break;
            line[n++] = (char)ch;
        }
        line[n] = 0;
        nst_key_import(line);
        break; }
    case 'i': air_identity(); break;             /* say who we are, now */
    case 'n': mesh_nodes(); break;               /* the Meshtastic nodes heard */
    case 'U': xfw_selftest(); break;             /* prove the flash path, non-destructively */
    case 'c': {                                  /* cfg set|get|list ... */
        char line[128]; int n = 0;
        for (uint32_t t0 = millis(); millis() - t0 < 10000 && n < (int)sizeof line - 1; ) {
            int ch = Serial.read();
            if (ch < 0) { delay(2); continue; }
            if (ch == '\r' || ch == '\n') break;
            line[n++] = (char)ch;
        }
        line[n] = 0;
        if (strncmp(line, "fg ", 3) == 0) nst_cfg_console(line + 3);
        else Serial.println("cfg set <key> <value> | cfg get <key> | cfg list");
        break; }
    case 'D':                                    /* into the bootloader, cleanly */
        Serial.println("rebooting into DFU");
        Serial.flush(); delay(50);
        sd_power_gpregret_set(0, 0x4E);          /* DFU_MAGIC_SERIAL_ONLY_RESET */
        sd_softdevice_disable();
        NVIC_SystemReset();
        break;
    case '?':
        Serial.printf("call=%s fw=%s%s clock=%lu key=%d lora=%d(%s %.3f MHz SF%u) ble=%d(err %d) link=%s peer=%s gatt rx=%lu tx=%lu\n",
                      nst_call(), xfw_version(), xfw_probation() ? "(probation)" : "",
                      (unsigned long)nst_now(),
                      (int)nst_have_key(), (int)s_radio_up, s_net.name, s_net.freq_mhz,
                      (unsigned)s_net.sf, (int)s_ble_up, s_ble_err,
                      tn_gatt_connected() ? "UP" : "none",
                      s_peer_known ? s_peer_call : "-",
                      (unsigned long)s_gatt_rx, (unsigned long)s_gatt_tx);
        if (s_mesh_on) {
            const mt_mesh_stats_t *m = &s_mesh.st;
            Serial.printf("meshtastic: node !%08lx frames=%lu decoded=%lu dupes=%lu relayed=%lu "
                          "cancelled=%lu text in=%lu out=%lu nodeinfo=%lu pki in=%lu fail=%lu "
                          "no_clock=%lu dropped=%lu\n",
                          (unsigned long)s_self_node, (unsigned long)m->rx_frames,
                          (unsigned long)m->rx_decoded, (unsigned long)m->rx_dupes,
                          (unsigned long)m->relayed, (unsigned long)m->relay_cancelled,
                          (unsigned long)m->text_in, (unsigned long)m->text_out,
                          (unsigned long)m->nodeinfo_out, (unsigned long)m->pki_in,
                          (unsigned long)m->pki_fail, (unsigned long)m->no_clock,
                          (unsigned long)m->dropped);
        }
        break;
    default: break;
    }
}

/* ── Bring-up ────────────────────────────────────────────────────────── */

/* `lora_mode` from /xprs/cfg: meshtastic (the default) or xprs. */
static void lora_pick(void)
{
    const char *m = xcfg_get("lora_mode", "meshtastic");
    if (strcmp(m, "xprs") == 0) return;
    if (strcmp(m, "meshtastic") != 0)
        Serial.printf("lora: mode %s is not on this board -- meshtastic\n", m);
    const mt_region_t *r = mt_region_find(xcfg_get("lora_region", "eu"));
    if (!r) r = mt_region_find("eu");
    s_net.name     = "meshtastic";
    s_net.freq_mhz = (float)mt_slot_freq_hz(r, MT_CH_NAME_LONGFAST, MT_LF_BW_HZ) / 1e6f;
    s_net.bw_khz   = MT_LF_BW_HZ / 1000.0f;
    s_net.sf       = MT_LF_SF;
    s_net.sync     = MT_LF_SYNC;
    s_net.preamble = MT_LF_PREAMBLE;
    s_mt = true;
}

static bool lora_begin(void)
{
    /* See board.h: this line is the first suspect if the radio hears
     * nothing. The datasheet names the pin and not what it selects. */
    pinMode(P1_LORA_RF_SW, OUTPUT);
    digitalWrite(P1_LORA_RF_SW, HIGH);

    s_spi.begin();

    int st = s_radio.begin(s_net.freq_mhz, s_net.bw_khz, s_net.sf, LORA_CR,
                           s_net.sync, LORA_POWER_DBM, s_net.preamble);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: begin failed (%d) -- no radio this boot\n", st);
        return false;
    }

    /* The Wio-SX1262 carries its own antenna switch on the chip's DIO2. If
     * this board turns out to drive the path from P0.05 instead, this is the
     * other half of that question -- see board.h. */
    s_radio.setDio2AsRfSwitch(true);
    s_radio.setCurrentLimit(140.0);

    s_radio.setPacketReceivedAction(dio1_isr);
    st = s_radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: startReceive failed (%d)\n", st);
        return false;
    }
    Serial.printf("lora: up on %s, %.3f MHz SF%u BW%.0f sync 0x%02X preamble %u, %d dBm\n",
                  s_net.name, s_net.freq_mhz, (unsigned)s_net.sf, s_net.bw_khz,
                  (unsigned)s_net.sync, (unsigned)s_net.preamble, LORA_POWER_DBM);
    return true;
}

/* The GNSS is wired through a TPS22916 load switch and this holds it OFF.
 * It is a receiver, not a bearer; nothing in this firmware reads it yet, and
 * on a board that lives on 5 W of sun an unused receiver that is merely
 * asleep is a current draw nobody chose. */
static void gnss_off(void)
{
    pinMode(P1_GNSS_POWER_EN, OUTPUT);
    digitalWrite(P1_GNSS_POWER_EN, LOW);
}

/* Keep the bearers moving for [ms]: what the updater calls so its last
 * answer is on the air before it takes the SoftDevice down. */
static void bearers_flush(uint32_t ms)
{
    for (uint32_t t0 = millis(); millis() - t0 < ms; ) {
        xb_tick(&s_lora, millis());
        if (s_ble_up) { tn_gatt_pump(); xb_tick(&s_ble, millis()); }
        delay(5);
    }
}

/* ── The station's task ──────────────────────────────────────────────────
 *
 * The Arduino core runs setup() and loop() on a task with a 4 KB stack
 * (cores/nRF5/main.cpp, LOOP_STACK_SZ) and does not let a sketch ask for
 * more. A signature is mbedtls scalar multiplication over secp256k1 plus
 * three wire-sized buffers on the way in and out, and that overflowed it:
 * the board fell silent between "ble: up" and the first key line, with
 * nothing on the port to say so. So the station is its own task with a
 * stack sized for the work, and the core's loop() only parks. */
#define STATION_STACK_WORDS 3072     /* 12 KB */
static void station_setup(void);
static void station_loop(void);
static void station_task(void *arg)
{
    (void)arg;
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
    /* A BOUNDED wait for the USB host, so a monitor attached at boot sees the
     * bring-up lines. Bounded, because this board's normal life is on a pole
     * with nothing attached, and a station that will not start without a
     * serial monitor is a station that does not start. */
    for (uint32_t t0 = millis(); !Serial && millis() - t0 < 2500; ) delay(10);
    delay(100);

    pinMode(P1_LED_USER, OUTPUT);
    pinMode(P1_LED_MESH, OUTPUT);
    led(P1_LED_USER, false);
    led(P1_LED_MESH, false);

    gnss_off();

    /* THE FILESYSTEM BEFORE THE SOFTDEVICE, and not the other way round.
     * With the SoftDevice up, the core's flash layer (flash_nrf5x.c) goes
     * through sd_flash_* and then blocks on a semaphore that only the SoC
     * event NRF_EVT_FLASH_OPERATION_SUCCESS gives -- and on this firmware
     * those events are pumped by tn_gatt_pump(), on this very task. The
     * first write deadlocked the station between "ble: up" and the first
     * key line. Before the SoftDevice the same layer writes NVMC directly
     * and returns; so the key, the boot counter, everything on flash, is
     * done here, and the one write that can happen later (a key import)
     * takes the SoftDevice down first and reboots. The RNG works the same
     * way round: before the SoftDevice NRF_RNG is the application's, and
     * xprssig_platform_random() reads it directly. */
    static const nst_cfg_t k_nst = { "X3", false };
    nst_init(&k_nst);
    nst_cfg_load();
    static const xfw_cfg_t k_xfw = {
        .board = "sensecap-p1-pro", .call = nst_call(), .sign = nst_sign, .flush = bearers_flush,
    };
    xfw_init(&k_xfw, nst_boot_epoch());
    ble_begin();

    /* The watchdog: a station that hangs on a pole reboots itself, and a
     * new image that hangs three times is put back (update.cpp). 60 s,
     * fed from the station loop and from inside the copier. */
    NRF_WDT->CONFIG = WDT_CONFIG_SLEEP_Msk;
    NRF_WDT->CRV = 60 * 32768;
    NRF_WDT->RREN = 1;
    NRF_WDT->TASKS_START = 1;

    Serial.printf("\nXPRS station %s -- SenseCAP Solar Node P1-Pro (boot %lu)\n",
                  nst_call(), (unsigned long)nst_boot_epoch());
    Serial.println("headless: LoRa + BLE5, signing. No WiFi on this chip.");

    /* From the hardware RNG, not the device ID: a seed that is the same at
     * every boot makes every boot's frames the same, and a repeater that
     * heard the last boot's probe ten minutes ago drops this one as a
     * duplicate (measured on the T1000-E, 2026-10-07). */
    uint32_t seed = 0;
    xprssig_platform_random((uint8_t *)&seed, sizeof seed);
    randomSeed(seed ^ NRF_FICR->DEVICEID[0]);

    lora_pick();
    s_radio_up = lora_begin();

    xb_init(&s_lora, &k_lora_ops, nst_call());
    xb_set_rx_cb(&s_lora, on_lora);
    /* No observation beacon on Meshtastic's channel, as no ESP32 station
     * has one on LoRa: presence is Bluetooth's, the NodeInfo tells the mesh
     * we are here, and t:identity (worth it) still goes every 30 minutes. */
    if (!s_mt) xb_set_beacon(&s_lora, lora_beacon, BEACON_EVERY_SEC, BEACON_JITTER_SEC);
    xb_set_pace(&s_lora, LORA_PACE_MS);
    /* The rolling-hour ledger, from the same modem constants the radio was
     * given ten lines up, so the charge cannot drift from the wire. Band
     * g3: 360 s of airtime an hour, 6 s of it kept for sos. */
    static xb_duty_t s_lora_duty;
    s_air = (xb_lora_air_t){
        .bw_hz = (uint32_t)(s_net.bw_khz * 1000.0f), .sf = s_net.sf,
        .cr = LORA_CR - 4, .preamble = s_net.preamble, .crc = true,
        .implicit_header = false,
    };
    /* On Meshtastic's channel a wire is one or two frames with a header
     * each, and the ledger is charged for what actually goes out. */
    struct air_fn { static uint32_t ms(int len, void *ctx) {
        (void)ctx;
        if (!s_mt) return xb_lora_airtime_ms(&s_air, len);
        uint32_t t = 0;
        for (int i = 0; i < mt_xprs_frames_for(len); i++)
            t += xb_lora_airtime_ms(&s_air, mt_xprs_frame_len(len, i));
        return t; } };
    xb_set_duty(&s_lora, &s_lora_duty, air_fn::ms, NULL,
                360000u, 6000u, 0u);
    xb_set_driver(s_radio_up);

    s_self_node = mt_node_of_call(nst_call(), (int)strlen(nst_call()));
    if (s_mt && s_radio_up) mesh_begin();

    xb_init(&s_ble, &k_ble_ops, nst_call());
    xb_set_rx_cb(&s_ble, on_ble);
    xb_set_beacon(&s_ble, ble_beacon, BEACON_EVERY_SEC, BEACON_JITTER_SEC);

    /* Three slow blinks on the mesh LED: the station started. Somebody under
     * the pole with no cable has this and nothing else. */
    for (int i = 0; i < 3 && s_radio_up; i++) { led_blip(P1_LED_MESH, 120); delay(120); }
}

static void station_loop(void)
{
    if (s_rx_pending) {
        s_rx_pending = false;

        uint8_t buf[MT_FRAME_MAX + 1];
        size_t n = s_radio.getPacketLength();
        if (s_mt && n > 0 && n <= MT_FRAME_MAX) {
            int st = s_radio.readData(buf, n);
            if (st == RADIOLIB_ERR_NONE)
                lora_rx_mt(buf, (int)n, (int)s_radio.getRSSI(), (int)s_radio.getSNR());
            else
                Serial.printf("lora: readData failed (%d)\n", st);
        } else if (s_mt) {
            Serial.printf("lora: dropped a %u-byte frame\n", (unsigned)n);
        } else if (n > 0 && n <= XB_WIRE_MAX) {
            int st = s_radio.readData(buf, n);
            if (st == RADIOLIB_ERR_NONE) {
                buf[n] = 0;
                /* Straight through: unlike the ESP-NOW bearer, nothing here
                 * runs on a borrowed task, so the SHA-256 an identifier costs
                 * is paid on loop() where it belongs and no drain hook is
                 * needed (xprsbearer.h). */
                xb_on_wire(&s_lora, (const char *)buf, (int)n,
                           peer_of((const char *)buf, (int)n, NULL),
                           (int)s_radio.getRSSI());
            } else {
                Serial.printf("lora: readData failed (%d)\n", st);
            }
        } else if (n > XB_WIRE_MAX) {
            Serial.printf("lora: dropped a %u-byte frame, longer than a wire\n",
                          (unsigned)n);
        }
        s_radio.startReceive();
    }

    xb_tick(&s_lora, millis());
    if (s_mesh_on) { mt_mesh_tick(&s_mesh, millis()); mesh_drain(); }
    if (s_ble_up) { tn_gatt_pump(); xb_tick(&s_ble, millis()); }
    if (s_blob_active) {
        xblob_tick(&s_blob, millis());
        static uint32_t next_bp;
        if ((int32_t)(millis() - next_bp) >= 0) {
            next_bp = millis() + 2000;
            Serial.printf("xblob: st=%u hashes=%u/%u blocks=%u/%u rounds=%u consumed=%lu\n",
                          s_blob.state, s_blob.hashes_got, s_blob.nblocks,
                          s_blob.got, s_blob.nblocks, s_blob.rounds,
                          (unsigned long)s_blob.consumed);
        }
    }
    xfw_tick(millis(), s_radio_up);
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;

    int c = Serial.read();
    if (c > 0) console(c);

    /* 30 s once, then every 30 minutes (9.3, 18.1). */
    static uint32_t next_identity = 30000;
    static bool     said_once;
    if ((int32_t)(millis() - next_identity) >= 0) {
        air_identity();
        said_once = true;
        next_identity = millis() + (said_once ? 1800000UL : 30000UL);
    }

    /* Once a minute, the same shape of line the other boards print, so one
     * habit reads every station in the fleet. */
    static uint32_t next_alive;
    uint32_t now = millis();
    if ((int32_t)(now - next_alive) >= 0) {
        next_alive = now + 60000;
        uint32_t rx = 0, tx = 0, cancelled = 0;
        xb_stats(&s_lora, &rx, &tx, &cancelled);
        uint32_t brx = 0, btx = 0, bcan = 0;
        xb_stats(&s_ble, &brx, &btx, &bcan);
        uint32_t sgot, sof; xfw_progress(&sgot, &sof);
        Serial.printf("alive %lus call=%s fw=%s%s lora rx=%lu tx=%lu cancel=%lu peers=%d | "
                      "ble rx=%lu tx=%lu peers=%d | heard=%lu radio=%d",
                      (unsigned long)(now / 1000), nst_call(), xfw_version(), xfw_probation() ? "?" : "",
                      (unsigned long)rx, (unsigned long)tx, (unsigned long)cancelled,
                      xb_peer_count(&s_lora, 600),
                      (unsigned long)brx, (unsigned long)btx, xb_peer_count(&s_ble, 600),
                      (unsigned long)s_heard, (int)s_radio_up);
        if (s_mesh_on) Serial.printf(" | mt frames=%lu relayed=%lu in=%lu out=%lu",
                                     (unsigned long)s_mesh.st.rx_frames, (unsigned long)s_mesh.st.relayed,
                                     (unsigned long)s_mesh.st.text_in, (unsigned long)s_mesh.st.text_out);
        if (sof) Serial.printf(" | update %lu/%lu", (unsigned long)sgot, (unsigned long)sof);
        Serial.println();
    }

    delay(5);
}
