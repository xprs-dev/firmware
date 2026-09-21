/**
 * @file xprslora.h
 * @brief XPRS over LoRa, in one of the station's LoRa modes.
 *
 * A LoRa receiver hears only the modulation and sync word it is set to, so
 * which LoRa network a station shares a channel with is a setting, chosen at
 * start and fixed until the next (`lora_mode`, docs/lora.md "One radio,
 * three networks"):
 *
 *   xprs        XPRS's own channel, as the fleet ran before 2026-09-19:
 *               SF7 (SF9 with the `far` profile), 125 kHz, CR 4/5, preamble
 *               8, the chip's default sync word 0x12, 869.5 MHz in Europe.
 *               The frame IS the XPRS wire.
 *   meshtastic  Meshtastic's LongFast (SF11, 250 kHz, CR 4/5, preamble 16,
 *               sync word 0x2B) on Meshtastic's frequency slot. Every XPRS
 *               packet rides as the payload of a Meshtastic Data frame on
 *               XPRS's own clear channel and private portnum (xprs_meshtastic
 *               /mt.h), one frame up to 233 bytes, two above that; every
 *               other frame goes to the Meshtastic repeater and bridge
 *               (mt_mesh). The default.
 *   meshcore    reserved: named everywhere, not in this firmware yet.
 *
 * The xb_* half never sees the difference: what it hands this file and
 * what this file hands back are plain XPRS wires, as on every other bearer.
 * In every mode the radio listens before it talks (a header already
 * arriving, then channel activity detection), transmits without blocking,
 * and charges every transmission against one duty ledger (xb_set_duty).
 */
#ifndef XPRS_BEARER_LORA_H
#define XPRS_BEARER_LORA_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "xprsbearer.h"
#include "mc_mesh.h"
#include "mt_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The LoRa modes, in the order `lora_mode` names them. */
typedef enum {
    XPRSLORA_MODE_XPRS = 0,
    XPRSLORA_MODE_MESHTASTIC,
    XPRSLORA_MODE_MESHCORE,
    XPRSLORA_MODE_COUNT
} xprslora_mode_t;

/** The mode a freshly flashed station runs. */
#define XPRSLORA_MODE_DEFAULT XPRSLORA_MODE_MESHTASTIC

/** "xprs", "meshtastic", "meshcore". */
const char *xprslora_mode_name(xprslora_mode_t mode);

/** The mode a word names; false for a word that names none. */
bool xprslora_mode_parse(const char *word, xprslora_mode_t *out);

/** Whether this firmware can run [mode] (meshcore: not yet). */
bool xprslora_mode_available(xprslora_mode_t mode);

/** The radio's wiring and tuning. A board that has no SX1262 simply never
 *  calls xprslora_start. */
typedef struct {
    int sck_pin, mosi_pin, miso_pin;
    int cs_pin, rst_pin, busy_pin, dio1_pin;
    uint32_t freq_hz;        /* 0 = the region preset's channel */
    int8_t tx_power_dbm;     /* 0 = a polite 14 dBm */
    bool use_tcxo;           /* module has a TCXO on DIO3 (the T-Deck does) */
    bool use_dio2_rf_switch; /* DIO2 drives the RF switch (the T-Deck too) */
    const char *region;      /* lora_region; NULL = the mode's entry 0 */
    xprslora_mode_t mode;    /* lora_mode; must be available */
    bool far;                /* xprs mode's `far` profile: SF9 */
    /* The modulation, when the operator has to match a channel this
     * firmware does not know the preset for. 0 = the mode's own.
     *
     * MeshCore's presets are regional AND they move: 869.618 MHz at
     * SF8/62.5 kHz here, SF7 on the same bandwidth in the US, and whole
     * regions changed during 2025. A station that cannot follow its
     * neighbours without a new firmware is a station nobody can join, so
     * `lora_sf` and `lora_bw_khz` exist. They are the operator's own risk:
     * a radio set to another modulation is deaf to everyone on the
     * mode's default. */
    uint8_t  sf;             /* 7..12 */
    uint16_t bw_khz;         /* 62, 125, 250, 500 (62 means 62.5) */
} xprslora_cfg_t;

