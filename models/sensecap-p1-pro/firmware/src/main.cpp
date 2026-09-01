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
}
#include "update.h"
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
 * The sync word is the one thing neither side sets: xprs_sx1262 never issues
 * SetLoRaSyncWord, so the chip keeps its reset default of 0x1424 -- the
 * private-network value, not LoRaWAN's public 0x3444. RadioLib writes that
 * same register from the one-byte shorthand 0x12. Same bytes on the air.
 */
#define LORA_FREQ_MHZ    869.5
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

/* ── The key, and the callsign that follows from it ──────────────────────
 *
 * XPRS 3: an X3 callsign is a station's and is derived from its public key,
 * so anybody who hears the t:identity can re-derive the callsign and check
 * that the two belong together. The derivation is the ESP32 boards'
 * (common/xprs_nostr/nostr_keys.c, nostr_keys_derive_callsign): the key's
 * bech32 npub, and the four characters after "npub1", uppercased.
 *
 * The private scalar is generated once from the SoftDevice's RNG and kept
 * in the internal LittleFS the Adafruit core provides -- the nRF52 has no
 * NVS, and a file in a filesystem the bootloader also leaves alone is the
 * same promise. Reflashing the application keeps it; a chip erase does not,
 * which is what 'K' (print the nsec) is for.
 *
 * Earlier images derived an X5 callsign from FICR.DEVICEID. X5 is a GROUP
 * (XPRS 3, section 26), not a station, and the suffix came from nothing a
 * receiver could check; that was wrong on both counts and is gone. */
#define KEY_PATH   "/xprs/key"
#define BOOT_PATH  "/xprs/boot"

static uint8_t  s_priv[XPRSSIG_KEY_LEN];
static uint8_t  s_pub[XPRSSIG_KEY_LEN];
static char     s_npub[80];
static bool     s_have_key;
static uint32_t s_boot_epoch;

/* Entropy for the signer (xprssig.h). With the SoftDevice up, NRF_RNG is
 * its and the application asks it; before that the peripheral is ours.
 * sd_rand_application_vector_get() returns NOT_ENOUGH_VALUES while its pool
 * refills, so the loop simply waits -- a few hundred microseconds a byte. */
extern "C" void xprssig_platform_random(uint8_t *out, size_t len)
{
    uint8_t sd_on = 0;
    sd_softdevice_is_enabled(&sd_on);
    size_t done = 0;
    while (done < len) {
        if (sd_on) {
            uint8_t avail = 0;
            sd_rand_application_bytes_available_get(&avail);
            uint8_t take = avail;
            if (take > len - done) take = (uint8_t)(len - done);
            if (take == 0) { delay(1); continue; }
            if (sd_rand_application_vector_get(out + done, take) == NRF_SUCCESS) done += take;
        } else {
            NRF_RNG->TASKS_START = 1;
            while (!NRF_RNG->EVENTS_VALRDY) { }
            NRF_RNG->EVENTS_VALRDY = 0;
            out[done++] = (uint8_t)NRF_RNG->VALUE;
            NRF_RNG->TASKS_STOP = 1;
        }
    }
}

static bool file_read(const char *path, uint8_t *buf, size_t n)
{
    File f(InternalFS);
    if (!f.open(path, FILE_O_READ)) return false;
    size_t got = f.read(buf, n);
    f.close();
    return got == n;
}

static bool file_write(const char *path, const uint8_t *buf, size_t n)
{
    InternalFS.remove(path);
    File f(InternalFS);
    if (!f.open(path, FILE_O_WRITE)) return false;
    size_t put = f.write(buf, n);
    f.close();
    return put == n;
}

static void derive_callsign(void)
{
    if (!s_have_key) { snprintf(s_call, sizeof s_call, "X3????"); return; }
    s_call[0] = 'X'; s_call[1] = '3';
    for (int i = 0; i < 4; i++) s_call[2 + i] = (char)toupper((unsigned char)s_npub[5 + i]);
    s_call[6] = 0;
}

/* Public half, npub and callsign from whatever scalar is in s_priv. */
static bool key_adopt(void)
{
    if (!xprssig_public_key(s_priv, s_pub)) return false;
    if (bech32_encode("npub", s_pub, sizeof s_pub, s_npub, sizeof s_npub) != ESP_OK) return false;
    s_have_key = true;
    derive_callsign();
    return true;
}

