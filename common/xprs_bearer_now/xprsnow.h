/**
 * @file xprsnow.h
 * @brief XPRS over ESP-NOW — every ESP32 already has this radio.
 *
 * ESP-NOW is Espressif's connectionless mode: 802.11 action frames, no
 * association, no AP, no DHCP. A frame carries **250 bytes**
 * (`ESP_NOW_MAX_DATA_LEN`), which is exactly the longest XPRS packet
 * (`docs/XPRS.md` §4) — so one packet is one frame, verbatim, and nothing is
 * ever fragmented. `link:espnow` is an assigned bearer word (§10.6.1) and
 * `docs/espnow.md` is this bearer's page.
 *
 * ── Why this is not a hotspot ───────────────────────────────────────────────
 *
 * Nobody connects. A SoftAP has a client ceiling and shares one channel's
 * bandwidth between everyone associated to it; ESP-NOW broadcast has neither
 * problem, because there is no association to run out of. The peer table needs
 * exactly ONE entry — the broadcast address — so `ESP_NOW_MAX_TOTAL_PEER_NUM`
 * (20) never binds however many stations are listening.
 *
 * Promiscuous mode is not used and is not needed: broadcast frames reach every
 * ESP-NOW device on the channel through the ordinary receive path, and the RSSI
 * that sniffing would have been for arrives with them (`rx_ctrl`).
 *
 * ── The channel and PHY: the station's profile ─────────────────────────────
 *
 * Default profile: ESP-NOW is on the WiFi channel at 1 Mbps and never leaves
 * it (a station with no router uses `espnow_ch`). Long-range profile
 * (docs/profiles.md): ESP-NOW has a home channel (13 by default,
 * `espnow_ch`; 11 where the WiFi country has no 13) and sends on Espressif's
 * long-range PHY, 250 kbps. A station on no router sits on the home channel. A station
 * on a router keeps the router's channel for WiFi and visits the home
 * channel for `espnow_win` ms (80) at the start of every second of the wall
 * clock; every station that knows the time sends inside that window, and one
 * without a clock learns it from the frames it hears. docs/espnow.md.
 *
 * ── Where the work happens ──────────────────────────────────────────────────
 *
 * ESP-NOW delivers into a callback that runs **in the WiFi task, on core 0** —
 * beside the radios, on the processor `docs/esp32.md` spends its length
 * defending. Deriving a §5 identifier is a SHA-256 and must not happen there.
 * So the callback copies the frame into a queue and returns; everything else
 * runs on the bearer task through `xprs_bearer`'s drain hook.
 */

#ifndef XPRS_BEARER_NOW_H
#define XPRS_BEARER_NOW_H

#include <stdint.h>
#include <stdbool.h>

#ifdef XPRSNOW_HOST_TEST
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The profile's ESP-NOW (docs/profiles.md, docs/espnow.md). Call before
 * wifi_up().
 * long_range false (the default profile): ESP-NOW on the WiFi channel at
 *   1 Mbps; [home_channel] is used only by a station with no router.
 * long_range true: the home channel (1..14) is ESP-NOW's own, the long-range
 *   PHY, and a station on a router visits it [window_ms] each second.
 */
void    xprsnow_configure(uint8_t home_channel, bool long_range, uint32_t window_ms);
/** Between esp_wifi_init() and esp_wifi_start(); returns the home channel. */
uint8_t xprsnow_wifi_prepare(const char *country);
uint8_t xprsnow_home_channel(void);
bool    xprsnow_long_range(void);
/** Visits to the home channel, refused switches, frames sent in a window,
 *  frames dropped from a full window queue. */
void    xprsnow_window_stats(uint32_t *visits, uint32_t *switch_fail,
                             uint32_t *sent, uint32_t *dropped);
/**
 * Somebody is using this station over WiFi (a Reticulum link is open, the
 * web server took a connection): no visit to the home channel for the next
 * [ms], counted from now; the longest hold asked for stands. Call it again
 * while the use lasts. Safe from any task, before start too.
 */
void    xprsnow_hold(uint32_t ms);
/** Visits put off because WiFi was in use, and the frames addressed to this
 *  station in the second before the last window and in the busiest second
 *  since the last call. */
void    xprsnow_window_busy(uint32_t *skipped, uint32_t *last, uint32_t *peak);

/** Longest XPRS packet, and exactly one ESP-NOW frame. */
#define XPRSNOW_WIRE_MAX   250

