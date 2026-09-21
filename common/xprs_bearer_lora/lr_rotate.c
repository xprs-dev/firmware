/* lr_rotate.c -- see lr_rotate.h. Platform-free on purpose. */

#include "lr_rotate.h"

bool lr_rotate_due(const lr_rotate_in_t *in)
{
    /* One network is not a rotation, and a ring of none is a bug upstream. */
    if (!in || in->n_modes < 2) return false;

    /* Never in the middle of a transmission, and never inside the settle
     * window after a retune: a frame aired about 30 ms after the modem
     * moved was transmitted in full and demodulated by nobody, four runs
     * out of four (2026-09-20). */
    if (in->blocked) return false;

    uint32_t in_slice = in->now_ms - in->slice_ms;

    /* The floor. Below it the settling costs more than the turn is worth,
     * and each network sees a station that appears and vanishes. */
    if (in_slice < in->floor_ms) return false;

    /* Somebody is mid-exchange with us: stay, because their sender gives
     * up in well under a minute and a message half delivered is a message
     * lost. The ceiling ends that courtesy -- on a lively channel "busy"
     * is almost always true, and without a ceiling the other network
     * would never be served again. */
    if (in->busy && in_slice < in->ceiling_ms) return false;

    return true;
}
