/* Host test for xmailcap.c (run test_xmailcap_host.sh). */
#include <stdio.h>
#include <string.h>

#include "xmailcap.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static int take(xmc_t *c, const char *from, int times, uint32_t hour)
{
    int took = 0;
    for (int i = 0; i < times; i++)
        if (xmc_may(c, from, hour)) { xmc_took(c, from, hour); took++; }
    return took;
}

int main(void)
{
    static xmc_t c;
    memset(&c, 0, sizeof c);

    CHECK(take(&c, "X1SPAM", 100, 7) == XMC_PER_SENDER, "one sender stops at XMC_PER_SENDER");
    CHECK(c.refused_sender == 100 - XMC_PER_SENDER, "the refusals are counted");
    CHECK(take(&c, "X1OTHR", 1, 7) == 1, "somebody else is not held back by it");
    CHECK(take(&c, "x1spam", 1, 7) == 0, "callsigns compare without case");

    CHECK(take(&c, "X1SPAM", 1, 8) == 1, "a new hour starts the counts afresh");
    CHECK(c.refused_sender >= 41, "but keeps the counters");

    /* A crowd of invented callsigns: the total holds, past the table too. */
    memset(&c, 0, sizeof c);
    int took = 0;
    char call[16];
    for (int i = 0; i < 2000; i++) {
        snprintf(call, sizeof call, "X1F%04d", i);
        took += take(&c, call, 1, 9);
    }
    CHECK(took == XMC_ALL, "everybody together stops at XMC_ALL");
    CHECK(c.refused_all == 2000 - XMC_ALL, "and that is counted");
    CHECK(c.n == XMC_SENDERS, "the table stays bounded");

    /* A sender past the table is still limited by the total. */
    memset(&c, 0, sizeof c);
    for (int i = 0; i < XMC_SENDERS; i++) {
        snprintf(call, sizeof call, "X1T%04d", i);
        take(&c, call, 1, 3);
    }
    CHECK(take(&c, "X1LATE", 5, 3) == 5, "a sender the table cannot hold is taken under the total");

    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ok", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
