/* Host test for lr_detect, lr_probe, lr_repeat and lr_worth. Run: sh test_detect_host.sh */
#include <stdio.h>
#include <string.h>

#include "lr_detect.h"
#include "lr_probe.h"
#include "lr_repeat.h"
#include "lr_worth.h"
#include "mt.h"
#include "mc.h"

static int fails, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL %s:%d  %s\n", __func__, __LINE__, what); } } while (0)

static const lrd_timing_t T = { 1200, { 20000, 8000 } };

/* Drive a sweep to the end, feeding [frames]/[echo_at] per network. */
static void run(lrd_t *d, const uint8_t *order, int n, const int frames[LRD_N],
                const int xprs[LRD_N], const int echo_at[LRD_N], uint32_t *t)
{
    lrd_begin(d, order, n, &T, *t);
    uint8_t net = 0;
    int fed[LRD_N] = {0};
    for (int guard = 0; guard < 100000 && d->active; guard++) {
        lrd_act_t a = lrd_tick(d, *t, &net);
        if (a == LRD_PROBE) lrd_probed(d);
        uint8_t cur = lrd_current(d);
        if (d->active && fed[cur] < frames[cur]) {
            lrd_on_frame(d, false, fed[cur] < xprs[cur]);
            fed[cur]++;
        }
        if (d->active && echo_at[cur] && (int)(*t - d->entered_ms) >= echo_at[cur])
            lrd_on_frame(d, true, false);
        *t += 100;
    }
}

static void test_sweep_sequence(void)
{
    lrd_t d; uint32_t t = 1000; uint8_t net = 99;
    const uint8_t order[] = { LRD_MT, LRD_MC };
    lrd_begin(&d, order, 2, &T, t);
    CHECK(lrd_tick(&d, t, &net) == LRD_TUNE && net == LRD_MT, "first stop is tuned first");
    CHECK(lrd_tick(&d, t + 500, &net) == LRD_IDLE, "no probe before the settle time");
    CHECK(lrd_tick(&d, t + 1200, &net) == LRD_PROBE, "probe once settled");
    lrd_probed(&d);
    CHECK(lrd_tick(&d, t + 5000, &net) == LRD_IDLE, "one probe until half the dwell");
    CHECK(lrd_tick(&d, t + 10000, &net) == LRD_PROBE, "second probe at half the dwell");
    lrd_probed(&d);
    lrd_on_frame(&d, false, false);
    CHECK(lrd_tick(&d, t + 15000, &net) == LRD_IDLE, "somebody else's frame does not end the dwell");
    CHECK(lrd_tick(&d, t + 20000, &net) == LRD_TUNE && net == LRD_MC, "dwell over: next stop");
    lrd_on_frame(&d, true, false);
    CHECK(lrd_tick(&d, t + 20100, &net) == LRD_DONE, "an echo ends the dwell early");
    CHECK(d.ev[LRD_MC].relayed && d.ev[LRD_MT].frames == 1, "evidence kept per network");
}

static void test_adopt_boot(void)
{
    lrd_ev_t ev[LRD_N]; memset(ev, 0, sizeof ev);
    lrd_pick_t pk = { LRD_MT, 0, 0 };
    ev[LRD_MC].relayed = true; ev[LRD_MT].frames = 9;
    CHECK(lrd_adopt(&pk, ev, true) == LRD_MC, "boot: a relayed probe beats busy frames");
    memset(ev, 0, sizeof ev);
    pk.current = LRD_MC;
    CHECK(lrd_adopt(&pk, ev, true) == LRD_MC, "boot with nothing heard keeps the last network");
    ev[LRD_MT].xprs = 1; ev[LRD_MT].frames = 1; ev[LRD_MC].frames = 40;
    CHECK(lrd_adopt(&pk, ev, true) == LRD_MT, "XPRS heard beats plain frames");
}

static void test_adopt_hysteresis(void)
{
    lrd_ev_t ev[LRD_N]; memset(ev, 0, sizeof ev);
    lrd_pick_t pk = { LRD_MT, 0, 0 };
    ev[LRD_MC].relayed = true; ev[LRD_MT].frames = 3;
    CHECK(lrd_adopt(&pk, ev, false) == LRD_MT, "one better sweep is not enough");
    CHECK(lrd_adopt(&pk, ev, false) == LRD_MC, "two in a row move");
    memset(ev, 0, sizeof ev);
    ev[LRD_MT].relayed = true;
    CHECK(lrd_adopt(&pk, ev, false) == LRD_MC, "lean toward MT starts over");
    memset(ev, 0, sizeof ev);
    ev[LRD_MC].frames = 1;
    CHECK(lrd_adopt(&pk, ev, false) == LRD_MC && pk.strikes == 0, "an interruption clears the lean");
    memset(ev, 0, sizeof ev);
    CHECK(lrd_adopt(&pk, ev, false) == LRD_MC, "an empty hour moves nothing");
    ev[LRD_MT].relayed = ev[LRD_MC].relayed = true;
    CHECK(lrd_adopt(&pk, ev, false) == LRD_MC, "a tie keeps the network we are on");
}

