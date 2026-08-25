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

bool xprslora_is_active(void);

/** RX/TX/cancelled/dupes counters, any may be NULL. */
void xprslora_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                    uint32_t *dupes);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_BEARER_LORA_H */
