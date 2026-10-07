/*
 * Which LoRa mesh is around: sweep, ask, decide, and decide again later.
 * Platform-free arithmetic in the style of lr_rotate.c: the caller owns the
 * radio and the clock, this owns the sequence and the verdict.
 *
 * THE SWEEP. Each network in [order] in turn: retune, wait LRD settle time
 * (a frame aired ~30 ms after a retune is demodulated by nobody, measured
 * 2026-09-20), probe, probe again at half the dwell, and move on when the
 * dwell is up. The dwell ends EARLY only when our own probe comes back
 * relayed (docs/lora.md section 4): hearing somebody else's frame is
 * evidence the network is alive, not that it will carry us.
 *
 * THE VERDICT. Evidence ranks: a relayed probe (a repeater carries us) beats
 * an XPRS frame heard (XPRS stations live here) beats any frame (the network
 * is alive) beats nothing. The best network wins. With no evidence anywhere
 * the current network is kept: an empty hour is not a reason to move.
 *
 * AND AGAIN, LATER. A periodic check moves only after TWO consecutive sweeps
 * agree that another network is strictly better, so one lucky probe or one
 * quiet hour does not flip a card between meshes.
 */
#ifndef XPRS_LR_DETECT_H
#define XPRS_LR_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LRD_MT = 0, LRD_MC = 1, LRD_N = 2 };

typedef struct {
    uint16_t frames;   /* anything of this network's heard during the dwell */
    uint16_t xprs;     /* of which XPRS frames */
    uint8_t  probes;   /* probes we aired */
    bool     relayed;  /* a probe came back carried */
} lrd_ev_t;

typedef struct {
    uint32_t settle_ms;          /* after a retune, before our first probe */
    uint32_t dwell_ms[LRD_N];    /* ceiling per network */
} lrd_timing_t;

typedef enum {
    LRD_IDLE = 0,   /* nothing to do this tick */
    LRD_TUNE,       /* retune to *net now (it is the next stop) */
    LRD_PROBE,      /* air a probe on the current network now */
    LRD_DONE,       /* the sweep is over: read the evidence, call lrd_adopt */
} lrd_act_t;

typedef struct {
    bool         active;
    uint8_t      order[LRD_N];
    uint8_t      n, at;
    uint32_t     entered_ms;
    bool         tuned;
    lrd_ev_t     ev[LRD_N];
    lrd_timing_t t;
} lrd_t;

/* Start a sweep over [order] (n of them). Evidence is cleared. */
void lrd_begin(lrd_t *d, const uint8_t *order, int n, const lrd_timing_t *t,
               uint32_t now);
/* Call every tick while d->active; [net] gets the network for LRD_TUNE. */
lrd_act_t lrd_tick(lrd_t *d, uint32_t now, uint8_t *net);
/* The caller aired a probe on the current stop. */
void lrd_probed(lrd_t *d);
/* A frame heard on the current stop. [echo]: it is our probe, carried. */
void lrd_on_frame(lrd_t *d, bool echo, bool xprs);
/* The network the sweep is on now (valid while active). */
uint8_t lrd_current(const lrd_t *d);

/* 3 relayed, 2 XPRS heard, 1 frames, 0 nothing. */
int lrd_rank(const lrd_ev_t *e);

/* The adoption, with memory across sweeps. [first]: the boot sweep, which
 * adopts the best at once; later sweeps need two in a row. */
typedef struct {
    uint8_t current;
    uint8_t wanted;     /* the network the last sweep preferred */
    uint8_t strikes;    /* consecutive sweeps preferring [wanted] */
} lrd_pick_t;
uint8_t lrd_adopt(lrd_pick_t *pk, const lrd_ev_t ev[LRD_N], bool first);

/* Is a periodic check due? Never while [busy]. */
bool lrd_due(uint32_t now, uint32_t last, uint32_t period, bool busy);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_LR_DETECT_H */
