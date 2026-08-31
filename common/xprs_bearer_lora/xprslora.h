/**
 * @file xprslora.h
 * @brief XPRS over LoRa: the third bearer, and the first with real range.
 *
 * The wire is the wire: the same <=250-byte ASCII packet every other bearer
 * carries, as the LoRa payload, no framing around it. 250 bytes fits the
 * SX1262's 255-byte buffer with room left over, which is not a coincidence --
 * the format's size limit was chosen for radios like this one.
 *
 * All the bearer discipline -- re-air jitter, the 13.2.1 cancel, duplicate
 * rings, beacons -- is xprs_bearer, shared with ESP-NOW and the LAN.
 * What this file adds is only the radio: an SX1262 behind the five function
 * pointers, receiving into a queue from the DIO1 interrupt's wake and airing
 * with the airtime respect a shared band demands.
 *
 * ON AIRTIME. 250 bytes at SF7/125 kHz is 390 ms on the air, and the band
 * is shared with a duty-cycle obligation -- 10% in band g3 (869.4-869.65,
 * where the fleet now sits), 1% in g1. Since 2026-08-31 the arithmetic is
 * DONE: a duty ledger (xb_set_duty) charges every transmission its real
 * time-on-air against a rolling hour, holds ordinary traffic when the
 * budget is spent, and keeps a reserve so an sos still leaves. Periodic
 * traffic is still not free -- it spends the same hour.
 */
#ifndef XPRS_BEARER_LORA_H
#define XPRS_BEARER_LORA_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "xprsbearer.h"

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
    uint8_t sf;              /* 0 = SF7 (the fleet); 9 = the `far` profile */
} xprslora_cfg_t;

/**
 * Where in the spectrum this station is allowed to be, and what it owes.
 *
 * The band moved on 2026-08-31: 868.000 MHz put half of a 125 kHz channel
 * BELOW band g1's floor while paying g1's price -- 1% duty and 14 dBm.
 * ERC 70-03 band g3 (869.40-869.65 MHz) allows 10% and up to 27 dBm
 * e.r.p., ten times the airtime and twice the range, so `eu` centres at
 * 869.5 MHz where the whole channel fits with margin. `eu-g1` remains for
 * an operator who must stay in the old sub-band. The US and AU 900 MHz
 * regimes cap the length of one transmission (dwell) rather than the hour.
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
 * Six seconds, which is section 31.1's own order of magnitude for one packet
 * at SF9 -- strict enough that a busy LAN cannot pour traffic onto the radio,
 * loose enough that a bench stays usable. The same figure the Flutter station
 * uses for its LoRa bearer, so the two implementations agree.
 *
 * The pace is a collision spacer; the ledger below is the accountant. Both
 * apply, and they are deliberately not one number.
 */
#define XPRSLORA_PACE_DEFAULT_MS 6000u

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

#ifdef __cplusplus
}
#endif

#endif /* XPRS_BEARER_LORA_H */
