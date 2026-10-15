/*
 * Host test for xprsid_verify against the specification's worked examples
 * (XPRS.md 6.1.2 and 6.1.3): the 60-character short Schnorr and the
 * 80-character BIP-340 form, told apart by the length of sig:.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <openssl/sha.h>

#include "xprs.h"
#include "xprsid.h"
#include "xprssig.h"

/* The codec's hash seam, from OpenSSL: independent of the code under test. */
void xprs_sha256(const uint8_t *in, size_t len, uint8_t out[32]) { SHA256(in, len, out); }

static int g_checks, g_fail;
#define CHECK(cond, fmt, ...) do {                                            \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++;                                                  \
        printf("  FAIL %s:%d  " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); } \
} while (0)

static const char *PX = "5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc";
static const char *SHORT_SIG = "V<-(s&U-xL(hjs8hbML0<8nw[A)a<YeW+5_1BYlWzX.)fQYP&LeI[ZC<n4Yl";
static const char *LONG_SIG  = "QPxg^#t7^xDBU$s1pg.d.u^UZ$B0qYj%c.(I[)FY?%WouV6?)^xS[jf,ftorZ=$fRs>JwXM_GfY&bYO@";
static const char *HEAD = "t:message f:X1QZ3N d:LISBOA ts:2026-08-08_14:26:40";
static const char *TAIL = "m:net starts in ten minutes";

static void unhex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
}

static bool verify_wire(const char *wire, const uint8_t pub[32])
{
    xprs_t p;
    if (!xprs_parse(wire, (int)strlen(wire), &p)) return false;
    return xprsid_verify(&p, pub);
}

static void test_both_forms_of_the_worked_example(void)
{
    uint8_t pub[32];
    unhex(PX, pub, 32);
    char w[400];
    snprintf(w, sizeof w, "%s sig:%s %s", HEAD, SHORT_SIG, TAIL);
    CHECK(verify_wire(w, pub), "the 60-character example does not verify");
    snprintf(w, sizeof w, "%s sig:%s %s", HEAD, LONG_SIG, TAIL);
    CHECK(strlen(w) == 163, "the 80-character example is %zu bytes, not 163", strlen(w));
    CHECK(verify_wire(w, pub), "the 80-character example does not verify");
}

static void test_the_long_form_is_bound_to_the_whole_packet(void)
{
    uint8_t pub[32];
    unhex(PX, pub, 32);
    char w[400];
    snprintf(w, sizeof w, "%s sig:%s m:net starts in nine minutes", HEAD, LONG_SIG);
    CHECK(!verify_wire(w, pub), "a changed body verified");
    snprintf(w, sizeof w, "t:message f:X1QZ3N d:PORTO ts:2026-08-08_14:26:40 sig:%s %s", LONG_SIG, TAIL);
    CHECK(!verify_wire(w, pub), "a changed destination verified");
    snprintf(w, sizeof w, "t:message f:X1QZ3N d:LISBOA ts:2026-08-08_14:26:41 sig:%s %s", LONG_SIG, TAIL);
    CHECK(!verify_wire(w, pub), "a changed time verified");
    snprintf(w, sizeof w, "%s sig:%s via:X3RLY7 %s", HEAD, LONG_SIG, TAIL);
    CHECK(verify_wire(w, pub), "a relayed copy (via: added) no longer verifies");
    uint8_t other[32];
    memcpy(other, pub, 32);
    other[0] ^= 1;
    snprintf(w, sizeof w, "%s sig:%s %s", HEAD, LONG_SIG, TAIL);
    CHECK(!verify_wire(w, other), "another key verified it");
}

static void test_no_ts_cannot_use_the_long_form(void)
{
    uint8_t pub[32];
    unhex(PX, pub, 32);
    char w[400];
    snprintf(w, sizeof w, "t:message f:X1QZ3N d:LISBOA sig:%s %s", LONG_SIG, TAIL);
    CHECK(!verify_wire(w, pub), "a packet with no ts: verified in the 80-character form");
}

static void test_any_other_length_is_not_a_signature(void)
{
    uint8_t pub[32];
    unhex(PX, pub, 32);
    char w[500], sig[100];
    int lens[] = {55, 65, 70, 75, 85, 90};
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        memset(sig, 'K', sizeof sig);
        sig[lens[i]] = 0;
        snprintf(w, sizeof w, "%s sig:%s %s", HEAD, sig, TAIL);
        CHECK(!verify_wire(w, pub), "%d characters verified", lens[i]);
    }
    /* The one that matters: the real 80 with five characters after it must
     * not be read as the 80 with something trailing. */
    snprintf(w, sizeof w, "%s sig:%sKKKKK %s", HEAD, LONG_SIG, TAIL);
    CHECK(!verify_wire(w, pub), "an 85-character value was accepted as its first 80");
}

int main(void)
{
    printf("xprsid host tests (the two signature forms, XPRS.md 6.1.2 and 6.1.3)\n");
    test_both_forms_of_the_worked_example();
    test_the_long_form_is_bound_to_the_whole_packet();
    test_no_ts_cannot_use_the_long_form();
    test_any_other_length_is_not_a_signature();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
