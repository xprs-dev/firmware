/*
 * xprs_gossip -- see xgossip.h for the shape and why it is that shape.
 *
 * ON DISK. Buckets: g_00.bin .. g_0F.bin, fixed 32-byte records, a callsign
 * hashing to exactly one of them. The point is the lookup: a flat file would
 * make every upsert and every question a read of the whole store, which on a
 * super's card is megabytes for one answer. Sixteen buckets turn that into a
 * sixteenth, and the hash is over the BASE callsign so X1ABC and X1ABC-7 land
 * together and are answered together.
 *
 * A bucket is read whole, worked on in RAM and written back. That is the
 * trade: one 8 KB buffer on the owner task's stack against a seek per record.
 * It bounds a bucket at XG_BUCKET_MAX records, which is what the byte budget
 * is expressed in.
 */
#include "xgossip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef XGOSSIP_HOST_TEST
#include <time.h>
#define XG_LOGI(...) do { } while (0)
#define XG_LOGW(...) do { } while (0)
#define XG_LOGE(fmt, ...) fprintf(stderr, "xgossip: " fmt "\n", ##__VA_ARGS__)
#else
#include "esp_log.h"
static const char *TAG = "xgossip";
#define XG_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define XG_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define XG_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#endif

/*
 * Geometry. Buckets are many and small, and a bucket is NEVER loaded whole:
 * the first version of this read one into a static buffer, which cost 16 KB
 * of .bss per call site and took 48 KB off a board whose free heap is
 * fourteen. Everything below streams the file a record at a time and keeps
 * only the rows for the ONE callsign being worked on -- which the caps bound
 * at XG_PER_CALL, so the working set is a kilobyte of stack whatever the
 * store grows to.
 */
#define XG_BUCKETS      64
#define XG_BUCKET_MAX  256        /* records per bucket; 8 KB of card */
#define XG_PER_CALL     40        /* G + K_SUPER: every row one callsign can
                                   * have in its bucket */
#define XG_QUEUE_LEN    12        /* sightings waiting for the pump */
#define XG_METERS       16        /* per-signer and per-direct meters */

/* 32 bytes. The layout is on the card, so it is not free to change. */
typedef struct {
    char     call[10];
    char     gw[10];
    uint8_t  bearer;              /* xg_bearer index, 0 = unknown */
    uint8_t  layer;               /* 2 visit, 3 live */
    /* two bytes of alignment padding sit here; the uint32s below need it,
     * so the record is 32 bytes without asking for any. */
    uint32_t first_ts;
    uint32_t last_ts;
} xg_rec_t;

_Static_assert(sizeof(xg_rec_t) == 32, "the on-card record is 32 bytes");

/* Bearer names, indexed. Stored as one byte rather than a string because the
 * record is on a card and a super holds a lot of them. Index 0 is unknown;
 * everything from XG_B_RADIO_FIRST to XG_B_RADIO_LAST is short-range, which
 * is what 36.9.4 means by radio truth -- `rns` sits outside it deliberately.
 */
enum { XG_B_UNKNOWN = 0, XG_B_BLE, XG_B_LAN, XG_B_ESPNOW, XG_B_LORA,
       XG_B_WIFI, XG_B_VHF, XG_B_UHF, XG_B_HF, XG_B_RNS, XG_B__COUNT };
#define XG_B_RADIO_FIRST XG_B_BLE
#define XG_B_RADIO_LAST  XG_B_HF

static const char *const k_bearer[XG_B__COUNT] = {
    "", "ble", "lan", "espnow", "lora", "wifi", "vhf", "uhf", "hf", "rns",
};

static uint8_t xg_bearer_of(const char *s)
{
    if (!s || !s[0]) return XG_B_UNKNOWN;
    for (int i = 1; i < XG_B__COUNT; i++)
        if (strcasecmp(k_bearer[i], s) == 0) return (uint8_t)i;
    return XG_B_UNKNOWN;
}

