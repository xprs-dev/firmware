/* xprs_blob (XBLOB) -- see xprs_blob.h for the shape and the flow. */

#include "xprs_blob.h"
#include <string.h>

/* The codec's one hash seam: mbedtls on the ESP32 (xprs_sha256_idf.c),
 * software on the nRF52 (xprs_sha256_sw.c), and compiled straight into the
 * host test. Declared here so this file needs nothing of xprs.h but the hash. */
void xprs_sha256(const uint8_t *in, size_t len, uint8_t out[32]);

/* One frame per ATT PDU; 244 is MTU-3 at MTU 247. */
#define XB_FRAME_MAX   244
#define XB_HDR         2          /* magic + type */

/* States. */
enum { R_MANIFEST = 0, R_RECV, R_DONE, R_DEAD,
       S_MANIFEST = 10, S_HASHES, S_WAIT_READY, S_BLOCKS, S_WAIT_NEED, S_DEAD };

/* ── little helpers ──────────────────────────────────────────────────────── */
static inline void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void w32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static inline uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t r32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline bool bm_get(const uint8_t *b, uint32_t i) { return (b[i >> 3] >> (i & 7)) & 1; }
static inline void bm_set(uint8_t *b, uint32_t i)   { b[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bm_clr(uint8_t *b, uint32_t i)   { b[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static int hdr(uint8_t *f, uint8_t type) { f[0] = XBLOB_MAGIC; f[1] = type; return XB_HDR; }

static int emit(xblob_t *s, const uint8_t *f, int n)
{
    int rc = s->ops->send(s->ops->ctx, f, n);
    return rc;   /* XBLOB_SEND_OK / _BUSY / <0 */
}

static void fail(xblob_t *s, uint8_t reason, bool tell_peer)
{
    if (tell_peer) { uint8_t f[3]; int o = hdr(f, XBLOB_T_FAIL); f[o++] = reason; s->ops->send(s->ops->ctx, f, o); }
    if (s->ops->done) s->ops->done(s->ops->ctx, false);
    s->state = (s->role == 1) ? S_DEAD : R_DEAD;
}

static void block_hash(const uint8_t *data, int len, uint8_t out[XBLOB_HASHLEN])
{
    uint8_t full[32];
    xprs_sha256(data, (size_t)len, full);
    memcpy(out, full, XBLOB_HASHLEN);
}

/* ── SERVER ────────────────────────────────────────────────────────────────
 * A single pump advances MANIFEST -> HASHES -> BLOCKS -> DONE, stopping the
 * instant the transport says BUSY and resuming from xblob_tx_ready(). The
 * blast is bounded only by the transport's own backpressure -- no ack per
 * block, which is the whole point. */
static void server_pump(xblob_t *s)
{
    uint8_t f[XB_FRAME_MAX];
    int o, rc;

    if (s->state == S_MANIFEST) {
        o = hdr(f, XBLOB_T_MANIFEST);
        f[o++] = XBLOB_VER;
        memcpy(f + o, s->sha, 32); o += 32;
        w32(f + o, s->size); o += 4;
        w16(f + o, s->blksz); o += 2;
        w16(f + o, s->nblocks); o += 2;
        f[o++] = s->hashlen;
        int sl = (int)strlen(s->sig85);
        if (sl > XBLOB_SIG_MAX - 1) sl = XBLOB_SIG_MAX - 1;
        f[o++] = (uint8_t)sl;
        memcpy(f + o, s->sig85, (size_t)sl); o += sl;
        rc = emit(s, f, o);
        if (rc == XBLOB_SEND_BUSY) return;
        if (rc < 0) { fail(s, XBLOB_FAIL_IO, false); return; }
        s->state = S_HASHES; s->hashes_sent = 0;
    }

    if (s->state == S_HASHES) {
        int per = (XB_FRAME_MAX - (XB_HDR + 3)) / s->hashlen;   /* start(2)+count(1) */
        while (s->hashes_sent < s->nblocks) {
            if (s->frames_sent - s->ack >= XBLOB_WINDOW) return;   /* credit spent */
            int count = s->nblocks - s->hashes_sent;
            if (count > per) count = per;
            o = hdr(f, XBLOB_T_HASHES);
            w16(f + o, s->hashes_sent); o += 2;
            f[o++] = (uint8_t)count;
            for (int j = 0; j < count; j++) {
                uint16_t idx = (uint16_t)(s->hashes_sent + j);
                uint8_t blk[XB_FRAME_MAX];
                int want = s->blksz;
                uint32_t off = (uint32_t)idx * s->blksz;
                if (off + want > s->size) want = (int)(s->size - off);
                int got = s->ops->block_read(s->ops->ctx, off, blk, want);
                if (got <= 0) { fail(s, XBLOB_FAIL_IO, true); return; }
                block_hash(blk, got, f + o);
                o += s->hashlen;
            }
            rc = emit(s, f, o);
            if (rc == XBLOB_SEND_BUSY) return;
            if (rc < 0) { fail(s, XBLOB_FAIL_IO, false); return; }
            s->hashes_sent += (uint16_t)count;
            s->frames_sent++;
        }
        /* THE SYNC POINT. Do not blast a single block until the receiver says
         * it holds the manifest and every hash -- otherwise blocks race a lost
         * HASHES frame and are discarded unverifiable at the far end. The
         * receiver answers READY (or START again, and this pass repeats). */
        s->state = S_WAIT_READY;
        return;
    }

    if (s->state == S_BLOCKS) {
        while (s->cursor < s->nblocks) {
            if (!bm_get(s->send_bm, s->cursor)) { s->cursor++; continue; }
            if (s->frames_sent - s->ack >= XBLOB_WINDOW) return;   /* wait for the receiver's ACK */
            uint32_t off = s->cursor * s->blksz;
            int want = s->blksz;
            if (off + want > s->size) want = (int)(s->size - off);
            o = hdr(f, XBLOB_T_BLOCK);
            w16(f + o, (uint16_t)s->cursor); o += 2;
            int got = s->ops->block_read(s->ops->ctx, off, f + o, want);
            if (got <= 0) { fail(s, XBLOB_FAIL_IO, true); return; }
            rc = emit(s, f, o + got);
            if (rc == XBLOB_SEND_BUSY) return;
            if (rc < 0) { fail(s, XBLOB_FAIL_IO, false); return; }
            s->cursor++;
            s->frames_sent++;
        }
        if (!s->done_sent) {
            o = hdr(f, XBLOB_T_DONE);
            rc = emit(s, f, o);
            if (rc == XBLOB_SEND_BUSY) return;
            if (rc < 0) { fail(s, XBLOB_FAIL_IO, false); return; }
            s->done_sent = true;
            s->state = S_WAIT_NEED;
        }
    }
}

void xblob_server_start(xblob_t *s, const xblob_ops_t *ops,
                        const uint8_t sha[32], uint32_t size,
                        uint16_t blksz, const char *sig85, uint32_t now_ms)
{
    (void)now_ms;
    memset(s, 0, sizeof *s);
    s->ops = ops;
    s->role = 1;
    s->state = S_MANIFEST;
    memcpy(s->sha, sha, 32);
    s->size = size;
    s->blksz = blksz;
    s->hashlen = XBLOB_HASHLEN;
    if (sig85) { strncpy(s->sig85, sig85, XBLOB_SIG_MAX - 1); s->sig85[XBLOB_SIG_MAX - 1] = 0; }
    s->nblocks = (uint16_t)((size + blksz - 1) / blksz);
    for (uint32_t i = 0; i < s->nblocks; i++) bm_set(s->send_bm, i);   /* send them all */
    s->cursor = 0;
    server_pump(s);
}

static void server_rx(xblob_t *s, const uint8_t *d, int len)
{
    uint8_t type = d[1];
    if (type == XBLOB_T_ACK) {
        if (len >= XB_HDR + 4) {
            uint32_t a = r32(d + XB_HDR);
            if (a > s->ack) s->ack = a;
            server_pump(s);        /* credit freed -> keep blasting */
        }
        return;
    }
    if (type == XBLOB_T_READY) {
        if (s->state != S_WAIT_READY && s->state != S_BLOCKS) return;
        s->cursor = 0;
        s->done_sent = false;
        s->frames_sent = 0; s->ack = 0;      /* new pass, fresh window */
        s->state = S_BLOCKS;
        server_pump(s);
        return;
    }
    if (type == XBLOB_T_NEED) {
        if (len < XB_HDR + 2) return;
        uint16_t nb = r16(d + XB_HDR);
        const uint8_t *bm = d + XB_HDR + 2;
        int bmbytes = (nb + 7) / 8;
        if (len < XB_HDR + 2 + bmbytes || nb != s->nblocks) return;
        memset(s->send_bm, 0, sizeof s->send_bm);
        for (uint32_t i = 0; i < nb; i++) if (bm_get(bm, i)) bm_set(s->send_bm, i);
        s->cursor = 0;
        s->done_sent = false;
        s->frames_sent = 0; s->ack = 0;   /* the window is per-pass, so drops in one
                                           * pass cannot leak credit into the next */
        s->state = S_BLOCKS;
        server_pump(s);
    } else if (type == XBLOB_T_OK) {
        if (s->ops->done) s->ops->done(s->ops->ctx, true);
        s->state = S_DEAD;
    } else if (type == XBLOB_T_START) {
        /* Receiver restarted (e.g. a HASHES gap): resend everything. */
        memset(s->send_bm, 0, sizeof s->send_bm);
        for (uint32_t i = 0; i < s->nblocks; i++) bm_set(s->send_bm, i);
        s->cursor = 0; s->hashes_sent = 0; s->done_sent = false;
        s->frames_sent = 0; s->ack = 0;      /* a restart is a new pass too */
        s->state = S_MANIFEST;
        server_pump(s);
    } else if (type == XBLOB_T_FAIL || type == XBLOB_T_BYE) {
        if (s->ops->done) s->ops->done(s->ops->ctx, false);
        s->state = S_DEAD;
    }
}

/* ── RECEIVER ──────────────────────────────────────────────────────────────*/

static void send_start(xblob_t *s)
{
    uint8_t f[XB_HDR + 32];
    int o = hdr(f, XBLOB_T_START);
    memcpy(f + o, s->sha, 32); o += 32;
    s->ops->send(s->ops->ctx, f, o);
}

/* Build and send a NEED bitmap of every block not yet held. Returns the count
 * still missing. */
static int send_need(xblob_t *s)
{
    uint8_t f[XB_HDR + 2 + XBLOB_BITMAP_BYTES];
    int o = hdr(f, XBLOB_T_NEED);
    w16(f + o, s->nblocks); o += 2;
    int bmbytes = (s->nblocks + 7) / 8;
    memset(f + o, 0, bmbytes);
    int missing = 0;
    for (uint32_t i = 0; i < s->nblocks; i++)
        if (!bm_get(s->have, i)) { bm_set(f + o, i); missing++; }
    o += bmbytes;
    s->ops->send(s->ops->ctx, f, o);
    return missing;
}

static void send_ack(xblob_t *s);
/* Decide what to ask for. Called on DONE and on the stall timer. */
static void recv_progress(xblob_t *s, uint32_t now_ms)
{
    if (s->state != R_RECV) return;
    s->last_rx_ms = now_ms;

    /* A HASHES frame went missing: we cannot verify those blocks, so ask the
     * server to resend the whole manifest rather than loop forever. */
    if (s->hashes_got < s->nblocks) {
        if (++s->rounds > XBLOB_MAX_ROUNDS) { fail(s, XBLOB_FAIL_ROUNDS, true); return; }
        s->consumed = 0; s->last_ack_sent = 0;
        send_start(s);
        return;
    }
    /* Hashes complete but nothing has arrived yet: the READY (or the server's
     * first blocks) went missing. Say READY again -- idempotent on the server. */
    if (s->got == 0) {
        if (++s->rounds > XBLOB_MAX_ROUNDS) { fail(s, XBLOB_FAIL_ROUNDS, true); return; }
        uint8_t f[XB_HDR]; int o = hdr(f, XBLOB_T_READY);
        s->consumed = 0; s->last_ack_sent = 0;
        s->ops->send(s->ops->ctx, f, o);
        return;
    }

    if (s->got >= s->nblocks) {
        uint8_t f[XB_HDR]; int o = hdr(f, XBLOB_T_OK);
        s->ops->send(s->ops->ctx, f, o);
        s->state = R_DONE;
        if (s->ops->done) s->ops->done(s->ops->ctx, true);
        return;
    }
    if (++s->rounds > XBLOB_MAX_ROUNDS) { fail(s, XBLOB_FAIL_ROUNDS, true); return; }
    s->consumed = 0; s->last_ack_sent = 0;   /* the re-blast is a new pass */
    send_need(s);
}

static void send_ack(xblob_t *s)
{
    uint8_t f[XB_HDR + 4];
    int o = hdr(f, XBLOB_T_ACK);
    w32(f + o, s->consumed); o += 4;
    if (s->ops->send(s->ops->ctx, f, o) == XBLOB_SEND_OK)
        s->last_ack_sent = s->consumed;      /* else the next frame retries */
}
static void maybe_ack(xblob_t *s)
{
    if (s->consumed - s->last_ack_sent >= XBLOB_ACK_EVERY) send_ack(s);
}

static void recv_rx(xblob_t *s, const uint8_t *d, int len, uint32_t now_ms)
{
    uint8_t type = d[1];

    if (type == XBLOB_T_MANIFEST) {
        int o = XB_HDR;
        if (len < o + 1 + 32 + 4 + 2 + 2 + 1 + 1) return;
        o += 1;                                   /* ver */
        if (memcmp(d + o, s->sha, 32) != 0) { fail(s, XBLOB_FAIL_SHA, true); return; }
        o += 32;
        s->size    = r32(d + o); o += 4;
        s->blksz   = r16(d + o); o += 2;
        s->nblocks = r16(d + o); o += 2;
        s->hashlen = d[o++];
        int sl = d[o++];
        if (s->nblocks > XBLOB_MAX_BLOCKS || s->hashlen != XBLOB_HASHLEN) { fail(s, XBLOB_FAIL_BIG, true); return; }
        if (sl > XBLOB_SIG_MAX - 1) sl = XBLOB_SIG_MAX - 1;
        if (len < o + sl) return;
        memcpy(s->sig85, d + o, (size_t)sl); s->sig85[sl] = 0;
        memset(s->have, 0, sizeof s->have);
        memset(s->hbits, 0, sizeof s->hbits);
        s->got = 0; s->hashes_got = 0; s->manifest_ok = true;
        s->consumed = 0; s->last_ack_sent = 0;
        s->state = R_RECV; s->last_rx_ms = now_ms;
        return;
    }

    if (!s->manifest_ok) return;

    if (type == XBLOB_T_HASHES) {
        s->consumed++; maybe_ack(s);        /* pacing counts arrivals, not acceptance */
        s->last_rx_ms = now_ms;
        if (len < XB_HDR + 3) return;
        uint16_t start = r16(d + XB_HDR);
        int count = d[XB_HDR + 2];
        if (start + count > s->nblocks) return;
        if (len < XB_HDR + 3 + count * s->hashlen) return;
        const uint8_t *h = d + XB_HDR + 3;
        for (int j = 0; j < count; j++) {
            uint16_t idx = (uint16_t)(start + j);
            if (!bm_get(s->hbits, idx)) {
                memcpy(s->bhash + (size_t)idx * s->hashlen, h + j * s->hashlen, s->hashlen);
                bm_set(s->hbits, idx);
                s->hashes_got++;
            }
        }
        if (s->hashes_got >= s->nblocks && s->got < s->nblocks) {
            /* Everything verifiable is in hand: cross the sync point. A new
             * pass on both ends, so the window opens clean. */
            uint8_t f[XB_HDR]; int o = hdr(f, XBLOB_T_READY);
            s->consumed = 0; s->last_ack_sent = 0;
            s->ops->send(s->ops->ctx, f, o);
        }
        return;
    }

    if (type == XBLOB_T_BLOCK) {
        s->consumed++; maybe_ack(s);        /* even a discarded block used the link */
        s->last_rx_ms = now_ms;
        if (len < XB_HDR + 2) return;
        uint16_t idx = r16(d + XB_HDR);
        const uint8_t *data = d + XB_HDR + 2;
        int dlen = len - (XB_HDR + 2);
        if (idx >= s->nblocks || dlen <= 0) return;
        if (bm_get(s->have, idx)) return;                    /* already have it */
        if (!bm_get(s->hbits, idx)) return;                  /* no hash to check against yet */
        uint8_t h[XBLOB_HASHLEN];
        block_hash(data, dlen, h);
        if (memcmp(h, s->bhash + (size_t)idx * s->hashlen, s->hashlen) != 0) return;  /* corrupt */
        if (s->ops->block_write(s->ops->ctx, (uint32_t)idx * s->blksz, data, dlen) != 0) return;
        bm_set(s->have, idx);
        s->got++;
        /* Completion is the last block, not the DONE frame: a dropped DONE
         * must not strand a receiver that already holds everything. */
        if (s->got >= s->nblocks && s->hashes_got >= s->nblocks) recv_progress(s, now_ms);
        return;
    }

    if (type == XBLOB_T_DONE) {
        recv_progress(s, now_ms);
        return;
    }
    if (type == XBLOB_T_FAIL || type == XBLOB_T_BYE) {
        if (s->ops->done) s->ops->done(s->ops->ctx, false);
        s->state = R_DEAD;
    }
}

void xblob_recv_start(xblob_t *s, const xblob_ops_t *ops,
                      const uint8_t sha[32], uint32_t size, uint32_t now_ms)
{
    memset(s, 0, sizeof *s);
    s->ops = ops;
    s->role = 0;
    s->state = R_MANIFEST;
    memcpy(s->sha, sha, 32);
    s->size = size;
    s->hashlen = XBLOB_HASHLEN;
    s->last_rx_ms = now_ms;
    send_start(s);
}

/* ── dispatch ──────────────────────────────────────────────────────────────*/
void xblob_rx(xblob_t *s, const uint8_t *d, int len, uint32_t now_ms)
{
    if (!s || len < XB_HDR || d[0] != XBLOB_MAGIC) return;
    if (s->role == 1) server_rx(s, d, len);
    else              recv_rx(s, d, len, now_ms);
}

void xblob_tx_ready(xblob_t *s)
{
    if (s && s->role == 1 && (s->state == S_MANIFEST || s->state == S_HASHES || s->state == S_BLOCKS))
        server_pump(s);
}

void xblob_tick(xblob_t *s, uint32_t now_ms)
{
    if (!s || s->role != 0) return;
    if ((uint32_t)(now_ms - s->last_rx_ms) < XBLOB_STALL_MS) return;
    /* Waiting for the MANIFEST and hearing nothing: the START (one write
     * command on a busy link) can be lost like anything else, so it is
     * retried like everything else -- bounded by the same round count. */
    if (s->state == R_MANIFEST) {
        if (++s->rounds > XBLOB_MAX_ROUNDS) { fail(s, XBLOB_FAIL_ROUNDS, true); return; }
        s->last_rx_ms = now_ms;
        send_start(s);
        return;
    }
    if (s->state != R_RECV || !s->manifest_ok) return;
    if (s->got >= s->nblocks) return;
    recv_progress(s, now_ms);   /* stream stalled -> ask for what is missing */
}

bool xblob_complete(const xblob_t *s)
{
    return s && s->role == 0 && s->manifest_ok && s->got >= s->nblocks;
}
