/* Host test for xmb.c on a RAM flash that behaves like the nRF52's: an
 * erase sets 0xFF, a write can only clear bits, and a word written more
 * than twice between erases is a failure. Run: sh test_xmb_host.sh */
#include <stdio.h>
#include <string.h>

#include "xmb.h"
#include "xprs.h"

static int fails, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL %s:%d  %s\n", __func__, __LINE__, what); } } while (0)

#define PAGES 6
static uint8_t  flash[PAGES * XMB_PAGE];
static uint8_t  wcount[PAGES * XMB_PAGE / 4];
static int      overwrites, erases;

static bool f_read(void *c, uint32_t off, void *buf, int len)
{ (void)c; memcpy(buf, flash + off, (size_t)len); return true; }
static bool f_write(void *c, uint32_t off, const void *buf, int len)
{
    (void)c;
    if (off % 4 || len % 4) return false;
    const uint8_t *b = buf;
    for (int i = 0; i < len; i++) flash[off + i] &= b[i];
    for (int w = 0; w < len / 4; w++) if (++wcount[off / 4 + w] > 2) overwrites++;
    return true;
}
static bool f_erase(void *c, int pg)
{
    (void)c;
    memset(flash + pg * XMB_PAGE, 0xFF, XMB_PAGE);
    memset(wcount + pg * XMB_PAGE / 4, 0, XMB_PAGE / 4);
    erases++;
    return true;
}
static const xmb_store_t ST = { PAGES, f_read, f_write, f_erase, NULL };

static int mail(char *w, const char *from, const char *to, int i, const char *extra)
{
    return snprintf(w, 250, "t:message f:%s d:%s ts:2026-10-07_12:%02d:%02d m:letter %d%s",
                    from, to, i / 60 % 60, i % 60, i, extra ? extra : "");
}

static void test_basic(void)
{
    memset(flash, 0x00, sizeof flash);          /* somebody else's bytes */
    xmb_t m;
    CHECK(xmb_open(&m, &ST), "open formats a foreign area");
    CHECK(m.live == 0 && xmb_capacity(&m) == 64, "empty, 64 records");
    char w[256];
    int n = mail(w, "X1AAAA", "X1BBBB", 1, NULL);
    CHECK(xmb_class(&m, w, n, "X2CARD", false, 0) == 0, "a stranger's mail is not carried");
    CHECK(xmb_class(&m, w, n, "X2CARD", true, 0) == 2, "a contact's mail is custody");
    CHECK(xmb_class(&m, w, n, "X1BBBB", true, 0) == 0, "our own mail is not held");
    CHECK(xmb_hold(&m, w, n, 2, 0) >= 0, "held");
    CHECK(xmb_hold(&m, w, n, 2, 0) < 0, "a duplicate is not held twice");
    const char *d = "t:mailbox f:X1BBBB ts:2026-10-07_12:00:00 hold:X3HOME,X2CARD sig:x";
    CHECK(xmb_decl(&m, d, (int)strlen(d), "X2CARD", 0), "a declaration naming us is kept");
    n = mail(w, "X1CCCC", "X1BBBB", 2, NULL);
    CHECK(xmb_class(&m, w, n, "X2CARD", false, 0) == 3, "now its mail is class 3");
    CHECK(xmb_hold(&m, w, n, 3, 0) >= 0, "held as class 3");
    CHECK(xmb_count(&m, "X1BBBB") == 2 && xmb_count(&m, NULL) == 2, "counted");

    int out[3];
    int k = xmb_due(&m, "X1BBBB", 1000, 0, out, 3);
    CHECK(k == 2, "both due on sighting");
    char r[256];
    CHECK(k == 2 && xmb_read(&m, out[0], r, sizeof r) > 0 && strstr(r, "letter 2"), "newest first");
    for (int i = 0; i < k; i++) xmb_aired(&m, out[i], 1000);
    CHECK(xmb_due(&m, "X1BBBB", 20000, 0, out, 3) == 0, "backed off after airing");
    CHECK(xmb_due(&m, "X1BBBB", 31001, 0, out, 3) == 2, "due again after 30 s");

    char id[8];
    n = mail(w, "X1AAAA", "X1BBBB", 1, NULL);
    xprs_id_of(w, n, id);
    CHECK(!xmb_receipt(&m, id, "X1ZZZZ"), "a receipt from somebody else releases nothing");
    CHECK(xmb_receipt(&m, id, "X1BBBB"), "the recipient's receipt releases it");

    xmb_t again;
    CHECK(xmb_open(&again, &ST) && xmb_count(&again, "X1BBBB") == 1, "the release survives a reboot");
    n = mail(w, "X1CCCC", "X1BBBB", 2, NULL);
    CHECK(xmb_class(&again, w, n, "X2CARD", false, 0) == 3, "so does the declaration");

    char rm[160];
    xprs_id_of(d, (int)strlen(d), id);
    snprintf(rm, sizeof rm, "t:mailbox f:X1BBBB ts:2026-10-07_13:00:00 r:%s remove:mailbox sig:x", id);
    CHECK(xmb_decl(&again, rm, (int)strlen(rm), "X2CARD", 0), "a removal kills it");
    CHECK(xmb_class(&again, w, n, "X2CARD", false, 0) == 0, "and its mail is plain again");
    CHECK(overwrites == 0, "no word written more than twice");
}