static void keys_init(void)
{
    InternalFS.begin();
    InternalFS.mkdir("/xprs");

    /* 10.7: the boots ordinal, so a clockless station's packets can still
     * be ordered by a receiver. Same thing the ESP32s keep in NVS. */
    uint8_t b[4] = {0};
    if (file_read(BOOT_PATH, b, 4)) s_boot_epoch = (uint32_t)b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    s_boot_epoch++;
    b[0] = s_boot_epoch; b[1] = s_boot_epoch >> 8; b[2] = s_boot_epoch >> 16; b[3] = s_boot_epoch >> 24;
    file_write(BOOT_PATH, b, 4);

    if (file_read(KEY_PATH, s_priv, sizeof s_priv) && key_adopt()) {
        Serial.printf("key: loaded, callsign %s\n", s_call);
        return;
    }
    if (!xprssig_generate(s_priv) || !key_adopt()) {
        Serial.println("key: could not generate -- this station will not sign");
        s_have_key = false;
        derive_callsign();
        return;
    }
    bool kept = file_write(KEY_PATH, s_priv, sizeof s_priv);
    Serial.printf("key: generated, callsign %s -- %s\n", s_call, kept ? "kept" : "NOT SAVED");
}

/* 'I' on the console, followed by an nsec and a newline: adopt somebody
 * else's key -- the way a replaced board keeps the callsign the pole is
 * known by. Same as the ESP32 boards' nostr import. */
static void key_import(const char *nsec)
{
    char hrp[8]; uint8_t priv[64]; size_t n = sizeof priv;
    if (bech32_decode(nsec, hrp, priv, &n) != ESP_OK || n != 32 || strcmp(hrp, "nsec") != 0) {
        Serial.println("import: not an nsec"); return;
    }
    uint8_t keep[32]; memcpy(keep, s_priv, 32);
    memcpy(s_priv, priv, 32);
    if (!key_adopt()) { memcpy(s_priv, keep, 32); key_adopt(); Serial.println("import: not a valid key"); return; }
    /* Flash is only writable without a deadlock while the SoftDevice is
     * down (see station_setup), and a station whose callsign just changed
     * has to come up again under it anyway. */
    Serial.printf("import: now %s -- writing and rebooting\n", s_call);
    Serial.flush(); delay(50);
    sd_softdevice_disable();
    file_write(KEY_PATH, s_priv, sizeof s_priv);
    NVIC_SystemReset();
}

/* sig: on our own packets (9.1). Unsigned when there is no key or no room,
 * both of which the spec permits and a receiver can see. */
static int sign_wire(char *wire, int len, int cap)
{
    if (!s_have_key) return len;
    return xprsid_sign(wire, len, cap, s_priv);
}

/* ts: under a synced clock, epoch:<boots>.<uptime> otherwise (10.7). This
 * board has no clock source at all yet, so it is always the second. */
static int time_field(char *out, int cap)
{
    return snprintf(out, (size_t)cap, "epoch:%lu.%lu",
                    (unsigned long)s_boot_epoch, (unsigned long)(millis() / 1000));
}

/* ── Config: the allow-list and the firmware key ─────────────────────────
 *
 * The ESP32 boards keep `fwkey` and `own1..own4` in NVS (docs/device.md);
 * here they are lines of `key=value` in /xprs/cfg, read once before the
 * SoftDevice starts and served to xprs_auth and the updater through the
 * same xcfg_get() they call on an ESP32. `cfg set` on the console writes
 * the file with the SoftDevice down and reboots, for the reason
 * station_setup() gives. Re-writable with a cable, deliberately: a lost
 * key is a ladder, never a brick. */
#define CFG_PATH "/xprs/cfg"
#define CFG_MAX  8
static struct { char key[12]; char val[96]; } s_cfg_kv[CFG_MAX];

static void cfg_load(void)
{
    File f(InternalFS);
    if (!f.open(CFG_PATH, FILE_O_READ)) return;
    static char buf[CFG_MAX * 110];
    int n = f.read((uint8_t *)buf, sizeof buf - 1);
    f.close();
    if (n <= 0) return;
    buf[n] = 0;
    int k = 0;
    for (char *line = strtok(buf, "\n"); line && k < CFG_MAX; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        snprintf(s_cfg_kv[k].key, sizeof s_cfg_kv[k].key, "%s", line);
        snprintf(s_cfg_kv[k].val, sizeof s_cfg_kv[k].val, "%s", eq + 1);
        k++;
    }
}