static void test_run_and_due(void)
{
    lrd_t d; uint32_t t = 0;
    const uint8_t order[] = { LRD_MT, LRD_MC };
    const int frames[LRD_N] = { 3, 0 }, xprs[LRD_N] = { 0, 0 }, echo[LRD_N] = { 7600, 1500 };
    run(&d, order, 2, frames, xprs, echo, &t);
    CHECK(!d.active && d.ev[LRD_MT].relayed && d.ev[LRD_MC].relayed, "both answered");
    CHECK(t < 20000 + 8000, "early ends shorten the sweep");
    CHECK(d.ev[LRD_MT].probes >= 1, "probed MT");
    CHECK(lrd_due(3600000, 0, 3600000, false), "due after the period");
    CHECK(!lrd_due(3600000, 0, 3600000, true), "never while busy");
    CHECK(!lrd_due(100, 0, 3600000, false), "not before");
    CHECK(lrd_due(5, 0xFFFFFFF0u, 20, false), "wraps");
}

static void test_probe_echo(void)
{
    lrp_probe_t pr; memset(&pr, 0, sizeof pr);
    uint8_t f[MT_FRAME_MAX];
    int n = lrp_mt_build(&pr, 0x11223344u, 0xABCD1234u, f, sizeof f);
    CHECK(n > MT_HDR_LEN, "mt probe built");
    CHECK(!lrp_mt_echo(&pr, f, n), "our own frame is not an echo (hop not spent)");
    mt_hdr_t h; mt_hdr_parse(f, n, &h);
    h.hop_limit = (uint8_t)(h.hop_limit - 1); mt_hdr_build(&h, f);
    CHECK(lrp_mt_echo(&pr, f, n), "re-aired with a hop spent is the echo");
    h.id ^= 1; mt_hdr_build(&h, f);
    CHECK(!lrp_mt_echo(&pr, f, n), "another id is not");

    uint8_t g[MC_FRAME_MAX];
    int m = lrp_mc_build(&pr, 0x55667788u, g, sizeof g);
    CHECK(m > 0, "mc probe built");
    CHECK(!lrp_mc_echo(&pr, g, m), "unrelayed mc probe is not an echo");
    mc_pkt_t p; mc_parse(g, m, &p);
    uint8_t hop = 0x42, r[MC_FRAME_MAX];
    int rn = mc_path_append(&p, &hop, r, sizeof r);
    CHECK(rn > 0 && lrp_mc_echo(&pr, r, rn), "relayed mc probe (path grew) is the echo");
}

static void test_worth(void)
{
    const char *m = "t:message f:X1ABCD scope:local m:hi scope:local";
    CHECK(lr_worth(m, (int)strlen(m), false), "a message is worth LoRa");
    const char *o = "t:observation f:X3ABCD hears:X1ABCD";
    CHECK(!lr_worth(o, (int)strlen(o), false), "presence is not, on a shared channel");
    CHECK(lr_worth(o, (int)strlen(o), true), "everything is, on XPRS's own channel");
    CHECK(lr_scope_local(m, (int)strlen(m)), "scope:local found before m:");
    const char *g = "t:message f:X1ABCD m:say scope:local please";
    CHECK(!lr_scope_local(g, (int)strlen(g)), "scope:local inside the text is text");
}

static int mt_frame(uint8_t *f, uint32_t to, uint32_t from, uint32_t id, uint8_t hop)
{
    mt_hdr_t h;
    memset(&h, 0, sizeof h);
    h.to = to; h.from = from; h.id = id;
    h.hop_limit = hop; h.hop_start = 3;
    h.channel = mt_longfast_hash();
    mt_hdr_build(&h, f);
    memset(f + MT_HDR_LEN, 0x5A, 20);
    return MT_HDR_LEN + 20;
}

