/*
 * test_xblob_host -- host (gcc) test for xprs_blob.c. Two in-memory sessions
 * (server + receiver) wired through bounded queues; a policy layer drops and
 * corrupts frames to exercise the missing/corrupt recovery.
 *
 *   gcc -Wall -Wextra -O1 -I../xprs_codec -o /tmp/test_xblob \
 *       xprs_blob.c ../xprs_codec/xprs_sha256_sw.c test_xblob_host.c && /tmp/test_xblob
 *
 * NOT part of any firmware build.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xprs_blob.h"

static int XB_hdr_ofs(void);
void xprs_sha256(const uint8_t *in, size_t len, uint8_t out[32]);

/* ── a bounded frame queue ─────────────────────────────────────────────── */
#define QCAP 12                 /* small, to exercise BUSY/tx_ready */
typedef struct { uint8_t buf[260]; int len; } frame_t;
typedef struct { frame_t f[QCAP]; int head, count; } queue_t;

static bool q_push(queue_t *q, const uint8_t *d, int n)
{
    if (q->count >= QCAP) return false;      /* full -> caller sees BUSY */
    frame_t *fr = &q->f[(q->head + q->count) % QCAP];
    memcpy(fr->buf, d, n); fr->len = n; q->count++;
    return true;
}
static int q_pop(queue_t *q, uint8_t *out)
{
    if (q->count == 0) return -1;
    frame_t *fr = &q->f[q->head];
    memcpy(out, fr->buf, fr->len);
    int n = fr->len;
    q->head = (q->head + 1) % QCAP; q->count--;
    return n;
}

/* ── the image and the two endpoints ───────────────────────────────────── */
static uint8_t  g_src[200000];
static uint32_t g_size;
static uint8_t  g_dst[256000];      /* receiver STAGE stand-in */
static bool     g_dst_set[256000 / 240 + 4];

static queue_t  q_s2r, q_r2s;
static bool     g_srv_done, g_srv_ok, g_rcv_done, g_rcv_ok;

/* policy */
static int  g_drop_every_block = -1;      /* drop BLOCK idx % this (per delivery) */
static int  g_corrupt_first_pass = 0;     /* corrupt this many blocks on pass 1 */
static bool g_drop_next_done = false;     /* swallow one DONE frame */
static int  g_drop_block_forever = -1;    /* always drop this idx (never converges) */
static int  g_drop_hashes = 0;            /* swallow this many HASHES frames */
static int  g_drop_ready = 0;             /* swallow this many READY frames */
static int  g_delivered_blocks = 0;

static int srv_send(void *c, const uint8_t *f, int n){ (void)c; return q_push(&q_s2r,f,n)?XBLOB_SEND_OK:XBLOB_SEND_BUSY; }
static int rcv_send(void *c, const uint8_t *f, int n){ (void)c; return q_push(&q_r2s,f,n)?XBLOB_SEND_OK:XBLOB_SEND_BUSY; }
static int srv_read(void *c, uint32_t off, uint8_t *dst, int cap){ (void)c; if(off>=g_size)return 0; int n=cap; if(off+n>g_size)n=g_size-off; memcpy(dst,g_src+off,n); return n; }
static int rcv_write(void *c, uint32_t off, const uint8_t *src, int len){ (void)c; memcpy(g_dst+off,src,len); g_dst_set[off/240]=true; return 0; }
static void srv_done(void *c, bool ok){ (void)c; g_srv_done=true; g_srv_ok=ok; }
static void rcv_done(void *c, bool ok){ (void)c; g_rcv_done=true; g_rcv_ok=ok; }

static const xblob_ops_t SRV_OPS = { NULL, srv_send, srv_read, NULL, srv_done };
static const xblob_ops_t RCV_OPS = { NULL, rcv_send, NULL, rcv_write, rcv_done };

