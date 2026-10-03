/* test_class_host.c -- the classifier's decision, on the host. See
 * lr_class.h. Every frame here is built field by field rather than taken
 * from a capture, so a reader can see WHY each one is what it is. */

#include <stdio.h>
#include <string.h>

#include "lr_class.h"
#include "mc.h"
#include "mt.h"

static int fails;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s\n", (what)); fails++; } \
    else printf("ok: %s\n", (what)); \
} while (0)

/* LongFast's hash on a stock node is 0x08; the clear XPRS channel's is
 * MT_CH_HASH_XPRS. The classifier is handed both rather than computing
 * them, because a hash is a hash of a name and a key. */
static const lr_class_in_t k_in = { .lf_hash = 0x08, .xprs_hash = MT_CH_HASH_XPRS };

/* ── Builders ─────────────────────────────────────────────────────────── */

static int mt_frame(uint8_t *out, uint32_t to, uint32_t from, uint32_t id,
                    uint8_t hop_limit, uint8_t hop_start, uint8_t channel,
                    int payload)
{
    memset(out, 0, (size_t)(MT_HDR_LEN + payload));
    out[0] = (uint8_t)(to & 0xFF);
    out[1] = (uint8_t)((to >> 8) & 0xFF);
    out[2] = (uint8_t)((to >> 16) & 0xFF);
    out[3] = (uint8_t)((to >> 24) & 0xFF);
    out[4] = (uint8_t)(from & 0xFF);
    out[5] = (uint8_t)((from >> 8) & 0xFF);
    out[6] = (uint8_t)((from >> 16) & 0xFF);
    out[7] = (uint8_t)((from >> 24) & 0xFF);
    out[8] = (uint8_t)(id & 0xFF);
    out[9] = (uint8_t)((id >> 8) & 0xFF);
    out[10] = (uint8_t)((id >> 16) & 0xFF);
    out[11] = (uint8_t)((id >> 24) & 0xFF);
    out[12] = (uint8_t)((hop_limit & 0x07) | ((hop_start & 0x07) << 5));
    out[13] = channel;
    for (int i = 0; i < payload; i++) out[MT_HDR_LEN + i] = (uint8_t)(0x40 + i);
    return MT_HDR_LEN + payload;
}

static int mc_frame(uint8_t *out, uint8_t route, uint8_t type, int hops,
                    int hash_size, int payload)
{
    int n = 0;
    out[n++] = (uint8_t)((route & 0x03) | ((type & 0x0F) << 2));
    if (route == MC_ROUTE_TRANSPORT_FLOOD || route == MC_ROUTE_TRANSPORT_DIRECT)
        for (int i = 0; i < 4; i++) out[n++] = 0x11;
    out[n++] = (uint8_t)((hops & 0x3F) | (((hash_size - 1) & 0x03) << 6));
    for (int i = 0; i < hops * hash_size; i++) out[n++] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < payload; i++) out[n++] = (uint8_t)(0x50 + i);
    return n;
}

/* ── The cases ────────────────────────────────────────────────────────── */

static void meshtastic_side(void)
{
    uint8_t f[MT_FRAME_MAX];
    int n;

    n = mt_frame(f, MT_BROADCAST, 0x12345678u, 0xDEADBEEFu, 2, 3, 0x08, 40);
    CHECK(lr_classify(f, n, &k_in) == LR_CLASS_MT,
          "a LongFast broadcast is Meshtastic");
    CHECK(!lr_class_mc_fits(f, n),
          "and MeshCore cannot claim it: 0xFF has version bits 11");

    n = mt_frame(f, 0x9A3C21FFu, 0x12345678u, 1, 0, 3, 0x08, 30);
    CHECK(lr_classify(f, n, &k_in) == LR_CLASS_MT,
          "a LongFast unicast with hops spent is Meshtastic");

    n = mt_frame(f, 0x9A3C21FFu, 0x12345678u, 1, 3, 3, 0x00,
                 1 + MT_PKI_OVERHEAD);
    CHECK(lr_class_mt_fits(f, n, &k_in),
          "channel 0 is a public-key direct message");

    n = mt_frame(f, 0x9A3C21FFu, 0x12345678u, 1, 3, 3, MT_CH_HASH_XPRS, 30);
    CHECK(lr_class_mt_fits(f, n, &k_in),
          "the clear XPRS channel is on this channel too");

    n = mt_frame(f, 0x9A3C21FFu, 0x12345678u, 1, 3, 3, 0x77, 30);
    CHECK(!lr_class_mt_fits(f, n, &k_in),
          "a unicast on a channel nobody here has is not Meshtastic");

    n = mt_frame(f, MT_BROADCAST, 0, 1, 3, 3, 0x08, 30);
    CHECK(!lr_class_mt_fits(f, n, &k_in), "a frame with no sender is nobody's");

    n = mt_frame(f, MT_BROADCAST, 0x12345678u, 1, 5, 3, 0x08, 30);
    CHECK(!lr_class_mt_fits(f, n, &k_in),
          "more hops left than it started with is not a Meshtastic frame");

    n = mt_frame(f, MT_BROADCAST, 0x12345678u, 1, 3, 3, 0x08, 0);
    CHECK(!lr_class_mt_fits(f, n, &k_in),
          "a header with no payload is not traffic");
}