static bool xg_is_radio(uint8_t b)
{
    return b >= XG_B_RADIO_FIRST && b <= XG_B_RADIO_LAST;
}

/* Per-signer meter: one row per observer, oldest evicted. */
typedef struct { char call[10]; uint32_t at; } xg_meter_t;

/* Every row in @p bucket belonging to @p call, with the offsets they sit at.
 * Also reports the bucket's first free slot and its stalest live row, which
 * are what an insert needs when the callsign has no room of its own. */
typedef struct {
    xg_rec_t r[XG_PER_CALL];
    long     off[XG_PER_CALL];
    int      n;
    long     free_off;           /* -1 when the bucket has no free slot */
    long     end_off;            /* where an append would land */
    long     stale_live_off;     /* -1 when the bucket holds no L3 row */
    uint32_t stale_live_ts;
    int      rows;               /* records in the bucket, free slots aside */
} xg_scan_t;

/* One queued sighting: decided on the receive path, written by the pump. */
typedef struct {
    char     call[10];
    char     gw[10];
    uint8_t  bearer;
    bool     radio;               /* may write L2 */
    bool     own;                 /* THIS station heard it, on its own radio */
    uint32_t ts;
} xg_job_t;

struct xgossip_s {
    char     dir[80];
    bool     ready;
    bool     super;
    uint32_t max_bytes;
    int      visit_k;

    xg_job_t queue[XG_QUEUE_LEN];
    uint8_t  q_head, q_count;

    xg_meter_t signer[XG_METERS];
    struct { char key[22]; uint32_t at; } direct[XG_METERS];

    /* The scan working set, HERE and not on the caller's stack.
     *
     * It is about 1.6 KB, and both boards' owner tasks are 8 KB with
     * secp256k1 signing already on them -- docs/esp32.md is explicit that a
     * stack overflow on one of these presents as a reboot loop that, from the
     * network, looks exactly like a station that answers, stops, and answers
     * again. One instance is enough because everything that scans (the pump
     * and the questions) runs on the task that owns the volume; the header
     * says so, and it is the same rule the index keeps. */
    xg_scan_t scan;

    xgossip_stats_t st;
};

/* ── the base callsign, and the bucket it belongs to ─────────────────────── */

/* Section 3.1: X1ABC-7 is X1ABC with an SSID. Gossip is about the STATION,
 * so the suffix is cut before anything is stored or looked up -- otherwise
 * the same operator on two radios is two strangers to the routing. */
static void xg_base(char *out, size_t cap, const char *call)
{
    size_t n = 0;
    for (; call && call[n] && call[n] != '-' && n + 1 < cap; n++)
        out[n] = (char)toupper((unsigned char)call[n]);
    out[n] = 0;
}

