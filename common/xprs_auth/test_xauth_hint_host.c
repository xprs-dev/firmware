/* Host test for xauth_hint_note (xauth_hint.c): the neighbours' clock hint.
 * Run with test_xauth_hint_host.sh. */
#include <stdio.h>
#include <string.h>
#include "xprs_auth.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main(void)
{
    const uint32_t T = 1795620000u;
    xauth_hint_t r[XAUTH_HINT_N];

    /* Two authors are not enough; a third that agrees is. */
    memset(r, 0, sizeof r);
    CHECK(!xauth_hint_note(r, "X3AAAA", T, 1000), "one author");
    CHECK(!xauth_hint_note(r, "X3BBBB", T + 5, 2000), "two authors");
    uint32_t m = xauth_hint_note(r, "X3CCCC", T + 2, 3000);
    CHECK(m >= T && m <= T + 10, "three agreeing authors give their median, got %u", (unsigned)m);

    /* One author three times is still one author. */
    memset(r, 0, sizeof r);
    xauth_hint_note(r, "X3AAAA", T, 1000);
    xauth_hint_note(r, "X3AAAA", T + 1, 2000);
    CHECK(!xauth_hint_note(r, "X3AAAA", T + 2, 3000), "one author repeating is not three");

    /* A liar far off does not move the median, and does not count. */
    memset(r, 0, sizeof r);
    xauth_hint_note(r, "X3AAAA", T, 1000);
    xauth_hint_note(r, "X3LIAR", T + 86400, 1500);
    CHECK(!xauth_hint_note(r, "X3BBBB", T + 3, 2000), "two honest and a liar are not three agreeing");
    m = xauth_hint_note(r, "X3CCCC", T + 1, 2500);
    CHECK(m >= T && m <= T + 10, "the liar is outvoted, got %u", (unsigned)m);

    /* Samples minutes apart are carried forward by the time since each was
     * heard, so they compare as readings of the same moment. */
    memset(r, 0, sizeof r);
    xauth_hint_note(r, "X3AAAA", T, 0);                    /* heard at 0 s   */
    xauth_hint_note(r, "X3BBBB", T + 240, 240000);         /* heard at 240 s */
    m = xauth_hint_note(r, "X3CCCC", T + 480, 480000);     /* heard at 480 s */
    CHECK(m >= T + 479 && m <= T + 481, "carried forward to now, got %u", (unsigned)(m - T));

    /* Older than ten minutes is forgotten. */
    memset(r, 0, sizeof r);
    xauth_hint_note(r, "X3AAAA", T, 0);
    xauth_hint_note(r, "X3BBBB", T + 1, 1000);
    CHECK(!xauth_hint_note(r, "X3CCCC", T + 700, 700000), "two samples past the window are gone");

    /* A stale carousel among fresh ones: the median is the fresh time. */
    memset(r, 0, sizeof r);
    xauth_hint_note(r, "X3OLD1", T - 300, 1000);
    xauth_hint_note(r, "X3NEW1", T, 1500);
    xauth_hint_note(r, "X3NEW2", T + 1, 2000);
    m = xauth_hint_note(r, "X3NEW3", T + 2, 2500);
    CHECK(m >= T && m <= T + 5, "median of 3 fresh and 1 stale is fresh, got %d", (int)(m - T));

    if (g_fail) { printf("xauth_hint: %d check(s) failed\n", g_fail); return 1; }
    printf("xauth_hint: all checks passed\n");
    return 0;
}
