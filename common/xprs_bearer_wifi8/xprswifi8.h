/**
 * @file xprswifi8.h
 * @brief XPRS over XPRS-WiFi (the `xwifi` library) — the long-range 2.4 GHz
 *        bearer, as a drop-in alternative to ESP-NOW on the same radio.
 *
 * This bearer carries the same 250-byte packet (`docs/XPRS.md` §4) that
 * `xprsnow` carries, and presents the same public surface to `xprs_app`, so the
 * station program does not care which of the two is behind it. What differs is
 * the radio and the time it takes.
 *
 * ── Why this is not ESP-NOW ─────────────────────────────────────────────────
 *
 * ESP-NOW is an 802.11 action frame: one 250-byte packet is one frame, on air
 * in under a millisecond, and `air()` returns once the driver has it. XPRS-WiFi
 * is a chirp-spread-spectrum waveform (`xwifi`, XPRS-WiFi.md): the SAME packet
 * is a transmission of many blocks lasting **seconds** (1.5 s, 3 s or 12 s,
 * §6.4), bought for about 24 dB of processing gain — the range ESP-NOW does not
 * have. So `air()` here QUEUES a packet and returns; the bearer's own task plays
 * it out over the next seconds and `xprswifi8_settle()` is how a caller waits
 * for "it has actually gone".
 *
 * ── The one radio, shared three ways ────────────────────────────────────────
 *
 * The chip has a single 2.4 GHz radio. `xwifi` does not speak 802.11 at all: it
 * drives the PHY directly as a raw CSS transmitter and sampler (see the port's
 * `xw_esp32c3_radio_start`). It therefore cannot be on the air at the same
 * instant as the normal 802.11 stack that the walk-up hotspot (SoftAP), the
 * local-WiFi uplink (STA) and ESP-NOW all use.
 *
 * The three coexist by TIME-SLICING. Most of the time the radio is the ordinary
 * AP+STA: the hotspot serves pages, the STA carries internet. Periodically the
 * scheduler borrows the radio for one XPRS-WiFi SLOT — suspends 802.11
 * (`xw_*_radio_suspend`), does one transmit or one receive window, then resumes
 * 802.11 (`xw_*_radio_resume`). During a slot the AP stops beaconing and the STA
 * is off-channel, so both hiccup for the slot's length: short, infrequent slots
 * keep that invisible, a 12 s transmission does not. `xprswifi8_cfg_t` sets the
 * cadence and the ceiling, and a transmission longer than one slot is played
 * across several, a block or two at a time (blocks are 7.84 ms, §6.4, and may be
 * split at block boundaries without loss — receivers reassemble from any pieces).
 *
 * A board that does NOT need the hotspot or the uplink can give the radio to
 * XPRS-WiFi full time (`slot_ms == 0, period_ms == 0`): then there is no 802.11
 * and no hiccup, and this is the field-test firmware's arrangement.
 *
 * ── Reassembly lives in xwifi; relay lives in xprs_bearer ────────────────────
 *
 * `xwifi`'s station reassembles a packet from its frames and hands up only the
 * COMPLETE 250-byte packet — exactly what ESP-NOW's receive callback hands up.
 * Everything above that (the §5 identifier, the §13 relay, the §13.2.1 cancel,
 * the beacon cadence, the duty ledger) is `xprs_bearer`, shared with every other
 * bearer, and is not duplicated here. `xwifi`'s own message layer (`xw_msg`,
 * receipts) is NOT used: receipts are the station program's job across bearers.
 *
 * ── Only where there is a port ───────────────────────────────────────────────
 *
 * `xwifi` has ESP32-C3 and ESP32-S3 ports and no others. A plain ESP32 or an
 * nRF52 has no XPRS-WiFi radio and must not link this bearer; `xprsnow` remains
 * their WiFi bearer. CONFIG_XPRS_WIFI8 gates the whole thing at build time.
 */

#ifndef XPRS_BEARER_WIFI8_H
#define XPRS_BEARER_WIFI8_H

#include <stdint.h>
#include <stdbool.h>

#ifdef XPRSWIFI8_HOST_TEST
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Longest XPRS packet, and the most one transmission carries. */
#define XPRSWIFI8_WIRE_MAX   250

/**
 * Transmissions waiting for the bearer task to play them.
 *
 * A transmission takes seconds, so this is not the per-frame queue ESP-NOW
 * needs; it is the handful of whole packets that may be outbound at once (our
 * own sends plus relays whose jitter has expired). Four matches
 * `XB_QUEUE_MAX`; a fifth caller waits or is refused rather than growing the
 * heap on a board that boots with kilobytes free.
 */
#define XPRSWIFI8_TX_QUEUE   4

