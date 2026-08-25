#include "xcadence.h"

uint32_t xcadence_ceiling(uint32_t silent_for_s)
{
    if (silent_for_s >= XC_QUARTER_SILENT) return XC_CEIL_QUARTER;
    if (silent_for_s >= XC_WEEK_SILENT) return XC_CEIL_WEEK;
    return XC_CEIL_FRESH;
}

uint32_t xcadence_floor(xc_peer_t peer, bool visible)
{
    if (!visible) return XC_HIDDEN_FLOOR;
    return peer == XC_FAST ? XC_FAST_FLOOR : XC_ORDINARY_FLOOR;
}

uint32_t xcadence_next(uint32_t current_s, xc_answer_t answer,
                       xc_peer_t peer, bool visible, uint32_t silent_for_s)
{
    const uint32_t floor = xcadence_floor(peer, visible);
    const uint32_t ceiling = xcadence_ceiling(silent_for_s);
    if (!current_s) current_s = XC_ORDINARY_FLOOR;

    uint32_t next;
    switch (answer) {
    case XC_NEWS:
        next = current_s / 2;
        break;
    case XC_QUIET:
        next = current_s > (0xFFFFFFFFu / 2) ? ceiling : current_s * 2;
        break;
    default: {
        /* Not clamped to the floor: the refusal says the floor was wrong. */
        const uint32_t base = current_s < XC_REFUSED_MIN ? XC_REFUSED_MIN
                                                         : current_s;
        const uint32_t backed = base > (0xFFFFFFFFu / 2) ? ceiling : base * 2;
        return backed > ceiling ? ceiling : backed;
    }
    }

    /* A ladder step can put the ceiling below the floor -- an ordinary peer
     * silent for a week wants 60 min against a 10 min floor. The ceiling
     * wins: asking less often is always allowed, asking more often is what
     * the budgets forbid. */
    if (next > ceiling) return ceiling;
    if (next < floor) return floor > ceiling ? ceiling : floor;
    return next;
}

uint32_t xcadence_initial(void) { return XC_ORDINARY_FLOOR; }

uint32_t xcadence_jitter(uint32_t seconds, uint32_t rand)
{
    if (seconds < 2) return seconds;
    const uint32_t span = seconds / 5;            /* +/-10% is a fifth wide */
    if (!span) return seconds;
    const uint32_t delta = rand % (span + 1);
    const uint32_t out = seconds - span / 2 + delta;
    return out < 1 ? 1 : out;
}