static void test_repeat_mt(void)
{
    lrr_t r;
    const uint32_t self = 0xA1B2C3D4u;
    lrr_init(&r, LRD_MT, self, NULL);
    uint8_t f[MT_FRAME_MAX];
    int n = mt_frame(f, MT_BROADCAST, 0x1111u, 0x77u, 3);
    lrr_on_frame(&r, f, n, -5, 0, 1000, 3);
    CHECK(r.st.relayed == 1, "mt: a new broadcast with hops left is queued");
    CHECK(lrr_due(&r, 1000) < 0, "mt: not before the CLIENT wait");
    CHECK(lrr_busy(&r, 1000 + 16 * r.slot_ms), "mt: busy when the relay is near");
    int i = lrr_due(&r, 1000 + 16 * r.slot_ms + 3 * r.slot_ms);
    CHECK(i >= 0, "mt: due after 2 x CWmax slots plus its random slots");
    mt_hdr_t h;
    CHECK(i >= 0 && mt_hdr_parse(r.q[i].frame, r.q[i].len, &h) && h.hop_limit == 2 &&
          h.relay_node == (uint8_t)self, "mt: one hop spent, our relay byte");
    lrr_aired(&r, i, false, 2000, 1);
    CHECK(r.q[i].used && lrr_due(&r, 2000) < 0, "mt: a refused air waits a few slots");
    lrr_aired(&r, i, true, 3000, 0);
    CHECK(!r.q[i].used && r.st.aired == 1, "mt: aired");

    n = mt_frame(f, MT_BROADCAST, 0x2222u, 0x88u, 3);
    lrr_on_frame(&r, f, n, -5, 0, 5000, 0);
    lrr_on_frame(&r, f, n, -5, 0, 5100, 0);
    CHECK(r.st.cancelled == 1 && lrr_due(&r, 60000) < 0, "mt: somebody else's copy cancels ours");

    uint32_t before = r.st.relayed;
    n = mt_frame(f, MT_BROADCAST, self, 0x99u, 3);
    lrr_on_frame(&r, f, n, 0, 0, 6000, 0);
    n = mt_frame(f, self, 0x3333u, 0x9Au, 3);
    lrr_on_frame(&r, f, n, 0, 0, 6000, 0);
    n = mt_frame(f, MT_BROADCAST, 0x3333u, 0x9Bu, 0);
    lrr_on_frame(&r, f, n, 0, 0, 6000, 0);
    CHECK(r.st.relayed == before && r.st.skipped == 3, "mt: ours, to us, no hops left: not repeated");

    n = mt_frame(f, MT_BROADCAST, 0x4444u, 0x9Cu, 3);
    lrr_note(&r, f, n, 7000);
    lrr_on_frame(&r, f, n, 0, 0, 7100, 0);
    CHECK(r.st.relayed == before, "mt: a noted XPRS frame is never repeated");

    n = mt_frame(f, MT_BROADCAST, 0x5555u, 0x9Du, 3);
    lrr_on_frame(&r, f, n, 0, 0, 8000, 0);
    CHECK(lrr_due(&r, 8000 + LRR_STALE_MS + 1) < 0, "mt: a relay a minute late is dropped");
}

static void test_repeat_mc(void)
{
    lrr_t r;
    const uint8_t mine[3] = { 0xC7, 0x01, 0x02 };
    lrr_init(&r, LRD_MC, 0, mine);
    uint8_t pl[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, f[MC_FRAME_MAX], g[MC_FRAME_MAX];
    mc_pkt_t p;
    memset(&p, 0, sizeof p);
    p.route = MC_ROUTE_FLOOD; p.type = MC_PT_GRP_TXT; p.hash_size = 1;
    p.payload = pl; p.payload_len = 8;
    p.hops = 1; p.path[0] = 0x42;
    int n = mc_build(&p, f, sizeof f);
    lrr_on_frame(&r, f, n, 0, 400, 1000, 5);
    int i = lrr_due(&r, 1000 + 5 * 416 / 2);
    mc_pkt_t q;
    CHECK(i >= 0 && mc_parse(r.q[i].frame, r.q[i].len, &q) && q.hops == 2 && q.path[1] == 0xC7,
          "mc: a flood goes on with our hash on its path");
    lrr_aired(&r, i, true, 1500, 0);

    p.hops = 2; p.path[1] = 0xC7; pl[0] = 9;
    n = mc_build(&p, g, sizeof g);
    uint32_t before = r.st.relayed;
    lrr_on_frame(&r, g, n, 0, 400, 2000, 0);
    CHECK(r.st.relayed == before, "mc: already through us: the loop check");

    p.route = MC_ROUTE_DIRECT; p.hops = 2; p.path[0] = 0xC7; p.path[1] = 0x33; pl[0] = 10;
    n = mc_build(&p, g, sizeof g);
    lrr_on_frame(&r, g, n, 0, 400, 3000, 0);
    i = lrr_due(&r, 3000);
    CHECK(i >= 0 && mc_parse(r.q[i].frame, r.q[i].len, &q) && q.hops == 1 && q.path[0] == 0x33,
          "mc: a direct packet for us goes on, ourselves taken off its path");
    p.path[0] = 0x55; pl[0] = 11;
    n = mc_build(&p, g, sizeof g);
    before = r.st.relayed;
    lrr_on_frame(&r, g, n, 0, 400, 4000, 0);
    CHECK(r.st.relayed == before, "mc: a direct packet for somebody else is theirs");
}

int main(void)
{
    printf("lr_detect / lr_probe / lr_repeat / lr_worth host tests\n");
    test_sweep_sequence();
    test_adopt_boot();
    test_adopt_hysteresis();
    test_run_and_due();
    test_probe_echo();
    test_worth();
    test_repeat_mt();
    test_repeat_mc();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

/* The ciphers the codecs reference; a probe uses none of them. */
#include <stdbool.h>
#include <stdint.h>
bool xlc_aes_encrypt_block(const uint8_t *key, int key_len, const uint8_t in[16], uint8_t out[16])
{ (void)key; (void)key_len; (void)in; (void)out; return false; }
bool xlc_aes_decrypt_block(const uint8_t *key, int key_len, const uint8_t in[16], uint8_t out[16])
{ (void)key; (void)key_len; (void)in; (void)out; return false; }