/** Packets the receive side may hold before the bearer task reads them. A
 *  window surfaces at most a couple of complete packets, so this is small. */
#define XPRSWIFI8_RX_QUEUE   4

/**
 * How the one radio is shared with 802.11 (the hotspot and the uplink).
 *
 * `slot_ms` is the longest single stretch XPRS-WiFi may hold the radio before
 * giving it back for `gap_ms` of 802.11. `period_ms` is how often a slot may
 * start when there is nothing to send (the receive duty: smaller hears more,
 * and starves the hotspot more). `channel` is the XPRS-WiFi channel (1..3,
 * XPRS-WiFi.md §4; the centres are not the 802.11 channel the STA is on).
 *
 * slot_ms == 0 gives XPRS-WiFi the radio full time: no 802.11, no hiccup, the
 * field-test arrangement. The default shares it: 120 ms slots, 380 ms of
 * 802.11 between them (a 24 % XPRS-WiFi duty), receiving four times a second.
 */
typedef struct {
    uint16_t slot_ms;
    uint16_t gap_ms;
    uint16_t period_ms;
    uint8_t  channel;
    uint8_t  looks;        /* segments per window, 1..XW_SEGMENTS (sensitivity) */
    uint16_t dac_amplitude;/* transmit level, per the port; not calibrated power */
} xprswifi8_cfg_t;

#define XPRSWIFI8_CFG_DEFAULT \
    { .slot_ms = 120, .gap_ms = 380, .period_ms = 250, .channel = 2, \
      .looks = 2, .dac_amplitude = 200 }

/** @brief One packet heard on XPRS-WiFi.
 *  @param snr_db Es/N0 of the transmission in dB, as the demodulator measured it
 *               (XPRS-WiFi has no MAC address, so there is no peer MAC). */
typedef void (*xprswifi8_rx_cb_t)(const char *wire, int len, int snr_db);

/** Told the §5 identifier of every valid packet, duplicates included. */
typedef void (*xprswifi8_heard_cb_t)(const char *id, const char *wire, int len);

/** Build this station's periodic beacon. Return its length, or 0. */
typedef int (*xprswifi8_beacon_cb_t)(char *out, int cap);

/**
 * @brief Bring the bearer up. The radio port must be started first
 *        (`xw_<chip>_radio_start`), and, for time-slicing, 802.11 (AP+STA)
 *        already running so there is something to hand the radio back to.
 *
 * @param callsign this station, used for `via:` when relaying. Copied.
 */
esp_err_t xprswifi8_start(const char *callsign, const xprswifi8_cfg_t *cfg);
void      xprswifi8_stop(void);
bool      xprswifi8_is_active(void);

void xprswifi8_set_rx_cb(xprswifi8_rx_cb_t cb);
void xprswifi8_set_heard_cb(xprswifi8_heard_cb_t cb);
void xprswifi8_set_beacon(xprswifi8_beacon_cb_t cb, uint32_t interval_sec,
                          uint32_t first_delay_sec);

/** Air one packet of OUR OWN, with no `via:`. Returns false if the queue is
 *  full or the length is wrong. The packet goes out over the next slots; use
 *  xprswifi8_settle() to wait for it. */
bool xprswifi8_send(const char *wire, int len);

/** Wait until every queued transmission has finished playing. False on timeout
 *  (the bearer is genuinely stuck, not merely slow — seconds are normal here). */
bool xprswifi8_settle(uint32_t timeout_ms);

/** Log every packet drained, with SNR and the first bytes. Off by default. */
void xprswifi8_set_trace(bool on);

/** Offer a packet heard on another bearer for re-airing here (§13.2.1). */
void xprswifi8_offer(const char *wire, int len);
/** Re-air on THIS bearer what was heard on it — a digipeater (§13.1). */
void xprswifi8_digipeat(const char *wire, int len);
/** Air a packet we already aired, verbatim: the idle-channel echo (xb_echo). */
void xprswifi8_echo(const char *wire, int len);

/** Milliseconds since anything was heard or aired here. */
uint32_t xprswifi8_idle_ms(uint32_t now_ms);

/** Stations heard within [max_age_sec]. */
int xprswifi8_peer_count(uint32_t max_age_sec);

/** The XPRS-WiFi channel this bearer transmits on (1..3). */
uint8_t xprswifi8_channel(void);

/** @param dropped packets the receive path had nowhere to put. */
void xprswifi8_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                     uint32_t *dropped);

/** @param issued transmissions started; @param done finished; @param failed
 *  could not start (the port refused, or a slot never came). */
void xprswifi8_tx_stats(uint32_t *issued, uint32_t *done, uint32_t *failed);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_BEARER_WIFI8_H */