/**
 * Frames the receive callback may hold before the bearer task drains them.
 *
 * Four was not enough. The drain shares a task with the LAN bearer's 100 ms
 * socket timeout, a SHA-256 per packet, signature checks, and SD writes behind
 * a lock the index writer holds across two fsyncs — a stall of several hundred
 * milliseconds is ordinary, and everything arriving during one was lost while
 * nothing on the dongle printed the counter.
 *
 * Sixteen was too many, and expensively so. This queue is heap, 264 bytes an
 * entry, on a board that finishes booting with about eight kilobytes free — so
 * the four kilobytes spent here came out of the allocation esp_now_send() needs,
 * and the bearer answered ESP_ERR_ESPNOW_NO_MEM to every transmission while the
 * drop counter it was widened to protect sat at zero throughout. Eight is twice
 * the original and costs half as much.
 */
#define XPRSNOW_RX_QUEUE   8

/**
 * @brief One packet heard on ESP-NOW.
 * @param mac  the sender's address, 6 bytes; valid only during the call
 * @param rssi dBm, as the radio measured it
 */
typedef void (*xprsnow_rx_cb_t)(const char *wire, int len,
                                const uint8_t mac[6], int rssi);

/** Told the §5 identifier of every valid packet, duplicates included. */
typedef void (*xprsnow_heard_cb_t)(const char *id, const char *wire, int len);

/** Build this station's periodic beacon. Return its length, or 0. */
typedef int (*xprsnow_beacon_cb_t)(char *out, int cap);

/**
 * @brief Bring the bearer up. WiFi must already be started (`esp_wifi_start`);
 *        it need not be associated.
 *
 * Turns modem sleep OFF: a station that sleeps misses ESP-NOW frames, which
 * Espressif's own example warns about. That is a coexistence decision as much
 * as a power one on a board that also runs Bluetooth.
 *
 * @param callsign this station, used for `via:` when relaying. Copied.
 */
esp_err_t xprsnow_start(const char *callsign);
void      xprsnow_stop(void);
bool      xprsnow_is_active(void);

void xprsnow_set_rx_cb(xprsnow_rx_cb_t cb);
void xprsnow_set_heard_cb(xprsnow_heard_cb_t cb);
void xprsnow_set_beacon(xprsnow_beacon_cb_t cb, uint32_t interval_sec,
                        uint32_t first_delay_sec);

/** Air one packet of OUR OWN, now, with no `via:` — it has taken no hops. */
bool xprsnow_send(const char *wire, int len);

/**
 * @brief Wait until every frame handed to the driver has actually left.
 *
 * `esp_now_send` is asynchronous: it returns once the frame is queued, not once
 * the radio has finished with it. Everything that needed to know "has it gone
 * yet" used to guess with a 120 ms delay, which is either too long or — the
 * expensive case — too short, and the §23.7 move retuned the radio out from
 * under an acceptance that had not been transmitted.
 *
 * The send callback makes it a fact instead. Returns false on timeout, which
 * means the driver is genuinely stuck rather than merely slow.
 */
bool xprsnow_settle(uint32_t timeout_ms);

/**
 * @brief Log every frame this bearer drains, with RSSI and the first bytes.
 *
 * For the windows where the question is "did the radio hear it at all" and no
 * counter can answer, because the packet may be lost before anything counts it:
 * the callback's cheap byte test, a full queue, a parse that fails. On for the
 * duration of a rendezvous, off the rest of the time — this is a per-packet log
 * line and it is not free.
 */
void xprsnow_set_trace(bool on);

/** Offer a packet heard on another bearer for re-airing here (§13.2.1). */
void xprsnow_offer(const char *wire, int len);

/** Re-air on THIS bearer what was heard on it -- a digipeater (13.1). Unlike
 *  xprsnow_offer(), having heard the packet here is the reason to repeat it. */
void xprsnow_digipeat(const char *wire, int len);

/* Air a packet we already aired, verbatim: the idle-channel echo
 * (xb_echo). */
void xprsnow_echo(const char *wire, int len);

/* Milliseconds since anything was heard or aired here. */
uint32_t xprsnow_idle_ms(uint32_t now_ms);

/** Stations heard within [max_age_sec]. */
int xprsnow_peer_count(uint32_t max_age_sec);

/** The channel this bearer is actually on — the STA's, whatever that is. */
uint8_t xprsnow_channel(void);

/** @param dropped frames the receive callback had nowhere to put. */
void xprsnow_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                   uint32_t *dropped);

/**
 * @param issued frames handed to the driver
 * @param done   frames the driver reported finished with, either way
 * @param failed of those, the ones it could not transmit
 */
void xprsnow_tx_stats(uint32_t *issued, uint32_t *done, uint32_t *failed);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_BEARER_NOW_H */