/* Deliver one server->receiver frame, applying the loss/corrupt policy. */
static void deliver_s2r(xblob_t *rcv, uint32_t now)
{
    uint8_t d[260];
    int n = q_pop(&q_s2r, d);
    if (n < 0) return;
    if (d[0] == XBLOB_MAGIC && d[1] == 0x02 /*BLOCK*/) {
        int idx = d[2] | (d[3] << 8);
        g_delivered_blocks++;
        if (g_drop_block_forever == idx) return;                 /* black hole */
        if (g_drop_every_block > 0 && (g_delivered_blocks % g_drop_every_block) == 0) return;
        if (g_corrupt_first_pass > 0) { g_corrupt_first_pass--; d[XB_hdr_ofs()] ^= 0xFF; }
    }
    if (d[0] == XBLOB_MAGIC && d[1] == 0x04 /*HASHES*/ && g_drop_hashes > 0) { g_drop_hashes--; return; }
    if (d[0] == XBLOB_MAGIC && d[1] == 0x06 /*DONE*/ && g_drop_next_done) { g_drop_next_done=false; return; }
    xblob_rx(rcv, d, n, now);
}
/* first payload byte offset of a BLOCK = magic+type+idx(2) = 4 */
static int XB_hdr_ofs(void){ return 4; }

static void deliver_r2s(xblob_t *srv, uint32_t now)
{
    uint8_t d[260]; int n = q_pop(&q_r2s, d);
    if (n < 0) return;
    if (d[0] == XBLOB_MAGIC && d[1] == 0x0C /*READY*/ && g_drop_ready > 0) { g_drop_ready--; return; }
    xblob_rx(srv, d, n, now);
}

/* Run one scenario to completion (or timeout). */
static int run(uint32_t size, const char *name)
{
    printf("  %-28s ", name);
    g_size = size;
    for (uint32_t i = 0; i < size; i++) g_src[i] = (uint8_t)(i * 2654435761u >> 13);
    memset(g_dst, 0, sizeof g_dst); memset(g_dst_set, 0, sizeof g_dst_set);
    memset(&q_s2r, 0, sizeof q_s2r); memset(&q_r2s, 0, sizeof q_r2s);
    g_srv_done=g_srv_ok=g_rcv_done=g_rcv_ok=false; g_delivered_blocks=0;
    /* per-scenario policy is set by the caller before run() */

    uint8_t sha[32]; xprs_sha256(g_src, size, sha);

    static xblob_t srv, rcv;
    uint32_t now = 1000;
    xblob_recv_start(&rcv, &RCV_OPS, sha, size, now);          /* sends START */
    /* deliver START to a not-yet-started server: our harness starts the server
     * on the START by hand (the app would look the image up by sha). */
    uint8_t st[64]; int sn = q_pop(&q_r2s, st);
    assert(sn > 0 && st[1] == XBLOB_T_START);
    xblob_server_start(&srv, &SRV_OPS, sha, size, 240, "", now);

    int guard = 0;
    while (!g_rcv_done && guard++ < 2000000) {
        int moved = 0;
        if (q_s2r.count) { deliver_s2r(&rcv, now); xblob_tx_ready(&srv); moved=1; }
        if (q_r2s.count) { deliver_r2s(&srv, now); xblob_tx_ready(&srv); moved=1; }
        if (!moved) {
            /* nothing in flight: advance time so the receiver's stall timer
             * fires and it asks for whatever is missing. */
            now += XBLOB_STALL_MS + 10;
            xblob_tick(&rcv, now);
            if (!q_r2s.count && !q_s2r.count && g_rcv_done) break;
            if (!q_r2s.count && !q_s2r.count) { /* truly stuck */ if (g_rcv_done) break; if (guard>1000000) break; }
        }
    }
    if (!g_rcv_done) { printf("FAIL (timeout)\n"); return 1; }
    return 0;
}

