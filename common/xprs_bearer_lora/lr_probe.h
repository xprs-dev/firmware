/*
 * The auto-detect probe: one small packet of the kind a network floods, so a
 * repeater in reach answers by re-airing it, and the matcher that recognises
 * the re-air. Lifted out of xprslora.c (survey_probe, survey_echo) so the
 * nRF52 cards detect a network exactly as the ESP32 stations do.
 * Platform-free: no radio, no clock, no randomness of its own.
 *
 *   meshtastic  a Data frame on XPRS's private portnum, one byte of payload,
 *               on LongFast's channel hash (NOT the XPRS one: XPRS stations
 *               would park it as a fragment and never repeat it). Every
 *               router relays by the header, not by what it can read.
 *   meshcore    an ACK with a checksum that matches nothing: four bytes, no
 *               crypto, a type their repeaters carry.
 *
 * Measured (docs/lora.md section 4): a MeshCore repeater re-airs within
 * 1.3-2 s, a Meshtastic one within 7.6 s; the strongest Meshtastic node
 * waits longest, by its own contention rule.
 */
#ifndef XPRS_LR_PROBE_H
#define XPRS_LR_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t mt_from, mt_id;     /* the Meshtastic probe's identity */
    uint32_t mc_hash;            /* the MeshCore probe's packet hash */
    bool     mt_live, mc_live;
} lrp_probe_t;

/* Build the probe into [out] (cap >= 255). [rnd] is fresh randomness from
 * the caller. Returns the frame length, 0 on failure; records what the echo
 * must match in [pr]. */
int lrp_mt_build(lrp_probe_t *pr, uint32_t self_node, uint32_t rnd,
                 uint8_t *out, int cap);
int lrp_mc_build(lrp_probe_t *pr, uint32_t rnd, uint8_t *out, int cap);

/* Is [frame] our probe, carried by somebody else? */
bool lrp_mt_echo(const lrp_probe_t *pr, const uint8_t *frame, int len);
bool lrp_mc_echo(const lrp_probe_t *pr, const uint8_t *frame, int len);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_LR_PROBE_H */