extern "C" const char *xcfg_get(const char *key, const char *def)
{
    for (int i = 0; i < CFG_MAX; i++)
        if (s_cfg_kv[i].key[0] && strcmp(s_cfg_kv[i].key, key) == 0) return s_cfg_kv[i].val;
    return def;
}

/* `cfg set <key> <value>` / `cfg get <key>` / `cfg list`. A set writes and
 * reboots -- see station_setup() for why flash only moves before the
 * SoftDevice. */
static void cfg_console(char *line)
{
    char *cmd = strtok(line, " "), *key = strtok(NULL, " "), *val = strtok(NULL, "");
    if (!cmd) return;
    if (strcmp(cmd, "list") == 0) {
        for (int i = 0; i < CFG_MAX; i++)
            if (s_cfg_kv[i].key[0]) Serial.printf("%s=%s\n", s_cfg_kv[i].key, s_cfg_kv[i].val);
        return;
    }
    if (!key) { Serial.println("cfg: set <key> <value> | get <key> | list"); return; }
    if (strcmp(cmd, "get") == 0) { Serial.printf("%s=%s\n", key, xcfg_get(key, "")); return; }
    if (strcmp(cmd, "set") != 0) return;
    int slot = -1;
    for (int i = 0; i < CFG_MAX; i++) {
        if (strcmp(s_cfg_kv[i].key, key) == 0) { slot = i; break; }
        if (slot < 0 && !s_cfg_kv[i].key[0]) slot = i;
    }
    if (slot < 0) { Serial.println("cfg: full"); return; }
    snprintf(s_cfg_kv[slot].key, sizeof s_cfg_kv[slot].key, "%s", key);
    snprintf(s_cfg_kv[slot].val, sizeof s_cfg_kv[slot].val, "%s", val ? val : "");
    char out[CFG_MAX * 110]; int n = 0;
    for (int i = 0; i < CFG_MAX; i++)
        if (s_cfg_kv[i].key[0] && s_cfg_kv[i].val[0])
            n += snprintf(out + n, sizeof out - n, "%s=%s\n", s_cfg_kv[i].key, s_cfg_kv[i].val);
    Serial.printf("cfg: %s set -- writing and rebooting\n", key);
    Serial.flush(); delay(50);
    sd_softdevice_disable();
    file_write(CFG_PATH, (const uint8_t *)out, (size_t)n);
    NVIC_SystemReset();
}

/* What xprs_auth takes from the ESP32 stack, supplied here (xprs_auth.c). */
extern "C" esp_err_t nostr_keys_derive_callsign(const char *npub, char *callsign)
{
    if (!npub || !callsign || strlen(npub) < 9 || strncmp(npub, "npub1", 5) != 0) return ESP_ERR_INVALID_ARG;
    callsign[0] = 'X'; callsign[1] = '3';
    for (int i = 0; i < 4; i++) callsign[2 + i] = (char)toupper((unsigned char)npub[5 + i]);
    callsign[6] = 0;
    return ESP_OK;
}

/* ── The clock, such as it is ────────────────────────────────────────────
 *
 * No RTC, no NTP, GNSS off. What this station has is the owner: a packet
 * signed by an allow-listed key carries a ts: that the owner's clock set,
 * and that is trusted once -- the first such packet after boot sets the
 * clock, and millis() carries it from there. Until then xauth refuses
 * every command with 408, as 25.4 says a clockless station must.
 *
 * WHAT THIS DOES NOT DEFEND: a signed command recorded from the air and
 * replayed at this station after a reboot sets the clock to the moment it
 * was signed and then passes its own freshness check. The damage is
 * bounded -- it is the owner's own command, so at worst an install of an
 * image the owner once approved -- and a real clock closes it; until
 * then, within one boot, accepted timestamps must only move forward. */
static uint32_t s_clock_epoch, s_clock_set_ms, s_clock_last_accepted;

extern "C" uint32_t xauth_platform_now(void)
{
    return s_clock_epoch ? s_clock_epoch + (millis() - s_clock_set_ms) / 1000 : 0;
}