/**
 * Where in the spectrum this station is allowed to be, and what it owes.
 * Each mode has its own table, because each network has its own channels:
 *
 *   xprs        `eu` 869.5 MHz (ERC 70-03 band g3: 10%, 500 mW e.r.p.),
 *               `eu-g1` 868.2 MHz (band g1: 1%, 25 mW), `us` 903.9 MHz and
 *               `au` 917.0 MHz with a 400 ms dwell per transmission.
 *   meshtastic  Meshtastic's LongFast slot for the region (mt_slot_freq_hz):
 *               `eu` 869.525, `us` 906.875, `au` 919.875 MHz. No SF11 frame
 *               fits a 400 ms dwell and Meshtastic applies none, so the
 *               900 MHz rows carry none; `lora_duty_ms` and a dwell can
 *               still be set by the operator.
 */
typedef struct {
    const char *name;        /* what lora_region selects */
    uint32_t freq_hz;
    uint32_t duty_ms;        /* transmit ms per rolling hour; 0 = none */
    uint32_t reserve_ms;     /* of that, sos/warning/urgent only */
    uint32_t dwell_ms;       /* longest single transmission; 0 = any */
    int8_t   max_dbm;        /* the region's e.r.p. ceiling, for the log */
} xprslora_region_t;

/** The table `lora_region` picks from in [mode]; entry 0 (`eu`) is the
 *  default. NULL and 0 for a mode this firmware does not have. */
const xprslora_region_t *xprslora_regions(xprslora_mode_t mode, int *count);

/** The mode the radio is in (the default before start). */
xprslora_mode_t xprslora_mode(void);

/**
 * Change mode on the running radio: retune, rebuild the airtime table and
 * the ledger, and go back to listening, with no restart. What was in flight
 * on the old channel is lost and the stations still on it can no longer
 * hear this one, so it is an operator's decision, never an automatic one.
 * The bridge's state is not freed: a switch back finds it, and a large
 * block freed and re-claimed is how a small heap fragments.
 *
 * ESP_ERR_NOT_SUPPORTED for a mode this firmware lacks, ESP_ERR_INVALID_STATE
 * while a survey is running, ESP_OK when already in [mode].
 */
esp_err_t xprslora_set_mode(xprslora_mode_t mode);

/* ── The survey ─────────────────────────────────────────────────────────
 *
 * Listen on each available mode in turn and report what was heard, so the
 * operator chooses on evidence. Nothing is transmitted for the duration
 * (our own traffic waits, as it does when the hour is spent), nothing is
 * handed to the bearer or to a bridge, and the radio goes back to the mode
 * it started in. */

#define XPRSLORA_SURVEY_NAMES    4
#define XPRSLORA_SURVEY_NAME_LEN 20

typedef struct {
    uint32_t frames;                 /* frames heard on this mode */
    int      names;
    bool     relayed;                /* somebody carried our probe */
    bool     probed;                 /* we put one on the air here */
    char     name[XPRSLORA_SURVEY_NAMES][XPRSLORA_SURVEY_NAME_LEN];
} xprslora_survey_mode_t;

/** Start one, [per_mode_s] on each mode (clamped to 5..300). Listen only:
 *  nothing is transmitted, which also means a network whose nodes happen
 *  to be quiet looks exactly like a network that is not there. */
esp_err_t xprslora_survey_start(uint32_t per_mode_s);

/**
 * The same sweep, but it SPEAKS once on each mesh mode: auto-detect.
 *
 * Listening alone cannot answer "is anybody there". MeshCore's nodes
 * advertise every one to four hours and Meshtastic's send a NodeInfo about
 * every three, so a quiet channel is silent for far longer than anybody
 * will stand at a station waiting. What is quick is asking: one small
 * packet of the kind that network floods, and a repeater within reach
 * re-airs it within a couple of seconds (measured on the bench against a
 * MeshCore repeater, 2026-09-20). Hearing our own packet come back with a
 * hop on it is proof of a working relay, not a guess.
 *
 * XPRS's own mode is not probed: its stations beacon every few seconds, so
 * listening is enough there.
 *
 * [per_mode_s] clamped to 5..300; 20 is the default and is chosen in
 * docs/lora.md, "Auto-detect".
 */
esp_err_t xprslora_detect_start(uint32_t per_mode_s);

/** Whether one is running now. */
bool xprslora_survey_active(void);

/** The last survey as JSON, `{"running":bool,"modes":{...}}`; 0 when there
 *  has not been one. */
