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
 * ON AIRTIME. 250 bytes at SF7/125 kHz is ~400 ms on the air, and 868 MHz is
 * a shared band with a duty-cycle obligation (1% in most of its sub-bands).
 * The bearer's own jitter and dupe-cancel already keep chatter down; do not
 * add periodic traffic here without doing the arithmetic.
 */
#ifndef XPRS_BEARER_LORA_H
#define XPRS_BEARER_LORA_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The radio's wiring and tuning. A board that has no SX1262 simply never
 *  calls xprslora_start. */
typedef struct {
    int sck_pin, mosi_pin, miso_pin;
    int cs_pin, rst_pin, busy_pin, dio1_pin;
    uint32_t freq_hz;        /* 0 = 868 MHz */
    int8_t tx_power_dbm;     /* 0 = a polite 14 dBm (the EU limit for g1) */
    bool use_tcxo;           /* module has a TCXO on DIO3 (the T-Deck does) */
    bool use_dio2_rf_switch; /* DIO2 drives the RF switch (the T-Deck too) */
} xprslora_cfg_t;

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
 */
#define XPRSLORA_PACE_DEFAULT_MS 6000u

/** RX/TX/cancelled/dupes counters, any may be NULL. */
void xprslora_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                    uint32_t *dupes);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_BEARER_LORA_H */
