/**
 * @file xprslora.h
 * @brief XPRS over LoRa, on Meshtastic's channel.
 *
 * Since 2026-09 the radio runs Meshtastic's LongFast modulation (SF11,
 * 250 kHz, CR 4/5, preamble 16, sync word 0x2B) on Meshtastic's frequency
 * slot for the region, so one radio hears both networks (docs/meshtastic.md).
 *
 * The wire is still the wire, inside a wrapper: every XPRS packet rides as
 * the payload of a Meshtastic Data frame on XPRS's own clear channel and
 * private portnum (xprs_meshtastic/mt.h), one frame up to 233 bytes, two
 * above that. The xb_* half never sees the wrapper; what it hands this
 * file and what this file hands back are plain XPRS wires, as on every
 * other bearer.
 *
 * Every frame that is NOT XPRS goes to xprs_meshtastic's repeater and
 * bridge (mt_mesh), which relays Meshtastic traffic under Meshtastic's own
 * flood rules and translates LongFast text to and from XPRS messages.
 *
 * ON AIRTIME. A full 255-byte frame at SF11/250 kHz is 2.1 s on the air,
 * five and a half times what the fleet's SF7 cost. The band is shared with
 * a duty-cycle obligation -- 10% in band g3 (869.4-869.65), which is the
 * one EU_868 slot Meshtastic has too -- and the duty ledger (xb_set_duty)
 * charges every transmission, XPRS and Meshtastic alike (xb_spend), against
 * one rolling hour. Before a transmission the radio listens: a packet
 * already arriving, or channel activity detection, holds it back.
 */
#ifndef XPRS_BEARER_LORA_H
#define XPRS_BEARER_LORA_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "xprsbearer.h"
#include "mt_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The radio's wiring and tuning. A board that has no SX1262 simply never
 *  calls xprslora_start. */
typedef struct {
    int sck_pin, mosi_pin, miso_pin;
    int cs_pin, rst_pin, busy_pin, dio1_pin;
    uint32_t freq_hz;        /* 0 = the region preset's channel */
    int8_t tx_power_dbm;     /* 0 = a polite 14 dBm */
    bool use_tcxo;           /* module has a TCXO on DIO3 (the T-Deck does) */
    bool use_dio2_rf_switch; /* DIO2 drives the RF switch (the T-Deck too) */
    const char *region;      /* lora_region; NULL = entry 0 (`eu`) */
} xprslora_cfg_t;

/**
 * Where in the spectrum this station is allowed to be, and what it owes.
 *
 * The frequency is Meshtastic's: its slot rule over the region's band for
 * the channel name LongFast (mt_slot_freq_hz), which is 869.525 MHz in
 * Europe. `eu-g1` (868.2 MHz) went with the move: Meshtastic has no channel
 * there, and a station alone on it would hear nobody.
 *
 * The US and AU rows used to cap one transmission at 400 ms (dwell). No
 * SF11 frame fits that, and Meshtastic, whose channel this now is, applies
 * no such cap on those bands: whether it binds a given installation is the
 * operator's question, and `lora_duty_ms` / a dwell can still be set.
 */
typedef struct {
    const char *name;        /* what lora_region selects */
    uint32_t freq_hz;
    uint32_t duty_ms;        /* transmit ms per rolling hour; 0 = none */
    uint32_t reserve_ms;     /* of that, sos/warning/urgent only */
    uint32_t dwell_ms;       /* longest single transmission; 0 = any */
    int8_t   max_dbm;        /* the region's e.r.p. ceiling, for the log */
} xprslora_region_t;

/** The table `lora_region` picks from; entry 0 (`eu`) is the default. */
const xprslora_region_t *xprslora_regions(int *count);

/** Bring the radio up and join the bearer fleet. Needs another bearer's task
 *  already pumping xb_tick_all() -- the LAN bearer owns that job. */
esp_err_t xprslora_start(const char *callsign, const xprslora_cfg_t *cfg);

typedef void (*xprslora_rx_cb_t)(const char *wire, int len, int rssi);
void xprslora_set_rx_cb(xprslora_rx_cb_t cb);

/** Air one packet of our own, now. */
bool xprslora_send(const char *wire, int len);

/** Offer a packet heard elsewhere for re-airing here (the bearer decides). */
void xprslora_offer(const char *wire, int len);

/** Re-air on THIS bearer what was heard on it -- a digipeater (13.1). Unlike
 *  xprslora_offer(), having heard the packet here is the reason to repeat it. */