int xprslora_survey_json(char *buf, size_t cap);

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
 * The default depends on the mode. Six seconds in `xprs` mode, where a
 * full packet at SF7 is 0.4 s; ten in `meshtastic` mode, where a typical
 * 150-byte packet at SF11 is 1.3 s and the same six seconds would have put
 * this station on the one shared channel a fifth of the time in a burst.
 * Ten keeps a burst near a tenth, which is also what band g3 allows over
 * the hour. It is deliberately not a legal duty-cycle calculation: that
 * figure depends on band and region, and is the operator's to set. 0
 * disables pacing.
 *
 * The pace is a collision spacer; the ledger below is the accountant. Both
 * apply, and they are deliberately not one number.
 */
void xprslora_set_pace(uint32_t per_packet_ms);

/** Milliseconds until the radio may transmit again; 0 when free now. */
uint32_t xprslora_owed_ms(void);

/** This radio's airtime for [len] bytes at the SF/BW/CR xprslora_start()
 *  set -- the number the duty ledger charges. 0 before start. */
uint32_t xprslora_airtime_ms(int len);

/** Re-point the ledger, e.g. from config. 0 budget with 0 dwell = off. */
void xprslora_set_duty(uint32_t budget_ms, uint32_t reserve_ms,
                       uint32_t dwell_ms);

/** The ledger's report; zeroed when the radio is down or unmetered. */
void xprslora_duty(xb_duty_report_t *out);

/**
 * Point the radio at a frequency of the operator's choosing, now: the same
 * retune the mode switch uses, and the station keeps its uptime.
 *
 * NOT EVERY BOARD IS AN 868 MHz BOARD -- the same SX1262 is sold matched
 * for 433, 868 and 915 MHz, and a MeshCore community on 433 picks its own
 * channel -- so this is a setting rather than a table lookup. 0 goes back
 * to the region's own channel. ESP_ERR_INVALID_ARG outside what the chip
 * can tune (150-960 MHz); whether it is legal where the station stands is
 * the operator's to answer, as the power ceiling already is. The region's
 * hourly budget and its ceiling still apply.
 */
esp_err_t xprslora_set_freq(uint32_t hz);

/** What the radio is on now: the operator's frequency, else the region's. */
uint32_t xprslora_freq(void);

/** Take a region preset of the running mode, now, and with it its channel,
 *  its hour and its ceiling. ESP_ERR_NOT_FOUND when the mode has no such
 *  row. Clears a frequency set by hand: a preset brings its own. */
esp_err_t xprslora_set_region(const char *name);

/** The region the radio was started with (never NULL after start). */
const xprslora_region_t *xprslora_region(void);

/** What the radio is actually set to: the spreading factor and the
 *  bandwidth in Hz, overrides included. Zeroes when the radio is down.
 *  An operator who set `lora_sf` reads this to see that it took. */
void xprslora_modem(int *sf, uint32_t *bw_hz);

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
 * Start the repeater and the bridge on the running radio, in `meshtastic`
 * mode only (ESP_ERR_NOT_SUPPORTED otherwise, and nothing is allocated).
 * Allocates the bridge's state once (about 9 KB, PSRAM where there is some);
 * without it the radio still carries XPRS, and says so in the log.
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

/* ── MeshCore: the repeater and the bridge ───────────────────────────── */

/* The same five things the station provides either bridge, so a station
 * that grows a third network writes its hooks once. */

/**
 * Start the repeater and the bridge on the running radio, in `meshcore`
 * mode only (ESP_ERR_NOT_SUPPORTED otherwise). The state is the block the
 * mode itself claimed on the way in (xprslora_set_mode), so this allocates
 * nothing and cannot fail for want of memory.
 */
esp_err_t xprslora_mc_start(const xprslora_mt_hooks_t *hooks,
                            const mc_mesh_cfg_t *cfg, const char *nick);

/** Every XPRS packet this station hears (MC_XPRS_HEARD) or originates
 *  (MC_XPRS_OWN), on any bearer and any task. */
void xprslora_mc_offer(const char *wire, int len, int origin);

/** The bridge's counters; false when MeshCore is not the running mode. */
bool xprslora_mc_stats(mc_mesh_stats_t *out);

/** MeshCore nodes heard; [i] 0..n-1 into [out] (a copy). Returns how many. */
int xprslora_mc_node(int i, mc_node_t *out);

/** The station's nick changed (announced at the next advert). */
void xprslora_mc_set_nick(const char *nick);

/** Stand the MeshCore worker down for an install, and back up after: it is
 *  6 KB of stack that a 1.4 MB transfer would rather have. Nothing is
 *  forgotten -- the bridge's own state stays. */
void xprslora_mc_pause(bool quiet);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_BEARER_LORA_H */
