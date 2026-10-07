/* Which LoRa mesh is around (lr_detect.h). */
#include "lr_detect.h"

#include <string.h>

void lrd_begin(lrd_t *d, const uint8_t *order, int n, const lrd_timing_t *t,
               uint32_t now)
{
    memset(d, 0, sizeof *d);
    if (n > LRD_N) n = LRD_N;
    for (int i = 0; i < n; i++) d->order[i] = order[i];
    d->n = (uint8_t)n;
    d->t = *t;
    d->entered_ms = now;
    d->active = n > 0;
}

uint8_t lrd_current(const lrd_t *d) { return d->order[d->at < d->n ? d->at : 0]; }

lrd_act_t lrd_tick(lrd_t *d, uint32_t now, uint8_t *net)
{
    if (!d->active) return LRD_IDLE;
    uint8_t cur = d->order[d->at];
    if (!d->tuned) {
        d->tuned = true;
        d->entered_ms = now;
        if (net) *net = cur;
        return LRD_TUNE;
    }
    uint32_t in = now - d->entered_ms;
    lrd_ev_t *e = &d->ev[cur];
    uint32_t dwell = d->t.dwell_ms[cur];
    bool over = e->relayed || in >= dwell;
    if (over) {
        d->at++;
        d->tuned = false;
        if (d->at >= d->n) {
            d->active = false;
            return LRD_DONE;
        }
        uint8_t nx = d->order[d->at];
        d->tuned = true;
        d->entered_ms = now;
        if (net) *net = nx;
        return LRD_TUNE;
    }
    /* First probe once settled; a second at half the dwell (a Meshtastic
     * router may wait its turn, and the first probe may have collided). */
    if ((e->probes == 0 && in >= d->t.settle_ms) ||
        (e->probes == 1 && in >= dwell / 2))
        return LRD_PROBE;
    return LRD_IDLE;
}

void lrd_probed(lrd_t *d)
{
    if (!d->active) return;
    lrd_ev_t *e = &d->ev[d->order[d->at]];
    if (e->probes < 255) e->probes++;
}

void lrd_on_frame(lrd_t *d, bool echo, bool xprs)
{
    if (!d->active) return;
    lrd_ev_t *e = &d->ev[d->order[d->at]];
    if (echo) { e->relayed = true; return; }
    if (e->frames < 0xFFFF) e->frames++;
    if (xprs && e->xprs < 0xFFFF) e->xprs++;
}

int lrd_rank(const lrd_ev_t *e)
{
    if (e->relayed) return 3;
    if (e->xprs) return 2;
    if (e->frames) return 1;
    return 0;
}

/* The strictly best network; on a tie the one we are on keeps it, then the
 * busier of the others. */
static uint8_t best_of(const lrd_ev_t ev[LRD_N], uint8_t prefer)
{
    uint8_t best = prefer;
    for (uint8_t i = 0; i < LRD_N; i++) {
        if (i == prefer) continue;
        int ri = lrd_rank(&ev[i]), rb = lrd_rank(&ev[best]);
        if (ri > rb || (ri == rb && best != prefer && ev[i].frames > ev[best].frames))
            best = i;
    }
    return best;
}

uint8_t lrd_adopt(lrd_pick_t *pk, const lrd_ev_t ev[LRD_N], bool first)
{
    if (pk->current >= LRD_N) pk->current = LRD_MT;
    uint8_t best = best_of(ev, pk->current);
    int rb = lrd_rank(&ev[best]), rc = lrd_rank(&ev[pk->current]);
    if (best == pk->current || rb == 0 || rb <= rc) {
        /* Nothing better, or nothing at all: stay, and forget any lean. */
        pk->strikes = 0;
        return pk->current;
    }
    if (first) {
        pk->current = best;
        pk->strikes = 0;
        return best;
    }
    if (pk->wanted == best) {
        if (pk->strikes < 255) pk->strikes++;
    } else {
        pk->wanted = best;
        pk->strikes = 1;
    }
    if (pk->strikes >= 2) {
        pk->current = best;
        pk->strikes = 0;
    }
    return pk->current;
}

bool lrd_due(uint32_t now, uint32_t last, uint32_t period, bool busy)
{
    return !busy && (uint32_t)(now - last) >= period;
}