static int xg_bucket(const char *base)
{
    uint32_t h = 2166136261u;                    /* FNV-1a */
    for (const char *p = base; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return (int)(h % XG_BUCKETS);
}

static void xg_path(const xgossip_t *g, char *out, size_t cap, int bucket)
{
    snprintf(out, cap, "%s/g_%02X.bin", g->dir, bucket);
}

/* ── a bucket, streamed ─────────────────────────────────────────────────── */

/*
 * A record whose call[0] is 0 is a FREE SLOT, not a record. Rows are removed
 * by writing one of those over them rather than by rewriting the file: a
 * bucket is on a card, the caller is a station with radios waiting, and
 * shuffling 8 KB to drop one sighting is the kind of write that shows up as a
 * missed packet (docs/esp32.md). Free slots are reused by the next insert.
 */
static bool xg_slot_free(const xg_rec_t *r) { return r->call[0] == 0; }

static void xg_scan(const xgossip_t *g, int bucket, const char *call,
                    xg_scan_t *sc)
{
    memset(sc, 0, sizeof *sc);
    sc->free_off = -1;
    sc->stale_live_off = -1;
    sc->stale_live_ts = 0xFFFFFFFFu;

    char path[112];
    xg_path(g, path, sizeof path, bucket);
    FILE *f = fopen(path, "rb");
    if (!f) return;

    xg_rec_t r;
    long off = 0;
    while (fread(&r, sizeof r, 1, f) == 1) {
        if (xg_slot_free(&r)) {
            if (sc->free_off < 0) sc->free_off = off;
        } else {
            sc->rows++;
            if (r.layer == 3 && r.last_ts < sc->stale_live_ts) {
                sc->stale_live_ts = r.last_ts;
                sc->stale_live_off = off;
            }
            if (call && strcasecmp(r.call, call) == 0 && sc->n < XG_PER_CALL) {
                sc->r[sc->n] = r;
                sc->off[sc->n] = off;
                sc->n++;
            }
        }
        off += (long)sizeof r;
    }
    sc->end_off = off;
    fclose(f);
}

/* One record, at one offset. The only way anything reaches the card. */
static bool xg_put(const xgossip_t *g, int bucket, long off, const xg_rec_t *r)
{
    char path[112];
    xg_path(g, path, sizeof path, bucket);
    FILE *f = fopen(path, "r+b");
    if (!f) f = fopen(path, "w+b");
    if (!f) {
        /* Loud: a bucket that will not write is gossip answering from a table
         * nobody is updating, which looks exactly like a quiet network. */
        XG_LOGE("bucket %s not written: %s", path, strerror(errno));
        return false;
    }
    bool ok = fseek(f, off, SEEK_SET) == 0 && fwrite(r, sizeof *r, 1, f) == 1;
    fflush(f);
    fclose(f);
    return ok;
}

static void xg_erase(const xgossip_t *g, int bucket, long off)
{
    xg_rec_t empty;
    memset(&empty, 0, sizeof empty);
    xg_put(g, bucket, off, &empty);
}

/* ── the walls ───────────────────────────────────────────────────────────── */

static uint32_t *xg_meter(xg_meter_t *tab, const char *key)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < XG_METERS; i++) {
        if (strcasecmp(tab[i].call, key) == 0) return &tab[i].at;
        if (!tab[i].call[0]) { slot = i; break; }
        if (tab[i].at < tab[oldest].at) oldest = i;
    }
    if (slot < 0) slot = oldest;
    snprintf(tab[slot].call, 10, "%s", key);
    tab[slot].at = 0;
    return &tab[slot].at;
}

/* ── queueing (receive path: RAM only) ───────────────────────────────────── */

static void xg_queue(xgossip_t *g, const char *call, const char *gw,
                     uint8_t bearer, bool radio, bool own, uint32_t now_s)
{
    if (g->q_count >= XG_QUEUE_LEN) {
        g->st.dropped++;
        return;
    }
    const int w = (g->q_head + g->q_count) % XG_QUEUE_LEN;
    xg_base(g->queue[w].call, sizeof g->queue[w].call, call);
    xg_base(g->queue[w].gw, sizeof g->queue[w].gw, gw);
    g->queue[w].bearer = bearer;
    g->queue[w].radio = radio;
    g->queue[w].own = own;
    g->queue[w].ts = now_s;
    g->q_count++;
    g->st.queued = g->q_count;
}

/* ── the pump: one queued sighting onto the card ─────────────────────────── */