static void meshcore_side(void)
{
    uint8_t f[MC_FRAME_MAX];
    int n;

    n = mc_frame(f, MC_ROUTE_FLOOD, MC_PT_ADVERT, 0, 1, 32 + 4 + 64 + 1 + 7);
    CHECK(lr_classify(f, n, &k_in) == LR_CLASS_MC,
          "a flood advert is MeshCore");

    n = mc_frame(f, MC_ROUTE_FLOOD, MC_PT_ADVERT, 0, 1, 40);
    CHECK(!lr_class_mc_fits(f, n),
          "an advert too short to hold a signature is not one");

    n = mc_frame(f, MC_ROUTE_FLOOD, MC_PT_GRP_TXT, 2, 1, 30);
    CHECK(lr_classify(f, n, &k_in) == LR_CLASS_MC,
          "a channel message two hops along is MeshCore");

    n = mc_frame(f, MC_ROUTE_TRANSPORT_FLOOD, MC_PT_TXT_MSG, 1, 2, 24);
    CHECK(lr_class_mc_fits(f, n),
          "a transport route carries its four extra bytes");

    n = mc_frame(f, MC_ROUTE_DIRECT, MC_PT_ACK, 1, 1, 4);
    CHECK(lr_class_mc_fits(f, n), "a direct ACK is four bytes of checksum");

    n = mc_frame(f, MC_ROUTE_DIRECT, MC_PT_ACK, 1, 1, 2);
    CHECK(!lr_class_mc_fits(f, n), "and three is not an ACK");

    /* 0x0C, 0x0D and 0x0E are not on the air. */
    for (uint8_t t = 0x0C; t <= 0x0E; t++) {
        n = mc_frame(f, MC_ROUTE_FLOOD, t, 0, 1, 20);
        CHECK(!lr_class_mc_fits(f, n), "a payload type that does not exist");
    }

    /* A path longer than the frame: the length byte claims hops the bytes
     * are not there for. This is the check mc_parse already makes and the
     * one that rejects most of the random noise. */
    n = mc_frame(f, MC_ROUTE_FLOOD, MC_PT_GRP_TXT, 4, 1, 20);
    CHECK(!lr_class_mc_fits(f, 6), "a path the frame is too short to hold");

    n = mc_frame(f, MC_ROUTE_FLOOD, MC_PT_GRP_TXT, 40, 2, 10);
    CHECK(!lr_class_mc_fits(f, n), "a path past MC_PATH_MAX");
}

static void neither_side(void)
{
    uint8_t f[64];

    CHECK(lr_classify(NULL, 0, &k_in) == LR_CLASS_NEITHER, "nothing is nobody's");
    CHECK(lr_classify(f, 0, &k_in) == LR_CLASS_NEITHER, "an empty frame");

    memset(f, 0, sizeof f);
    f[0] = 0xC0;                       /* version 3: no MeshCore */
    CHECK(lr_classify(f, 20, &k_in) == LR_CLASS_NEITHER,
          "a version nobody speaks, and no sender either");

    /* An XPRS wire aired bare, as `xprs` mode airs it: a station in `both`
     * mode must not mistake it for either network's frame. The leading `0`
     * of a length-prefixed wire is 0x30, so the version bits are 0 and
     * MeshCore's structure has to be what rejects it. */
    const char *wire = "133 t:message f:X1QZ3N d:X3RLY7 m:hello";
    CHECK(lr_classify((const uint8_t *)wire, (int)strlen(wire), &k_in)
              == LR_CLASS_NEITHER,
          "a bare XPRS wire belongs to neither network");
}

