/*
 * A repeater and nothing else, for a station that carries a native mesh but
 * does not bridge it: a battery card has neither the flash nor the stack for
 * mt_mesh.c / mc_mesh.c, whose node keys alone pull in X25519 and Ed25519.
 *
 * The rules are theirs, kept the same so a card and a station on the same
 * channel behave alike (and cancel each other):
 *
 *   meshtastic  the managed flood. A frame not heard in the last ten
 *               minutes, with hops left, not ours, not addressed to us and
 *               not steered to another next hop, is re-aired with
 *               hop_limit - 1 and our relay byte after the firmware's CLIENT
 *               wait (2 x CWmax slots plus a random number of slots, fewer
 *               for a faint copy, which came from further away).
 *   meshcore    the flood repeater. A flood packet short of the hop cap,
 *               with room in its path and not already through us, goes on
 *               with our hash appended after MeshCore's random wait; a
 *               direct packet only when we are the next hop on its path.
 *
 * Either way a copy heard before ours left cancels ours: somebody nearer
 * already did the job. XPRS frames are never repeated here (the station
 * carries XPRS itself, by the packet, not by the frame); they are only
 * noted, so a later copy is not mistaken for native traffic.
 *
 * Our MeshCore "hash" is not a public key prefix (that is what costs
 * Ed25519); it is a stable byte of our own, which is all a path needs for
 * its loop check. A client looking at the path sees an unknown repeater.
 *
 * Platform-free: the caller owns the radio, the clock and the randomness.
 */
#ifndef XPRS_LR_REPEAT_H
#define XPRS_LR_REPEAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LRR_SEEN      40
#define LRR_TXQ       3
#define LRR_SEEN_MS   600000u     /* ten minutes, as the firmware's history */
#define LRR_STALE_MS  60000u      /* a relay not aired in a minute is news no more */
#define LRR_MC_MAX_HOPS 8         /* mc_mesh.h MC_RELAY_MAX_HOPS */
#define LRR_FRAME_MAX 255

typedef struct {
    uint32_t a, b;               /* mt: from, id. mc: packet hash, 0 */
    uint32_t t_ms;
} lrr_seen_t;

typedef struct {
    bool     used;
    uint8_t  len;
    uint32_t a, b;
    uint32_t due_ms, queued_ms;
    uint8_t  frame[LRR_FRAME_MAX];
} lrr_q_t;

typedef struct {
    uint32_t heard, dupes, relayed, aired, cancelled, skipped, dropped;
} lrr_stats_t;

typedef struct {
    uint8_t     net;             /* LRD_MT or LRD_MC (lr_detect.h) */
    uint32_t    self_node;       /* our Meshtastic node number */
    uint8_t     self_hash[3];    /* our MeshCore path bytes */
    uint32_t    slot_ms;
    lrr_seen_t  seen[LRR_SEEN];
    int         seen_pos;
    lrr_q_t     q[LRR_TXQ];
    lrr_stats_t st;
} lrr_t;

/* One instance per network. [self_hash] may be NULL on Meshtastic. */
void lrr_init(lrr_t *r, uint8_t net, uint32_t self_node, const uint8_t self_hash[3]);

/* A frame of the network's own, heard. [air_ms]: what this frame costs on
 * the air (MeshCore's wait is built on it). [rnd]: fresh randomness. */
void lrr_on_frame(lrr_t *r, const uint8_t *f, int len, int snr, uint32_t air_ms,
                  uint32_t now, uint32_t rnd);

/* An XPRS frame heard, or any frame we aired ourselves: remembered, so a
 * copy of it is never repeated as native traffic. */
void lrr_note(lrr_t *r, const uint8_t *f, int len, uint32_t now);

/* The queued relay due now (stale ones are dropped on the way), or -1. */
int  lrr_due(lrr_t *r, uint32_t now);
/* What became of lrr_due's frame. Not aired: it is tried again a little
 * later, as the firmware does after a busy channel. */
void lrr_aired(lrr_t *r, int i, bool ok, uint32_t now, uint32_t rnd);

/* Anything queued due within a second? (Not a moment to leave the channel.) */
bool lrr_busy(const lrr_t *r, uint32_t now);

/* Forget the queue (the station moved to another network). */
void lrr_flush(lrr_t *r);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_LR_REPEAT_H */
