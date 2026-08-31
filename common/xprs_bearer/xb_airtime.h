/**
 * @file xb_airtime.h
 * @brief What a LoRa packet costs on the air, in milliseconds.
 *
 * Semtech AN1200.13's time-on-air arithmetic, in integers, with no radio and
 * no platform behind it. It lives in xprs_bearer rather than beside the
 * SX1262 driver because the SenseCAP P1-Pro -- the one board that sits on a
 * pole at 22 dBm for a year -- takes this component by symlink and never
 * links the ESP-IDF LoRa bearer at all. The arithmetic ships neutral so that
 * every implementation charges the same numbers.
 *
 * Checked against the app note's own worked figures at SF7/125 kHz/CR 4-5,
 * preamble 8, CRC on: 250 bytes = 389 ms, 64 bytes = 118 ms
 * (test_xbduty_host.c holds the table).
 */
#ifndef XB_AIRTIME_H
#define XB_AIRTIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bw_hz;      /* 125000 for the fleet's channel */
    uint8_t  sf;         /* 7..12 */
    uint8_t  cr;         /* 1..4 == 4/5 .. 4/8 */
    uint8_t  preamble;   /* symbols; the fleet uses 8 */
    bool     crc;        /* CRC on, as the fleet transmits */
    bool     implicit_header;
} xb_lora_air_t;

/**
 * Milliseconds of airtime for [payload_len] bytes, rounded up.
 *
 * Low-data-rate optimisation is DERIVED (symbol time > 16 ms, i.e. SF11/SF12
 * at 125 kHz), never passed in: two boards disagreeing about DE is exactly
 * the silent class of mismatch the "MUST MATCH" comments in the LoRa configs
 * already warn about, and one copy of the rule is enough.
 */
uint32_t xb_lora_airtime_ms(const xb_lora_air_t *p, int payload_len);

#ifdef __cplusplus
}
#endif
#endif /* XB_AIRTIME_H */