static void xg_apply(xgossip_t *g, const xg_job_t *j)
{
    const int bucket = xg_bucket(j->call);
    xg_scan_t *sc = &g->scan;
    xg_scan(g, bucket, j->call, sc);

    /* An ordinary station admits a callsign only if it has a reason to know
     * it. Hearing it on our OWN radio is that reason, and it is how a
     * callsign becomes known in the first place -- after which what other
     * stations say about it is worth keeping too. */
    if (!g->super && !j->own && sc->n == 0) {
        g->st.refused_need++;
        return;
    }

    /* TTL, on the rows of this callsign only: the sweep rides the insert, so
     * an idle station does no work at all, and the work it does do is bounded
     * by one callsign rather than by the store. */
    for (int i = 0; i < sc->n; i++) {
        if (sc->r[i].layer != 3) continue;
        if (j->ts <= sc->r[i].last_ts) continue;
        if (j->ts - sc->r[i].last_ts <= XGOSSIP_LIVE_TTL_SEC) continue;
        xg_erase(g, bucket, sc->off[i]);
        sc->r[i].call[0] = 0;                 /* gone from this pass too */
    }

    for (int layer = 3; layer >= 2; layer--) {
        if (layer == 2 && !j->radio) continue;

        int found = -1, held = 0, stalest = -1;
        for (int i = 0; i < sc->n; i++) {
            if (xg_slot_free(&sc->r[i]) || sc->r[i].layer != layer) continue;
            held++;
            if (stalest < 0 || sc->r[i].last_ts < sc->r[stalest].last_ts) stalest = i;
            if (strcasecmp(sc->r[i].gw, j->gw) == 0) found = i;
        }

        if (found >= 0) {
            if (j->ts > sc->r[found].last_ts) sc->r[found].last_ts = j->ts;
            sc->r[found].bearer = j->bearer;
            xg_put(g, bucket, sc->off[found], &sc->r[found]);
            continue;
        }

        /* The per-callsign cap (36.9.4's K and G): a new gateway takes the
         * stalest one's place rather than being turned away, because the
         * newest sighting is the one routing wants. */
        const int cap = layer == 3 ? XGOSSIP_LIVE_G : g->visit_k;
        long off = -1;
        if (held >= cap && stalest >= 0) {
            off = sc->off[stalest];
        } else if (sc->free_off >= 0) {
            off = sc->free_off;
            sc->free_off = -1;                /* one insert, one slot */
        } else if (sc->rows < XG_BUCKET_MAX) {
            off = sc->end_off;
            sc->end_off += (long)sizeof(xg_rec_t);
            sc->rows++;
        } else if (sc->stale_live_off >= 0) {
            /* The bucket itself is full. L3 pays: the visit history is the
             * layer 36.9.4 allows to live forever. */
            off = sc->stale_live_off;
            sc->stale_live_off = -1;
        } else {
            continue;                        /* all visits: nothing to give */
        }

        xg_rec_t r;
        memset(&r, 0, sizeof r);
        snprintf(r.call, sizeof r.call, "%s", j->call);
        snprintf(r.gw, sizeof r.gw, "%s", j->gw);
        r.bearer = j->bearer;
        r.layer = (uint8_t)layer;
        r.first_ts = j->ts;
        r.last_ts = j->ts;
        if (xg_put(g, bucket, off, &r) && g->st.rows < 4) {
            /* The first few, out loud. A store that is quietly writing
             * nothing looks exactly like a quiet network, and that is the
             * failure this component is most likely to have. */
            XG_LOGI("first sightings: %s seen by %s on %s (L%d)",
                    r.call, r.gw, k_bearer[r.bearer < XG_B__COUNT ? r.bearer : 0],
                    layer);
        }
        if (g->st.rows < 0xFFFFFFFFu) g->st.rows++;
    }
}

void xgossip_pump(xgossip_t *g)
{
    if (!g || !g->ready) return;
    while (g->q_count) {
        const xg_job_t job = g->queue[g->q_head];
        g->q_head = (uint8_t)((g->q_head + 1) % XG_QUEUE_LEN);
        g->q_count--;
        xg_apply(g, &job);
    }
    g->st.queued = 0;
}

/* ── the feeds ───────────────────────────────────────────────────────────── */

