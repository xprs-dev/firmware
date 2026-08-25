/*
 * Host test for the poll ladder. It exists because this is a LOAD setting on
 * somebody else's station: every number here is either a budget the peer
 * enforces or a battery the user pays for, so "it seemed about right" is not
 * a defence. The phone's xprs_cadence_test.dart checks the same shapes.
 */
#include "xcadence.h"

#include <stdio.h>

static int checks, failed;
#define CHECK(cond, ...) do {                                              \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failed++;                                                      \
            printf("  FAIL %s:%d  ", __func__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* A room that wakes up must be followed quickly, and one that goes quiet must
 * be let go gently. */
static void test_halves_and_doubles(void)
{
    uint32_t iv = 600;
    iv = xcadence_next(iv, XC_NEWS, XC_FAST, true, 0);
    CHECK(iv == 300, "news gave %u, wanted 300", iv);
    iv = xcadence_next(iv, XC_NEWS, XC_FAST, true, 0);
    CHECK(iv == 150, "news gave %u, wanted 150", iv);
    iv = xcadence_next(iv, XC_QUIET, XC_FAST, true, 0);
    CHECK(iv == 300, "quiet gave %u, wanted 300", iv);
}

/* Only a super's raised budgets can serve a fast caller. Asking an ordinary
 * peer faster steals its whole cross-caller allowance. */
static void test_floors(void)
{
    uint32_t iv = 20;
    for (int i = 0; i < 8; i++)
        iv = xcadence_next(iv, XC_NEWS, XC_FAST, true, 0);
    CHECK(iv == XC_FAST_FLOOR, "a busy super settled at %u, floor is %u",
          iv, XC_FAST_FLOOR);

    iv = 600;
    for (int i = 0; i < 8; i++)
        iv = xcadence_next(iv, XC_NEWS, XC_ORDINARY, true, 0);
    CHECK(iv == XC_ORDINARY_FLOOR,
          "an ordinary peer was polled at %u, floor is %u",
          iv, XC_ORDINARY_FLOOR);

    /* Nobody looking: a super gets no more than an ordinary peer, because the
     * news nobody is awake to read is not worth anybody's battery. */
    iv = 15;
    iv = xcadence_next(iv, XC_NEWS, XC_FAST, false, 0);
    CHECK(iv == XC_HIDDEN_FLOOR, "hidden gave %u, wanted %u",
          iv, XC_HIDDEN_FLOOR);
}

/* The ladder: what has said nothing for months is not worth asking often. */
static void test_ceilings(void)
{
    CHECK(xcadence_ceiling(0) == XC_CEIL_FRESH, "fresh ceiling wrong");
    CHECK(xcadence_ceiling(XC_WEEK_SILENT) == XC_CEIL_WEEK, "week wrong");
    CHECK(xcadence_ceiling(XC_QUARTER_SILENT) == XC_CEIL_QUARTER,
          "quarter wrong");
    CHECK(xcadence_ceiling(XC_WEEK_SILENT - 1) == XC_CEIL_FRESH,
          "a day short of a week already climbed the ladder");

    uint32_t iv = 600;
    for (int i = 0; i < 10; i++)
        iv = xcadence_next(iv, XC_QUIET, XC_ORDINARY, true, XC_QUARTER_SILENT);
    CHECK(iv == XC_CEIL_QUARTER, "a three-month silence settled at %u", iv);

    /* The ceiling wins over the floor when the ladder puts it lower --
     * asking less often is always allowed. */
    iv = xcadence_next(60, XC_NEWS, XC_FAST, true, XC_QUARTER_SILENT);
    CHECK(iv <= XC_CEIL_QUARTER, "climbed past the ceiling to %u", iv);
}

/*
 * A 429 must always slow us down, and must be able to back off PAST the floor
 * -- the refusal is the peer saying that floor was wrong for it. This is the
 * case a clamp would quietly break: floored at 15 s against a peer refusing
 * us, we would ask 240 times an hour and be refused 240 times.
 */
static void test_refusal_always_slows(void)
{
    uint32_t iv = XC_FAST_FLOOR;
    uint32_t prev = iv;
    for (int i = 0; i < 6; i++) {
        iv = xcadence_next(iv, XC_REFUSED, XC_FAST, true, 0);
        CHECK(iv > prev || iv == XC_CEIL_FRESH,
              "a refusal did not slow us: %u -> %u", prev, iv);
        prev = iv;
    }
    CHECK(iv == XC_CEIL_FRESH, "refusals settled at %u", iv);

    iv = xcadence_next(1, XC_REFUSED, XC_FAST, true, 0);
    CHECK(iv >= XC_REFUSED_MIN,
          "a refusal left us asking every %u s, minimum is %u",
          iv, XC_REFUSED_MIN);
}

/* An archiver nobody has asked yet is polite, and earns its speed. */
static void test_initial(void)
{
    CHECK(xcadence_initial() == XC_ORDINARY_FLOOR,
          "a stranger is started at %u", xcadence_initial());
    /* Zero is not a cadence: a caller with no state must not end up asking
     * continuously. */
    CHECK(xcadence_next(0, XC_NEWS, XC_FAST, true, 0) >= XC_FAST_FLOOR,
          "no state gave a sub-floor interval");
}

/* Many stations pulling one super on the same interval arrive together. */
static void test_jitter_stays_in_band(void)
{
    for (uint32_t r = 0; r < 200; r++) {
        const uint32_t j = xcadence_jitter(600, r);
        CHECK(j >= 540 && j <= 660, "jitter of 600 gave %u", j);
    }
    CHECK(xcadence_jitter(1, 7) == 1, "jitter broke a one-second interval");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("xcadence host tests\n");
    test_halves_and_doubles();
    test_floors();
    test_ceilings();
    test_refusal_always_slows();
    test_initial();
    test_jitter_stays_in_band();
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
