/*
 * Who an nRF52 station hears directly, on which link, and what Bluetooth may
 * hand to LoRa. Shared by models/sensecap-p1-pro and models/sensecap-t1000e:
 * lifted out of the T1000-E's main.cpp (2026-11-25) when the P1-Pro, on
 * Meshtastic's LongFast with only lr_worth to go by, was found spending most
 * of its hour of airtime re-airing other stations' history commands.
 *
 * The hears: list of a beacon (XPRS.md 15.6.3) names stations heard with no
 * via:, a neighbour one hop away. The ESP32 boards keep this in
 * xprs_station, which leans on FreeRTOS and is not ported.
 */
#ifndef XPRS_NRF_HEARS_H
#define XPRS_NRF_HEARS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NHR_MAX      16
#define NHR_FRESH_MS 600000UL       /* "heard" means within ten minutes */

/* A peer id for the bearer's table, from the callsign (FNV-1a, never 0);
 * [direct] (may be NULL) says whether the packet came with no via:. */
uint64_t nhr_peer_of(const char *wire, int len, bool *direct);

/* Note a packet heard on Bluetooth ([ble]) or LoRa. Only a direct one
 * (no via:) from somebody else counts. */
void nhr_touch(const char *wire, int len, bool ble);

/* Heard directly on that link within NHR_FRESH_MS. */
bool nhr_on(const char *call, bool ble);

/* Heard directly, on any link, within [ms]. */
bool nhr_heard_directly(const char *call, uint32_t ms);

int  nhr_count(void);

/* The first station heard directly on Bluetooth within [ms] that [want]
 * accepts (an owner's phone, say), or NULL. */
const char *nhr_find_ble(uint32_t ms, bool (*want)(const char *call));

/* " hears:A,B,..." most recent first, so a truncated list keeps the useful
 * half; 0 and nothing written when nobody is fresh. */
int  nhr_render(char *out, int cap);

/*
 * What Bluetooth (or a LoRa digipeat) may put on a shared LoRa channel. A
 * station in a room full of stations hears their whole conversation, most
 * of which is no business of a shared, slow channel. In this order:
 *  - only what is worth LoRa (lr_worth) and never what its author kept to
 *    the room (scope:local, XPRS.md 9.11.1);
 *  - not a packet for somebody heard directly on Bluetooth: it reached them
 *    already, the way it reached us;
 *  - a command, a result or a request only for somebody heard directly on
 *    LoRa: a phone here asking a station only LoRa reaches. The rest is
 *    stations keeping their archives in step on media of their own;
 *  - somebody's identity once in three hours: a phone re-signs it every
 *    half hour, and the keys in it do not change;
 *  - not old news: a ts: more than half an hour behind now (or, with no
 *    clock, behind the newest ts: heard lately) is a carousel or a history
 *    replay;
 *  - not twice in half an hour.
 * True: offer it to LoRa (and it is remembered as aired).
 */
bool nhr_to_lora(const char *wire, int len);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_NRF_HEARS_H */
