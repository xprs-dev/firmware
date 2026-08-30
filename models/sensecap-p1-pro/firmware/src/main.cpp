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
 *   Signing.  common/xprs_sig already has two backends chosen by
 *          preprocessor -- OpenSSL for the host harness, mbedtls on the
 *          ESP32s -- so the shape for a third is already there. It needs a
 *          secp256k1 for this chip. Until then this station's packets go out
 *          unsigned, which the spec allows and which every receiver can see.
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
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#include "board.h"

extern "C" {
#include "xprs.h"
#include "xprsbearer.h"
#include "tinynimble.h"
#include "tn_att.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
}

/* ── What this radio is set to ───────────────────────────────────────────
 *
 * The module covers 862-930 MHz and the supplied antenna is specified
 * 868-915 MHz, so all three of the bands anyone asks about are in range.
 * 868.0 is the EU default and what the T-Deck already uses, so two boards on
 * one bench meet without either being re-flashed.
 *
 * WHAT IS LEGAL IS NOT WHAT IS POSSIBLE. 22 dBm is the module's maximum and
 * is above the EU 868 limit for many duty-cycle classes; the pacing below is
 * a floor, not compliance advice, and the operator owns the final say.
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
 * The sync word is the one thing neither side sets: xprs_sx1262 never issues
 * SetLoRaSyncWord, so the chip keeps its reset default of 0x1424 -- the
 * private-network value, not LoRaWAN's public 0x3444. RadioLib writes that
 * same register from the one-byte shorthand 0x12. Same bytes on the air.
 */
#define LORA_FREQ_MHZ    868.0
#define LORA_BW_KHZ      125.0
#define LORA_SF          7
#define LORA_CR          5         /* 4/5, as the fleet uses */
#define LORA_SYNC_WORD   0x12      /* = 0x1424, the SX1262 private default */
#define LORA_POWER_DBM   22
#define LORA_PREAMBLE    8

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
static char            s_call[16];
static volatile bool   s_rx_pending;
static bool            s_radio_up;
static uint32_t        s_heard;

/* ── The callsign ────────────────────────────────────────────────────────
 *
 * Same shape and same alphabet as every other board's (xprs_app.c,
 * derive_callsign): X5 plus four characters from a 30-letter set with B, I,
 * O and 1 left out, so it cannot be misread aloud or off a screen.
 *
 * The source differs because the source has to. On an ESP32 this comes from
 * the WiFi MAC, and this chip has no WiFi. FICR.DEVICEID is the nRF52840's
 * factory-programmed 64-bit device id: unique, readable without a radio, and
 * stable across reflashes -- which is the whole requirement. An X3 callsign
 * would be better still because a receiver can re-derive it from the signing
 * key, and that is waiting on signing. */
static void derive_callsign(void)
{
    static const char *b32 = "ACDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint32_t v = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
    v &= 0x00FFFFFF;
    s_call[0] = 'X'; s_call[1] = '5';
    for (int i = 0; i < 4; i++) { s_call[2 + i] = b32[v % 30]; v /= 30; }
    s_call[6] = 0;
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

static bool lora_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_radio_up) return false;

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
static void on_lora(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    heard("lora", wire, len, rssi);
    if (s_ble_up) xb_offer(&s_ble, wire, len);
}

static void on_ble(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    heard("ble", wire, len, rssi);
    xb_offer(&s_lora, wire, len);
}

/* ── What we say ─────────────────────────────────────────────────────────
 *
 * The observation beacon of XPRS 10.6.1, minus the `hears:` list. That list
 * comes from xprs_station, which is not ported here yet; a beacon without it
 * is still a valid observation and still tells the network this station
 * exists, which is the part that matters from a pole. */
static int lora_beacon(char *out, int cap)
{
    return snprintf(out, (size_t)cap, "t:observation f:%s link:lora peers:%d",
                    s_call, xb_peer_count(&s_lora, 600));
}