static int expect_ok(uint32_t size, const char *name)
{
    if (run(size, name)) return 1;
    if (!g_rcv_ok) { printf("FAIL (receiver gave up)\n"); return 1; }
    if (memcmp(g_src, g_dst, size) != 0) { printf("FAIL (bytes differ)\n"); return 1; }
    printf("ok\n"); return 0;
}
static int expect_giveup(uint32_t size, const char *name)
{
    if (run(size, name)) return 1;
    if (g_rcv_ok) { printf("FAIL (should have given up)\n"); return 1; }
    printf("ok (gave up as expected)\n"); return 0;
}

int main(void)
{
    int fails = 0;
    printf("xprs_blob host tests\n");

    g_drop_every_block=-1; g_corrupt_first_pass=0; g_drop_next_done=false; g_drop_block_forever=-1; g_drop_hashes=0; g_drop_ready=0;
    fails += expect_ok(162000, "clean 162 KB");

    g_drop_every_block=17; g_corrupt_first_pass=0; g_drop_next_done=false; g_drop_block_forever=-1;
    fails += expect_ok(162000, "drop 1/17 blocks");

    g_drop_every_block=-1; g_corrupt_first_pass=40; g_drop_next_done=false; g_drop_block_forever=-1;
    fails += expect_ok(162000, "corrupt 40 blocks");

    g_drop_every_block=-1; g_corrupt_first_pass=0; g_drop_next_done=true; g_drop_block_forever=-1;
    fails += expect_ok(50000, "dropped DONE -> stall NEED");

    g_drop_every_block=9; g_corrupt_first_pass=25; g_drop_next_done=true; g_drop_block_forever=-1;
    fails += expect_ok(162000, "drops+corrupt+lost DONE");

    g_drop_every_block=-1; g_corrupt_first_pass=0; g_drop_next_done=false; g_drop_block_forever=-1; g_drop_hashes=1; g_drop_ready=0;
    fails += expect_ok(162000, "lost HASHES -> START restart");

    g_drop_every_block=-1; g_corrupt_first_pass=0; g_drop_next_done=false; g_drop_block_forever=-1; g_drop_hashes=0; g_drop_ready=1;
    fails += expect_ok(80000, "lost READY -> re-READY");

    g_drop_every_block=-1; g_corrupt_first_pass=0; g_drop_next_done=false; g_drop_block_forever=5; g_drop_hashes=0; g_drop_ready=0;
    fails += expect_giveup(20000, "one block black-holed");

    /* wrong-sha manifest: start the receiver expecting a different sha. */
    {
        printf("  %-28s ", "wrong-sha manifest rejected");
        g_size=8000; for(uint32_t i=0;i<g_size;i++) g_src[i]=(uint8_t)i;
        memset(&q_s2r,0,sizeof q_s2r); memset(&q_r2s,0,sizeof q_r2s);
        g_srv_done=g_srv_ok=g_rcv_done=g_rcv_ok=false;
        uint8_t real_sha[32], fake_sha[32];
        xprs_sha256(g_src,g_size,real_sha); memcpy(fake_sha,real_sha,32); fake_sha[0]^=0xFF;
        static xblob_t srv,rcv; uint32_t now=1000;
        xblob_recv_start(&rcv,&RCV_OPS,fake_sha,g_size,now);
        uint8_t st[64]; q_pop(&q_r2s,st);
        xblob_server_start(&srv,&SRV_OPS,real_sha,g_size,240,"",now);   /* server offers real sha */
        int guard=0; while(!g_rcv_done && guard++<100000){ if(q_s2r.count) deliver_s2r(&rcv,now); if(q_r2s.count) deliver_r2s(&srv,now); if(!q_s2r.count&&!q_r2s.count) break; }
        if(g_rcv_done && !g_rcv_ok){ printf("ok\n"); } else { printf("FAIL\n"); fails++; }
    }

    printf("%s (%d failing)\n", fails?"FAILED":"all passed", fails);
    return fails ? 1 : 0;
}