static void ambiguous_side(void)
{
    uint8_t f[MT_FRAME_MAX];

    /* The case the caller has to settle. Built on purpose: a Meshtastic
     * unicast whose `to` ends in a byte with clear version bits, on
     * LongFast's hash, whose remaining lengths happen to add up as a
     * MeshCore channel message. The verdict is EITHER, never a guess. */
    int n = mt_frame(f, 0x9A3C2100u, 0x12345678u, 1, 3, 3, 0x08, 30);
    bool mt = lr_class_mt_fits(f, n, &k_in);
    bool mc = lr_class_mc_fits(f, n);
    if (mt && mc)
        CHECK(lr_classify(f, n, &k_in) == LR_CLASS_EITHER,
              "a frame both structures accept is EITHER, not a guess");
    else
        CHECK(lr_classify(f, n, &k_in) == (mt ? LR_CLASS_MT : LR_CLASS_MC),
              "this one is not ambiguous after all, and says which");

    /* And the property that matters however the bytes fall: a verdict is
     * never both networks at once, so a frame is never handed to two
     * engines that each relay what they cannot read. */
    for (uint32_t to = 0; to < 256; to++) {
        n = mt_frame(f, to, 0x12345678u, 1, 3, 3, 0x08, 30);
        lr_class_t v = lr_classify(f, n, &k_in);
        if (v != LR_CLASS_MT && v != LR_CLASS_MC &&
            v != LR_CLASS_EITHER && v != LR_CLASS_NEITHER) {
            CHECK(false, "a verdict outside the four");
            return;
        }
    }
    CHECK(true, "256 destinations, and every verdict is one of the four");
}

/* The two predicates the caller leans on when both structures fit. */
static void xprs_envelope_side(void)
{
    uint8_t f[MT_FRAME_MAX];
    int n;

    /* XPRS on MeshCore's channel: a flood RAW_CUSTOM whose payload starts
     * with the whole-wire marker 0x00. Its first byte is pinned, so this is
     * the strongest cheap test there is on that side. */
    n = mc_frame(f, MC_ROUTE_FLOOD, MC_PT_RAW_CUSTOM, 0, 1, 40);
    f[2] = 0x00;
    CHECK(lr_class_mc_is_xprs(f, n), "a RAW_CUSTOM is XPRS's own on MeshCore");
    CHECK(!lr_class_mt_is_xprs(f, n, &k_in),
          "and Meshtastic does not claim it as ours");

    /* XPRS on Meshtastic's channel: the clear XPRS channel hash. */
    n = mt_frame(f, 0x9A3C21FFu, 0x12345678u, 1, 3, 3, MT_CH_HASH_XPRS, 30);
    CHECK(lr_class_mt_is_xprs(f, n, &k_in),
          "the XPRS channel hash is XPRS's own on Meshtastic");
    CHECK(!lr_class_mc_is_xprs(f, n),
          "and MeshCore does not claim it as ours");

    /* A frame neither claims: a plain LongFast broadcast. The caller's rule
     * is "exactly one", so two falses must decide nothing. */
    n = mt_frame(f, MT_BROADCAST, 0x12345678u, 1, 3, 3, 0x08, 40);
    CHECK(!lr_class_mt_is_xprs(f, n, &k_in) && !lr_class_mc_is_xprs(f, n),
          "a LongFast broadcast is nobody's XPRS");

    /* And a MeshCore advert: its structure fits, its type is not ours. */
    n = mc_frame(f, MC_ROUTE_FLOOD, MC_PT_ADVERT, 0, 1, 32 + 4 + 64 + 1);
    CHECK(!lr_class_mc_is_xprs(f, n), "an advert is MeshCore's, not ours");
}

int main(void)
{
    printf("-- the classifier --\n");
    meshtastic_side();
    meshcore_side();
    neither_side();
    xprs_envelope_side();
    ambiguous_side();
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
