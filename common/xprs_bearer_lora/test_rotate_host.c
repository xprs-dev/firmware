/* test_rotate_host.c -- the rotation's decision, on the host.
 *
 * lr_rotate_due() is the whole of when a station sharing one radio between
 * two networks may leave the one it is on. It is arithmetic over numbers
 * the caller gathers, so it is tested here rather than on a radio; what a
 * radio has to prove is a different list (docs/lora.md, "Taking turns").
 *
 * Every case below was run once against the unfixed code first: a test
 * that passes without its fix proves nothing (docs/lora.md, lessons).
 */

#include <stdio.h>
#include <stdlib.h>

#include "lr_rotate.h"

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

/* The numbers a station actually runs with: a 25 s floor, four slices of
 * ceiling, two networks in the ring. */
static lr_rotate_in_t base(uint32_t in_slice_ms)
{
    lr_rotate_in_t in = {
        .now_ms = 1000000u + in_slice_ms,
        .slice_ms = 1000000u,
        .floor_ms = 25000u,
        .ceiling_ms = 100000u,
        .n_modes = 2,
        .busy = false,
        .blocked = false,
    };
    return in;
}

int main(void)
{
    /* The floor: an idle network is left as soon as its turn is up, and
     * not one tick before. */
    {
        lr_rotate_in_t in = base(0);
        check(!lr_rotate_due(&in), "a slice does not end the moment it starts");
        in = base(24999u);
        check(!lr_rotate_due(&in), "the floor holds to its last millisecond");
        in = base(25000u);
        check(lr_rotate_due(&in), "an idle network is left when the floor is up");
        in = base(600000u);
        check(lr_rotate_due(&in), "and stays left long afterwards");
    }

    /* Busy: an exchange holds the radio past the floor, because the other
     * side gives up in well under a minute. */
    {
        lr_rotate_in_t in = base(30000u);
        in.busy = true;
        check(!lr_rotate_due(&in), "a live exchange holds the radio past the floor");
        in = base(99999u);
        in.busy = true;
        check(!lr_rotate_due(&in), "and holds it to the last millisecond of the ceiling");
        in = base(100000u);
        in.busy = true;
        check(lr_rotate_due(&in), "the ceiling ends the courtesy");
        in = base(20000u);
        in.busy = true;
        check(!lr_rotate_due(&in), "busy below the floor is still below the floor");
    }

    /* Blocked: nothing moves the radio mid-transmission or mid-settle,
     * whatever the clock says. */
    {
        lr_rotate_in_t in = base(200000u);
        in.blocked = true;
        check(!lr_rotate_due(&in), "a transmission in flight blocks the turn");
        in = base(200000u);
        in.blocked = true;
        in.busy = true;
        check(!lr_rotate_due(&in), "blocked beats busy and the ceiling both");
    }

    /* A ring of one is a mode, not a rotation. */
    {
        lr_rotate_in_t in = base(200000u);
        in.n_modes = 1;
        check(!lr_rotate_due(&in), "one network never takes a turn");
        in = base(200000u);
        in.n_modes = 0;
        check(!lr_rotate_due(&in), "an empty ring never takes a turn");
        check(!lr_rotate_due(NULL), "and neither does nothing at all");
    }

    /* The clock wraps every 49 days and a station outlives that. The
     * subtraction is unsigned on purpose; this is the case that proves
     * it. */
    {
        const uint32_t before_wrap = 4096u;         /* 0xFFFFF000 */
        lr_rotate_in_t in = base(0);
        in.slice_ms = (uint32_t)(0u - before_wrap);
        in.now_ms = 20000u - before_wrap;           /* 20 s into the slice */
        check(!lr_rotate_due(&in), "a slice that straddles the wrap is not over");
        in.now_ms = 28000u - before_wrap;           /* 28 s into it */
        check(lr_rotate_due(&in), "and ends on time on the other side of it");
    }

    /* A ceiling below the floor is an operator's arithmetic error, not a
     * reason to sit on one network for ever: the floor still governs. */
    {
        lr_rotate_in_t in = base(30000u);
        in.ceiling_ms = 10000u;
        in.busy = true;
        check(lr_rotate_due(&in), "a ceiling under the floor does not trap the radio");
    }

    if (fails) { printf("rotate: %d check(s) failed\n", fails); return 1; }
    printf("lr_rotate: all checks passed\n");
    return 0;
}