static void test_full_and_rotation(void)
{
    memset(flash, 0xFF, sizeof flash);
    memset(wcount, 0, sizeof wcount);
    xmb_t m;
    CHECK(xmb_open(&m, &ST), "open blank");
    char w[256];
    int held = 0;
    /* Class 3 for one recipient (declared), class 2 for many others. */
    const char *d = "t:mailbox f:X1KEEP ts:2026-10-07_12:00:00 hold:X2CARD sig:x";
    xmb_decl(&m, d, (int)strlen(d), "X2CARD", 0);
    for (int i = 0; i < 8; i++) {
        int n = mail(w, "X1SEND", "X1KEEP", i, NULL);
        if (xmb_hold(&m, w, n, 3, 0) >= 0) held++;
    }
    for (int i = 0; i < 300; i++) {
        char to[8];
        snprintf(to, sizeof to, "X1T%03d", i % 40);
        char from[8];
        snprintf(from, sizeof from, "X1F%03d", i % 30);
        int n = mail(w, from, to, 100 + i, " pad pad pad pad pad pad pad pad pad pad pad");
        if (xmb_hold(&m, w, n, 2, 0) >= 0) held++;
    }
    CHECK(m.live <= xmb_capacity(&m), "never above capacity");
    CHECK(xmb_count(&m, "X1KEEP") == 8, "class 3 survives a flood of class 2");
    CHECK(m.stats.evicted > 200 && m.stats.relocated > 0 && overwrites == 0, "evicted, relocated, clean");
    xmb_t again;
    CHECK(xmb_open(&again, &ST), "reopen after many rotations");
    CHECK(again.live == m.live && xmb_count(&again, "X1KEEP") == 8, "the same records after reboot");
    int n = mail(w, "X1NEW1", "X1NEW2", 999, NULL);
    CHECK(xmb_hold(&again, w, n, 2, 0) >= 0, "and it keeps taking mail");

    /* Class 2 cannot push out class 3. */
    xmb_t full;
    memset(flash, 0xFF, sizeof flash);
    memset(wcount, 0, sizeof wcount);
    xmb_open(&full, &ST);
    for (int i = 0; i < 64; i++) {
        char to[8];
        char from[8];
        snprintf(to, sizeof to, "X1K%03d", i / 9);
        snprintf(from, sizeof from, "X1S%03d", i / 16);
        n = mail(w, from, to, i, NULL);
        xmb_hold(&full, w, n, 3, 0);
    }
    n = mail(w, "X1S2", "X1OTHER", 500, NULL);
    CHECK(xmb_hold(&full, w, n, 2, 0) < 0, "custody refused when only declared mail is left");
    CHECK(xmb_count(&full, NULL) == 64, "nothing declared was lost");
}

static void test_state_and_expiry(void)
{
    memset(flash, 0xFF, sizeof flash);
    memset(wcount, 0, sizeof wcount);
    xmb_t m;
    xmb_open(&m, &ST);
    uint8_t b[16] = { 1, 2, 3 }, g[16];
    CHECK(xmb_state_put(&m, b, 3) && xmb_state_get(&m, g, sizeof g) == 3 && g[2] == 3, "state kept");
    int before = m.live;
    CHECK(xmb_state_put(&m, b, 3) && m.live == before, "an unchanged state is not rewritten");
    b[0] = 9;
    xmb_state_put(&m, b, 3);
    xmb_t again;
    xmb_open(&again, &ST);
    CHECK(xmb_state_get(&again, g, sizeof g) == 3 && g[0] == 9 && again.live == 1, "one state, the newest");

    char w[256];
    int n = snprintf(w, sizeof w, "t:message f:X1A d:X1B ts:2026-10-07_12:00:00 until:2026-10-08_12:00:00 m:x");
    CHECK(xmb_hold(&again, w, n, 2, 1791381600u) >= 0, "held before until:");
    xmb_expire(&again, 1791468001u);
    CHECK(xmb_count(&again, "X1B") == 0, "gone after until:");
    n = snprintf(w, sizeof w, "t:message f:X1A d:X1B ts:2026-10-07_12:00:00 m:y");
    xmb_hold(&again, w, n, 2, 0);
    xmb_expire(&again, 1791381600u + 7 * 86400u + 1);
    CHECK(xmb_count(&again, "X1B") == 0, "a week without until:");
}

int main(void)
{
    printf("xprs_mailbox host tests\n");
    test_basic();
    test_full_and_rotation();
    test_state_and_expiry();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