void xgossip_note_direct(xgossip_t *g, const char *call, const char *self,
                         const char *bearer, uint32_t now_s)
{
    if (!g || !g->ready || !call || !*call || !self || !*self) return;
    char cb[10], sb[10];
    xg_base(cb, sizeof cb, call);
    xg_base(sb, sizeof sb, self);
    if (!cb[0] || strcasecmp(cb, sb) == 0) return;

    /* Every beacon from every neighbour lands here, several a second on a
     * busy bench, and the table's answer does not change by the beacon. */
    char key[22];
    snprintf(key, sizeof key, "%s|%s", cb, bearer ? bearer : "");
    int slot = -1, oldest = 0;
    for (int i = 0; i < XG_METERS; i++) {
        if (strcmp(g->direct[i].key, key) == 0) { slot = i; break; }
        if (!g->direct[i].key[0]) { slot = i; g->direct[i].at = 0; break; }
        if (g->direct[i].at < g->direct[oldest].at) oldest = i;
    }
    if (slot < 0) { slot = oldest; g->direct[slot].at = 0; }
    if (g->direct[slot].key[0] && g->direct[slot].at &&
        now_s - g->direct[slot].at < XGOSSIP_DIRECT_SEC) return;
    snprintf(g->direct[slot].key, sizeof g->direct[slot].key, "%s", key);
    g->direct[slot].at = now_s;

    const uint8_t b = xg_bearer_of(bearer);
    xg_queue(g, cb, sb, b, xg_is_radio(b), true, now_s);
    g->st.accepted++;
}

bool xgossip_would_accept(xgossip_t *g, const char *observer, uint32_t now_s)
{
    if (!g || !g->ready || !observer || !*observer) return false;
    char ob[10];
    xg_base(ob, sizeof ob, observer);
    for (int i = 0; i < XG_METERS; i++)
        if (strcasecmp(g->signer[i].call, ob) == 0)
            return now_s - g->signer[i].at >= XGOSSIP_SIGNER_SEC;
    return true;
}

void xgossip_note_hears(xgossip_t *g, const char *observer,
                        const char *const *hears, int n_hears,
                        const char *link, bool verified, uint32_t now_s)
{
    if (!g || !g->ready || !observer || !*observer || n_hears <= 0) return;
    if (!verified) {
        /* An unsigned claim about who is where is exactly what an attacker
         * would send, and it is the cheapest thing in the world to send. */
        g->st.refused_unsigned++;
        return;
    }
    char ob[10];
    xg_base(ob, sizeof ob, observer);
    uint32_t *at = xg_meter(g->signer, ob);
    if (*at && now_s - *at < XGOSSIP_SIGNER_SEC) {
        g->st.refused_quota++;
        return;
    }
    *at = now_s;

    const uint8_t b = xg_bearer_of(link);
    const bool radio = xg_is_radio(b);
    for (int i = 0; i < n_hears; i++) {
        char cb[10];
        xg_base(cb, sizeof cb, hears[i]);
        if (!cb[0] || strcasecmp(cb, ob) == 0) continue;
        xg_queue(g, cb, ob, b, radio, false, now_s);
    }
    g->st.accepted++;
}

/* ── the questions ───────────────────────────────────────────────────────── */

int xgossip_where_is(xgossip_t *g, const char *call,
                     xgossip_sighting_t *out, int max)
{
    if (!g || !g->ready || !call || !out || max <= 0) return 0;
    char cb[10];
    xg_base(cb, sizeof cb, call);
    xg_scan_t *sc = &g->scan;
    xg_scan(g, xg_bucket(cb), cb, sc);

    int w = 0;
    /* L3 first, then L2: a live sighting outranks a place somebody used to
     * be, and within a layer the freshest wins. */
    for (int layer = 3; layer >= 2 && w < max; layer--) {
        for (;;) {
            int best = -1;
            for (int i = 0; i < sc->n; i++) {
                if (xg_slot_free(&sc->r[i]) || sc->r[i].layer != layer) continue;
                bool taken = false;
                for (int k = 0; k < w; k++)
                    if (out[k].layer == layer &&
                        strcasecmp(out[k].gw, sc->r[i].gw) == 0) taken = true;
                if (taken) continue;
                if (best < 0 || sc->r[i].last_ts > sc->r[best].last_ts) best = i;
            }
            if (best < 0 || w >= max) break;
            snprintf(out[w].call, sizeof out[w].call, "%s", sc->r[best].call);
            snprintf(out[w].gw, sizeof out[w].gw, "%s", sc->r[best].gw);
            snprintf(out[w].bearer, sizeof out[w].bearer, "%s",
                     k_bearer[sc->r[best].bearer < XG_B__COUNT
                              ? sc->r[best].bearer : 0]);
            out[w].ts = sc->r[best].last_ts;
            out[w].layer = sc->r[best].layer;
            w++;
        }
    }
    return w;
}