static uint32_t ts_to_epoch(const char *ts)
{
    int y, mo, d, h, mi, se;
    if (sscanf(ts, "%4d-%2d-%2d_%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + h * 3600 + mi * 60 + se);
}

/* No RTC here, so the clock is whatever the newest signed owner command says
 * (25.4). MONOTONIC FORWARD: each valid owner ts that is later than what we
 * hold advances the clock and never moves it back, so a replayed older
 * command cannot rewind us and then pass its own freshness check. Runs on
 * every owner command, not just the first, so the clock tracks the sender
 * instead of drifting from a single early sample. */
static void clock_learn(const xprs_t *p)
{
    char from[16] = "", ts[24] = "";
    if (!xprs_get_str(p, "f", from, sizeof from) || !xprs_get_str(p, "ts", ts, sizeof ts)) return;
    if (xprs_get(p, "sig", NULL) == NULL || xprs_get(p, "via", NULL) != NULL) return;
    uint8_t pub[32];
    if (!xauth_owner_key_of(from, pub) || !xprsid_verify(p, pub)) return;
    uint32_t t = ts_to_epoch(ts);
    if (!t || t <= xauth_platform_now()) return;   /* never backward */
    s_clock_epoch = t; s_clock_set_ms = millis(); s_clock_last_accepted = t;
    Serial.printf("clock: %s from %s\n", ts, from);
}

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
    if (!xprs_get_str(&p, "f", from, sizeof from) || !from[0] || strcmp(from, s_call) == 0) return;
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
/* A t:command addressed to us: the updater's, and never carried further
 * (25.4: a command is acted on where it arrives; a thousand image chunks
 * digipeated three hops would be the channel gone for an afternoon). */
static bool command_for_us(xb_t *b, const char *wire, int len)
{
    xprs_t p; char t[12] = "", d[16] = "";
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "t", t, sizeof t) || strcmp(t, "command") != 0) return false;
    if (!xprs_get_str(&p, "d", d, sizeof d) || strcmp(d, s_call) != 0) return false;
    clock_learn(&p);
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
    xb_digipeat(&s_lora, wire, len);
    if (s_ble_up) xb_offer(&s_ble, wire, len);
}

static void on_ble(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    heard("ble", wire, len, rssi);
    hears_touch(wire, len);
    if (command_for_us(&s_ble, wire, len)) return;
    xb_offer(&s_lora, wire, len);
}

/* ── What we say ─────────────────────────────────────────────────────────
 *
 * The observation beacon of XPRS 10.6.1 with the hears: list of 10.6.3, and
 * signed (9.1) so a receiver can hold the callsign to the key it learned
 * from our t:identity. The signature is 65 bytes; an observation with eight
 * neighbours is still well inside a wire. */
static int beacon(char *out, int cap, const char *link, int peers)
{
    int n = snprintf(out, (size_t)cap, "t:observation f:%s link:%s peers:%d", s_call, link, peers);
    if (n <= 0 || n >= cap) return n;
    n += hears_render(out + n, cap - n - (5 + XPRSSIG_B85_LEN));
    return sign_wire(out, n, cap);
}

static int lora_beacon(char *out, int cap) { return beacon(out, cap, "lora", xb_peer_count(&s_lora, 600)); }
static int ble_beacon(char *out, int cap)  { return beacon(out, cap, "ble",  xb_peer_count(&s_ble, 600)); }

/* t:identity (9.3): the key behind the callsign, so a neighbour can verify
 * every signature after it. Once at 30 s so a station that just came up is
 * findable, then every 30 minutes (18.1) -- a binding that never changes is
 * not worth a fresh archive record a minute. Same cadence as xprs_app.c. */