void xprslora_digipeat(const char *wire, int len);

/* Air a packet we already aired, verbatim: the idle-channel echo
 * (xb_echo). */
void xprslora_echo(const char *wire, int len);

/* Milliseconds since anything was heard or aired here. */
uint32_t xprslora_idle_ms(uint32_t now_ms);

bool xprslora_is_active(void);

/**
 * Silence one packet owes this radio, in milliseconds (XPRS.md section 31.1:
 * "LoRa on ISM | a legal duty cycle, often 1 percent -- at SF9 a single packet
 * owes several seconds of silence").
 *
 * Re-airs offered while the debt stands WAIT; they are not dropped. Our own
 * transmissions are charged but never blocked.
 *
 * The default is XPRSLORA_PACE_DEFAULT_MS. It is deliberately not a legal
 * duty-cycle calculation: the real figure depends on band, spreading factor
 * and region -- at SF7 a 250-byte packet is ~400 ms, which under a 1 percent
 * duty cycle owes about 40 seconds -- and that number is the operator's to
 * set, not this library's to guess. 0 disables pacing.
 */
void xprslora_set_pace(uint32_t per_packet_ms);

/** Milliseconds until the radio may transmit again; 0 when free now. */
uint32_t xprslora_owed_ms(void);

/**
 * Ten seconds between our own XPRS transmissions. It was six at SF7, where
 * a packet was 0.4 s; at SF11 a typical 150-byte packet is 1.3 s, so the
 * same spacing would have put this station on the one shared channel a
 * fifth of the time in a burst. Ten keeps a burst near a tenth, which is
 * also what band g3 allows over the hour.
 *
 * The pace is a collision spacer; the ledger below is the accountant. Both
 * apply, and they are deliberately not one number.
 */
#define XPRSLORA_PACE_DEFAULT_MS 10000u

/** This radio's airtime for [len] bytes at the SF/BW/CR xprslora_start()
 *  set -- the number the duty ledger charges. 0 before start. */
uint32_t xprslora_airtime_ms(int len);

/** Re-point the ledger, e.g. from config. 0 budget with 0 dwell = off. */
void xprslora_set_duty(uint32_t budget_ms, uint32_t reserve_ms,
                       uint32_t dwell_ms);

/** The ledger's report; zeroed when the radio is down or unmetered. */
void xprslora_duty(xb_duty_report_t *out);

/** The region the radio was started with (never NULL after start). */
const xprslora_region_t *xprslora_region(void);

/** RX/TX/cancelled/dupes counters, any may be NULL. */
void xprslora_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                    uint32_t *dupes);

/* ── Meshtastic: the repeater and the bridge ─────────────────────────── */

/** What the station provides the bridge. Called on the bearer task, with
 *  the bridge's lock held (it is recursive: calling back into
 *  xprslora_mt_offer from here is safe). */
typedef struct {
    /* A translated wire for every bearer but LoRa and for the local UI;
     * [sign] = sign it as this station first (a gateway receipt). */
    void (*deliver)(const char *wire, int len, bool sign);
    /* The time field, "ts:..." (seconds zeroed when [to_minute]) or
     * "epoch:..."; returns its length. */
    int  (*stamp)(char *out, int cap, bool to_minute);
    /* A verified nick for an XPRS callsign. */
    bool (*nick_of)(const char *call, char *out, int cap);
} xprslora_mt_hooks_t;

/**
 * Start the repeater and the bridge on the running radio. Allocates the
 * bridge's state once (about 9 KB, PSRAM where there is some); without it
 * the radio still carries XPRS, and says so in the log.
 */
esp_err_t xprslora_mt_start(const xprslora_mt_hooks_t *hooks,
                            const mt_mesh_cfg_t *cfg, const char *nick);

/** Every XPRS packet this station hears (MT_XPRS_HEARD) or originates
 *  (MT_XPRS_OWN), on any bearer and any task. Cheap: a parse and maybe a
 *  queued frame. */
void xprslora_mt_offer(const char *wire, int len, int origin);

/** The bridge's counters; false when it is not running. */
bool xprslora_mt_stats(mt_mesh_stats_t *out);

/** Meshtastic nodes heard, freshest first is not promised; [i] 0..n-1 into
 *  [out] (a copy). Returns how many there are. */
int xprslora_mt_node(int i, mt_node_t *out);

/** The station's nick changed (announced at the next NodeInfo). */
void xprslora_mt_set_nick(const char *nick);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_BEARER_LORA_H */
