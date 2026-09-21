/**
 * @file lr_rotate.h
 * @brief When a station sharing one radio between two networks should move.
 *
 * A LoRa station has one receiver, so serving Meshtastic and MeshCore at
 * once means serving them in turn, and while it is on one it is deaf to
 * the other. Neither network holds anything for a node that was not
 * listening: Meshtastic's Store & Forward is off by default and cannot
 * decrypt other people's direct messages, MeshCore's room servers are
 * separate hardware, and both repeaters are stateless floods
 * (docs/lora.md, "Taking turns on two networks").
 *
 * So the slice is bounded by the OTHER networks' patience, not by ours:
 * a Meshtastic sender airs a direct message three times over 15 to 45 s
 * and then gives up; a MeshCore client tries three times over about 20 to
 * 25 s on our channel; a broadcast and a channel message are each aired
 * exactly once, with no retry at all.
 *
 * The decision is kept here, away from the radio, because it is
 * arithmetic and arithmetic can be tested (test_rotate_host.c).
 */
#ifndef XPRS_LR_ROTATE_H
#define XPRS_LR_ROTATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How recently a direct message must have gone either way for the running
 *  network to count as busy. One minute: long enough to cover the gap
 *  between a message and its answer, short enough that a quiet exchange
 *  releases the radio. */
#define LR_ROT_RECENT_MS 60000u

/** Everything the decision depends on, gathered by the caller so that the
 *  decision itself touches nothing. */
typedef struct {
    uint32_t now_ms;
    uint32_t slice_ms;      /* when the running slice started */
    uint32_t floor_ms;      /* never leave before this */
    uint32_t ceiling_ms;    /* leave even when busy after this */
    uint8_t  n_modes;       /* how many networks are in the ring */
    bool     busy;          /* the running bridge is mid-exchange */
    bool     blocked;       /* transmitting, or still settling after a retune */
} lr_rotate_in_t;

/** Is this slice over? */
bool lr_rotate_due(const lr_rotate_in_t *in);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_LR_ROTATE_H */