int xgossip_try_candidates(xgossip_t *g, const char *call, const char *self,
                           char *out, int cap)
{
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    xgossip_sighting_t s[6];
    const int n = xgossip_where_is(g, call, s, 6);
    char sb[10];
    xg_base(sb, sizeof sb, self ? self : "");
    int w = 0;
    for (int i = 0; i < n; i++) {
        /* Never name ourselves: the asker just asked US and got a 404. */
        if (strcasecmp(s[i].gw, sb) == 0) continue;
        bool dup = false;
        for (int k = 0; k < i; k++)
            if (strcasecmp(s[k].gw, s[i].gw) == 0) dup = true;
        if (dup) continue;
        const int need = (int)strlen(s[i].gw) + (w ? 1 : 0);
        if (w + need >= cap) break;
        if (w) out[w++] = ',';
        w += snprintf(out + w, (size_t)(cap - w), "%s", s[i].gw);
    }
    return w;
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

void xgossip_set_super(xgossip_t *g, bool super)
{
    if (!g) return;
    g->super = super;
    g->max_bytes = super ? XGOSSIP_MAX_BYTES_SUPER : XGOSSIP_MAX_BYTES;
    g->visit_k = super ? XGOSSIP_VISIT_K_SUPER : XGOSSIP_VISIT_K;
    XG_LOGI("gossip: %s, %u KB, K=%d",
            super ? "super (every callsign it can learn of)"
                  : "need-to-know", (unsigned)(g->max_bytes / 1024),
            g->visit_k);
}

xgossip_t *xgossip_open(const char *dir)
{
    if (!dir || !*dir) return NULL;
    xgossip_t *g = (xgossip_t *)calloc(1, sizeof *g);
    if (!g) return NULL;
    snprintf(g->dir, sizeof g->dir, "%s", dir);
    /*
     * Make the directory if it will take one, and then PROVE the store can
     * write before claiming to be open.
     *
     * Both halves were paid for on the bench. Leaving the directory to the
     * caller meant every bucket write failed with ENOENT while open()
     * reported success, so the station looked like it simply had no gossip
     * yet. And mkdir is not something to rely on: on the T-Deck's internal
     * flash FAT it comes back EACCES, and the archive beside it only works
     * because its own directories predate the attempt. So the caller is
     * expected to hand over a directory that already exists -- the one the
     * archive lives in -- and this checks rather than assumes.
     */
    mkdir(g->dir, 0777);
    char probe[112];
    snprintf(probe, sizeof probe, "%s/g_probe.tmp", g->dir);
    FILE *pf = fopen(probe, "wb");
    if (!pf) {
        XG_LOGE("cannot write in %s: %s -- gossip will not be kept",
                g->dir, strerror(errno));
        free(g);
        return NULL;
    }
    fclose(pf);
    remove(probe);
    g->ready = true;
    xgossip_set_super(g, false);

    uint32_t rows = 0;
    for (int b = 0; b < XG_BUCKETS; b++) {
        xg_scan(g, b, NULL, &g->scan);
        rows += (uint32_t)g->scan.rows;
    }
    g->st.rows = rows;
    g->st.bytes = rows * (uint32_t)sizeof(xg_rec_t);
    XG_LOGI("gossip open %s: %u sighting(s)", dir, (unsigned)rows);
    return g;
}

void xgossip_close(xgossip_t *g)
{
    if (!g) return;
    xgossip_pump(g);
    free(g);
}

void xgossip_stats(xgossip_t *g, xgossip_stats_t *out)
{
    if (!g || !out) return;
    *out = g->st;
    out->bytes = g->st.rows * (uint32_t)sizeof(xg_rec_t);
}