static void air_identity(void)
{
    if (!s_have_key) return;
    char wire[XPRS_MAX_WIRE + 1], tf[32];
    time_field(tf, sizeof tf);
    int n = snprintf(wire, sizeof wire, "t:identity f:%s %s k:%s", s_call, tf, s_npub);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = sign_wire(wire, n, (int)sizeof wire);
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
    tn_gatt_send((const uint8_t *)wire, len);
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
        xprs_get_str(&p, "d", dst, sizeof dst) && strcmp(dst, s_call) == 0) {
        clock_learn(&p);
        xfw_gatt_rx((const char *)d, n, gatt_reply);
        blob_maybe_start();   /* a cmd:update just opened the session -> pull it fast */
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
    tn_adv_cfg_t adv = {
        .handle = 0, .props = 0, .itvl_min = 0x100, .itvl_max = 0x100,
        .chan_map = 0x07, .own_addr_type = 0x01, .tx_power = 127,
        .primary_phy = TN_PHY_1M, .secondary_phy = TN_PHY_1M, .sid = 0,
    };
    tn_adv_configure(&adv);
    s_ble_err = p1_scan_on();
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
    case 'k':                                    /* who we are, publicly */
        Serial.printf("call=%s npub=%s boots=%lu\n", s_call, s_have_key ? s_npub : "-",
                      (unsigned long)s_boot_epoch);
        break;
    case 'K': {                                  /* the private half -- a backup, on request only */
        char nsec[80] = "-";
        if (s_have_key) bech32_encode("nsec", s_priv, sizeof s_priv, nsec, sizeof nsec);
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
        key_import(line);
        break; }
    case 'i': air_identity(); break;             /* say who we are, now */
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
        if (strncmp(line, "fg ", 3) == 0) cfg_console(line + 3);
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
        Serial.printf("call=%s fw=%s%s clock=%lu key=%d lora=%d ble=%d(err %d) link=%s peer=%s gatt rx=%lu tx=%lu\n",
                      s_call, xfw_version(), xfw_probation() ? "(probation)" : "",
                      (unsigned long)xauth_platform_now(),
                      (int)s_have_key, (int)s_radio_up, (int)s_ble_up, s_ble_err,
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
    keys_init();
    cfg_load();
    static const xfw_cfg_t k_xfw = {
        .board = "sensecap-p1-pro", .call = s_call, .sign = sign_wire, .flush = bearers_flush,
    };
    xfw_init(&k_xfw, s_boot_epoch);
    ble_begin();

    /* The watchdog: a station that hangs on a pole reboots itself, and a
     * new image that hangs three times is put back (update.cpp). 60 s,
     * fed from the station loop and from inside the copier. */
    NRF_WDT->CONFIG = WDT_CONFIG_SLEEP_Msk;
    NRF_WDT->CRV = 60 * 32768;
    NRF_WDT->RREN = 1;
    NRF_WDT->TASKS_START = 1;

    Serial.printf("\nXPRS station %s -- SenseCAP Solar Node P1-Pro (boot %lu)\n",
                  s_call, (unsigned long)s_boot_epoch);
    Serial.println("headless: LoRa + BLE5, signing. No WiFi on this chip.");

    randomSeed(NRF_FICR->DEVICEID[0]);

    s_radio_up = lora_begin();

    xb_init(&s_lora, &k_lora_ops, s_call);
    xb_set_rx_cb(&s_lora, on_lora);
    xb_set_beacon(&s_lora, lora_beacon, BEACON_EVERY_SEC, BEACON_JITTER_SEC);
    xb_set_pace(&s_lora, LORA_PACE_MS);
    /* The rolling-hour ledger, from the same modem constants the radio was
     * given ten lines up, so the charge cannot drift from the wire. Band
     * g3: 360 s of airtime an hour, 6 s of it kept for sos. */
    static xb_duty_t s_lora_duty;
    static const xb_lora_air_t k_lora_air = {
        .bw_hz = (uint32_t)(LORA_BW_KHZ * 1000.0), .sf = LORA_SF,
        .cr = LORA_CR - 4, .preamble = LORA_PREAMBLE, .crc = true,
        .implicit_header = false,
    };
    struct air_fn { static uint32_t ms(int len, void *ctx) {
        (void)ctx; return xb_lora_airtime_ms(&k_lora_air, len); } };
    xb_set_duty(&s_lora, &s_lora_duty, air_fn::ms, NULL,
                360000u, 6000u, 0u);
    xb_set_driver(s_radio_up);

    xb_init(&s_ble, &k_ble_ops, s_call);
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
                      (unsigned long)(now / 1000), s_call, xfw_version(), xfw_probation() ? "?" : "",
                      (unsigned long)rx, (unsigned long)tx, (unsigned long)cancelled,
                      xb_peer_count(&s_lora, 600),
                      (unsigned long)brx, (unsigned long)btx, xb_peer_count(&s_ble, 600),
                      (unsigned long)s_heard, (int)s_radio_up);
        if (sof) Serial.printf(" | update %lu/%lu", (unsigned long)sgot, (unsigned long)sof);
        Serial.println();
    }

    delay(5);
}