static int ble_beacon(char *out, int cap)
{
    return snprintf(out, (size_t)cap, "t:observation f:%s link:ble peers:%d",
                    s_call, xb_peer_count(&s_ble, 600));
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
                xb_on_wire(&s_ble, wire, len, 0, r->rssi);
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
static void gatt_connected(void *c, uint16_t conn, bool central)
{ (void)c; Serial.printf("gatt: link 0x%04x ready (%s), %d bytes per send\n",
                          conn, central ? "we dialled" : "dialled in", tn_gatt_mtu()); }
static void gatt_disconnected(void *c, uint16_t conn, uint8_t reason)
{ (void)c; Serial.printf("gatt: link 0x%04x closed (0x%02x)\n", conn, reason); }
static void gatt_rx(void *c, const uint8_t *d, int n)
{ (void)c; s_gatt_rx++; Serial.printf("gatt rx %dB: %.*s\n", n, n, (const char *)d); }
static const tn_gatt_cb_t k_gatt_cb = { gatt_connected, gatt_disconnected, gatt_rx, NULL };

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
    tn_adv_cfg_t adv = {
        .handle = 0, .props = 0, .itvl_min = 0x100, .itvl_max = 0x100,
        .chan_map = 0x07, .own_addr_type = 0x01, .tx_power = 127,
        .primary_phy = TN_PHY_1M, .secondary_phy = TN_PHY_1M, .sid = 0,
    };
    tn_adv_configure(&adv);
    tn_scan_cfg_t scan = { .own_addr_type = 0x01, .passive = 1,
                           .itvl = 0x0060, .window = 0x0050, .phy = TN_PHY_1M };
    s_ble_err = tn_scan_start(&scan, ble_report, NULL);
    if (s_ble_err != TN_OK) return;
    s_ble_up = true;
    Serial.println("ble: up -- BLE5 extended advertising, scanning");
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
        int n = snprintf(msg, sizeof msg, "hello from %s #%lu", s_call, (unsigned long)++s_gatt_tx);
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
    case 'D':                                    /* into the bootloader, cleanly */
        Serial.println("rebooting into DFU");
        Serial.flush(); delay(50);
        sd_power_gpregret_set(0, 0x4E);          /* DFU_MAGIC_SERIAL_ONLY_RESET */
        sd_softdevice_disable();
        NVIC_SystemReset();
        break;
    case '?':
        Serial.printf("call=%s lora=%d ble=%d(err %d) link=%s peer=%s gatt rx=%lu tx=%lu\n",
                      s_call, (int)s_radio_up, (int)s_ble_up, s_ble_err,
                      tn_gatt_connected() ? "UP" : "none",
                      s_peer_known ? s_peer_call : "-",
                      (unsigned long)s_gatt_rx, (unsigned long)s_gatt_tx);
        break;
    default: break;
    }
}

/* ── Bring-up ────────────────────────────────────────────────────────── */

static bool lora_begin(void)
{
    /* See board.h: this line is the first suspect if the radio hears
     * nothing. The datasheet names the pin and not what it selects. */
    pinMode(P1_LORA_RF_SW, OUTPUT);
    digitalWrite(P1_LORA_RF_SW, HIGH);

    s_spi.begin();

    int st = s_radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                           LORA_SYNC_WORD, LORA_POWER_DBM, LORA_PREAMBLE);
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
    Serial.printf("lora: up on %.1f MHz SF%d BW%.0f, %d dBm\n",
                  LORA_FREQ_MHZ, LORA_SF, LORA_BW_KHZ, LORA_POWER_DBM);
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

void setup(void)
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
    derive_callsign();

    Serial.printf("\nXPRS station %s -- SenseCAP Solar Node P1-Pro\n", s_call);
    Serial.println("headless, one bearer: LoRa. No WiFi on this chip.");

    randomSeed(NRF_FICR->DEVICEID[0]);

    s_radio_up = lora_begin();

    xb_init(&s_lora, &k_lora_ops, s_call);
    xb_set_rx_cb(&s_lora, on_lora);
    xb_set_beacon(&s_lora, lora_beacon, BEACON_EVERY_SEC, BEACON_JITTER_SEC);
    xb_set_pace(&s_lora, LORA_PACE_MS);
    xb_set_driver(s_radio_up);

    ble_begin();
    xb_init(&s_ble, &k_ble_ops, s_call);
    xb_set_rx_cb(&s_ble, on_ble);
    xb_set_beacon(&s_ble, ble_beacon, BEACON_EVERY_SEC, BEACON_JITTER_SEC);

    /* Three slow blinks on the mesh LED: the station started. Somebody under
     * the pole with no cable has this and nothing else. */
    for (int i = 0; i < 3 && s_radio_up; i++) { led_blip(P1_LED_MESH, 120); delay(120); }
}

void loop(void)
{
    if (s_rx_pending) {
        s_rx_pending = false;

        uint8_t buf[XB_WIRE_MAX + 1];
        size_t n = s_radio.getPacketLength();
        if (n > 0 && n <= XB_WIRE_MAX) {
            int st = s_radio.readData(buf, n);
            if (st == RADIOLIB_ERR_NONE) {
                buf[n] = 0;
                /* Straight through: unlike the ESP-NOW bearer, nothing here
                 * runs on a borrowed task, so the SHA-256 an identifier costs
                 * is paid on loop() where it belongs and no drain hook is
                 * needed (xprsbearer.h). */
                xb_on_wire(&s_lora, (const char *)buf, (int)n,
                           0, (int)s_radio.getRSSI());
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
    if (s_ble_up) { tn_gatt_pump(); xb_tick(&s_ble, millis()); }

    int c = Serial.read();
    if (c > 0) console(c);

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
        Serial.printf("alive %lus call=%s lora rx=%lu tx=%lu cancel=%lu peers=%d | "
                      "ble rx=%lu tx=%lu peers=%d | heard=%lu radio=%d\n",
                      (unsigned long)(now / 1000), s_call,
                      (unsigned long)rx, (unsigned long)tx, (unsigned long)cancelled,
                      xb_peer_count(&s_lora, 600),
                      (unsigned long)brx, (unsigned long)btx, xb_peer_count(&s_ble, 600),
                      (unsigned long)s_heard, (int)s_radio_up);
    }

    delay(5);
}
