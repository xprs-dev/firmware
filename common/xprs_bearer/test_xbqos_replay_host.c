/*
 * The QoS harness (spec QoS.md section 8), firmware side: the bench trace
 * replayed through the real bearer -- xb_on_wire and xb_digipeat for what
 * LoRa heard, xb_offer for what the short bearers heard, xb_send_ex for the
 * owner, the pump for the rest -- onto a simulated Meshtastic LongFast
 * channel, and the pass criteria checked on what went out. The Dart twin is
 * core test/xprs_qos_replay_test.dart; the trace is the same file.
 *
 * Input on stdin (test_xbqos_replay_host.sh makes it from the .jsonl.gz):
 *   #start <utc ms>
 *   <t ms>\t<bearer>\t<rssi>\t<wire>
 * Argument: the speed (1 = as recorded, 4 = the hour compressed four times).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xprsbearer.h"
#include "xb_airtime.h"
#include "xb_qos.h"

#define SELF "X3SEP2"
#define BUDGET 360000u
#define MAXP 8000
#define MAXTX 4000
#define MAXV 64

static const xb_lora_air_t k_lf = { .bw_hz = 250000, .sf = 11, .cr = 1,
                                    .preamble = 16, .crc = true };
static uint32_t lf_ms(int len, void *c)
{
    (void)c;
    if (len <= 220) return xb_lora_airtime_ms(&k_lf, len + 16);
    return xb_lora_airtime_ms(&k_lf, 236) + xb_lora_airtime_ms(&k_lf, len - 220 + 16);
}

typedef struct { uint32_t t; char bearer[8]; char *wire; } heard_t;
static heard_t g_tr[MAXP];
static int g_n;
static uint64_t g_start_ms;

typedef struct {
    uint32_t t, air, queued;
    uint8_t cls;
    char kind;                    /* 'o' owner, 's' sos, 0 */
    uint32_t author, k;
    uint8_t ptype;
} tx_t;
static tx_t g_tx[MAXTX];
static int g_ntx;

static char g_viol[MAXV][160];
static int g_nviol, g_viol_total;
static void viol(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void viol(const char *fmt, ...)
{
    g_viol_total++;
    if (g_nviol >= MAXV) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_viol[g_nviol++], sizeof g_viol[0], fmt, ap);
    va_end(ap);
}

/* ── the station ─────────────────────────────────────────────────────── */

static uint32_t g_now = 1000;      /* uptime ms */
static uint32_t g_busy;
static xb_t g_b;
static xb_duty_t g_d;
static xq_t g_q;

/* Who was heard directly, per side: call hash -> uptime ms. */
#define HN 128
static struct { uint32_t h, t; bool lora; } g_heard[HN];
static void heard_note(uint32_t h, bool lora)
{
    int slot = -1, old = 0;
    for (int i = 0; i < HN; i++) {
        if (g_heard[i].h == h && g_heard[i].lora == lora) { slot = i; break; }
        if (!g_heard[i].h && slot < 0) slot = i;
        if (g_heard[i].t < g_heard[old].t) old = i;
    }
    if (slot < 0) slot = old;
    g_heard[slot].h = h; g_heard[slot].lora = lora; g_heard[slot].t = g_now;
}
static bool heard_on(const char *call, bool lora)
{
    uint32_t h = xq_call_hash(call, (int)strlen(call));
    for (int i = 0; i < HN; i++)
        if (g_heard[i].h == h && g_heard[i].lora == lora &&
            g_now - g_heard[i].t <= 600000)
            return true;
    return false;
}
static bool h_here(const char *c) { return heard_on(c, true); }
static bool h_cheaper(const char *c) { return heard_on(c, false); }
static uint32_t h_epoch(void) { return (uint32_t)(g_start_ms / 1000) + (g_now - 1000) / 1000; }
/* Our own share of the last minute plus a steady 10% of foreign traffic,
 * as the Dart harness assumes. */
static uint8_t h_util(void)
{
    uint32_t ms = 0;
    for (int i = g_ntx - 1; i >= 0 && g_now - g_tx[i].t <= 60000; i--) ms += g_tx[i].air;
    return (uint8_t)(10 + ms * 100 / 60000);
}

/* What the injected packets are, by their m: text. */
static char g_inj_kind(const char *w)
{
    if (strstr(w, "m:owner ")) return 'o';
    if (strstr(w, "m:help ")) return 's';
    return 0;
}
static uint32_t g_inj_at[64];       /* index by kind*32 + n */
static int inj_index(const char *w)
{
    const char *m = strstr(w, "m:owner ");
    if (m) return atoi(m + 8) % 32;
    m = strstr(w, "m:help ");
    if (m) return 32 + atoi(m + 7) % 32;
    return -1;
}
static bool g_inj_aired[64];

static void ctx_now(xq_ctx_t *c)
{
    memset(c, 0, sizeof *c);
    c->now_ms = g_now;
    c->epoch_s = h_epoch();
    c->spent_ms = g_d.spent_ms;
    c->budget_ms = g_d.budget_ms;
    int st = xb_peer_count(&g_b, 600);
    c->stations = (uint8_t)(st > 255 ? 255 : st);
    c->util_pct = h_util();
}

static bool f_air(void *ctx, const char *w, int n)
{
    (void)ctx;
    uint32_t air = lf_ms(n, NULL);
    tx_t *t = g_ntx < MAXTX ? &g_tx[g_ntx++] : NULL;
    const xb_queued_t *e = NULL;
    for (int i = 0; i < XB_QUEUE_MAX; i++)
        if (w == g_b.queue[i].wire) e = &g_b.queue[i];
    if (t) {
        memset(t, 0, sizeof *t);
        t->t = g_now; t->air = air; t->queued = e ? e->queued_ms : g_now;
        int fl = 0;
        const char *f = xq_field(w, n, "f", &fl);
        t->author = f ? xq_call_hash(f, fl) : 0;
        t->ptype = xq_periodic_type(w, n);
        const char *k = xq_field(w, n, "k", &fl);
        t->k = k ? xq_call_hash(k, fl) : 0;   /* any stable hash will do */
        if (e) t->cls = e->cls;
        else {
            xq_ctx_t c; ctx_now(&c);
            c.own = f && t->author == xq_call_hash(SELF, 6);
            t->cls = (uint8_t)xq_classify(w, n, &c);
        }
        t->kind = g_inj_kind(w);
        int ix = inj_index(w);
        if (t->kind && ix >= 0 && !g_inj_aired[ix]) {
            g_inj_aired[ix] = true;
            uint32_t lim = t->kind == 's' ? 5000 : 30000;
            if (g_now - g_inj_at[ix] > lim)
                viol("3: %s took %ums", t->kind == 's' ? "sos" : "owner",
                     g_now - g_inj_at[ix]);
        }
    }
    /* Criterion 9: nothing ahead of it in 4.3 order that was due could
     * have gone. */
    if (e) {
        xq_ctx_t c; ctx_now(&c);
        for (int i = 0; i < XB_QUEUE_MAX; i++) {
            const xb_queued_t *x = &g_b.queue[i];
            if (!x->used || x == e || (int32_t)(g_now - x->due_ms) < 0) continue;
            if (xb_qos_order(x, e) >= 0) continue;
            if (xq_may_air(&g_q, (xq_class_t)x->cls,
                           x->cls >= XQ_CONV ? x->author : 0,
                           lf_ms(x->len, NULL), &c) == XQ_OK)
                viol("9: %s waited behind %s", xq_class_name((xq_class_t)x->cls),
                     xq_class_name((xq_class_t)e->cls));
        }
    }
    g_busy = g_now + air;
    return true;
}
static uint32_t f_now(void) { return g_now; }
static uint32_t f_rand(void) { return (uint32_t)rand(); }
static const xb_ops_t k_ops = { .air = f_air, .now_ms = f_now, .random = f_rand,
                                .name = "lora" };

static void on_lora(const char *w, int n, uint64_t peer, int rssi)
{
    (void)peer; (void)rssi;
    int fl = 0;
    const char *f = xq_field(w, n, "f", &fl);
    if (f && xq_call_hash(f, fl) == xq_call_hash(SELF, 6)) return;
    xb_digipeat(&g_b, w, n);
}

/* ── the run ─────────────────────────────────────────────────────────── */

static void load(void)
{
    static char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "#start ", 7)) { g_start_ms = strtoull(line + 7, NULL, 10); continue; }
        if (g_n >= MAXP) break;
        char *a = strchr(line, '\t'); if (!a) continue; *a++ = 0;
        char *b = strchr(a, '\t'); if (!b) continue; *b++ = 0;
        char *c = strchr(b, '\t'); if (!c) continue; *c++ = 0;
        g_tr[g_n].t = (uint32_t)strtoul(line, NULL, 10);
        snprintf(g_tr[g_n].bearer, sizeof g_tr[g_n].bearer, "%s", a);
        g_tr[g_n].wire = strdup(c);
        g_n++;
    }
}

static void ts_of(uint32_t epoch, char out[32])
{
    /* civil_from_days */
    int32_t z = (int32_t)(epoch / 86400) + 719468;
    int32_t era = z / 146097, doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    int32_t m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    uint32_t s = epoch % 86400;
    snprintf(out, 32, "%04d-%02d-%02d_%02u:%02u:%02u", (int)y, (int)m, (int)d,
             s / 3600, s / 60 % 60, s % 60);
}

int main(int argc, char **argv)
{
    int speed = argc > 1 ? atoi(argv[1]) : 1;
    if (speed < 1) speed = 1;
    srand(7);
    load();
    if (!g_n) { printf("no trace on stdin\n"); return 2; }

    xb_init(&g_b, &k_ops, SELF);
    xb_set_rx_cb(&g_b, on_lora);
    xb_set_duty(&g_b, &g_d, lf_ms, NULL, BUDGET, 6000, 0);
    xq_init(&g_q, &XQ_PROFILE_LORA, XPRS_REACH_REGIONAL);
    xq_hooks_t h = { .heard_here = h_here, .reached_cheaper = h_cheaper,
                     .epoch_s = h_epoch, .util_pct = h_util };
    xq_set_hooks(&g_q, &h);
    xb_set_qos(&g_b, &g_q);

    uint32_t span = g_tr[g_n - 1].t / (uint32_t)speed;
    uint32_t end = span + 10 * 60000;
    uint32_t next_owner = 3 * 60000, sos_at[3] = { span / 5, span / 2, span * 4 / 5 };
    int owner_n = 0, sos_n = 0, i = 0;
    bool owner_pending = false;
    char owner_wire[128];
    uint32_t owner_t = 0;

    for (uint32_t rel = 0; rel <= end; rel += 250) {
        g_now = 1000 + rel;
        while (i < g_n && g_tr[i].t / (uint32_t)speed <= rel) {
            heard_t *t = &g_tr[i++];
            int n = (int)strlen(t->wire);
            int fl = 0;
            const char *f = xq_field(t->wire, n, "f", &fl);
            uint32_t fh = f ? xq_call_hash(f, fl) : 0;
            bool lora = !strcmp(t->bearer, "lora");
            if (fh && !xq_field(t->wire, n, "via", &fl)) heard_note(fh, lora);
            if (fh == xq_call_hash(SELF, 6)) continue;
            if (lora) xb_on_wire(&g_b, t->wire, n, fh, -90);
            else xb_offer(&g_b, t->wire, n);
        }
        if (rel >= next_owner && rel <= span) {
            char ts[32];
            ts_of(h_epoch(), ts);
            snprintf(owner_wire, sizeof owner_wire,
                     "t:message f:" SELF " ts:%s scope:regional m:owner %d", ts, owner_n);
            g_inj_at[owner_n % 32] = g_now;
            owner_n++;
            owner_pending = true;
            owner_t = g_now;
            next_owner += 7 * 60000;
        }
        if (sos_n < 3 && rel >= sos_at[sos_n]) {
            char ts[32], w[96];
            ts_of(h_epoch(), ts);
            snprintf(w, sizeof w, "t:sos f:X1HELP ts:%s m:help %d", ts, sos_n);
            g_inj_at[32 + sos_n] = g_now;
            sos_n++;
            xb_offer(&g_b, w, (int)strlen(w));
        }
        if ((int32_t)(g_now - g_busy) < 0) continue;
        if (owner_pending) {
            /* The radio is free: this station says it now. */
            xb_send_t r = xb_send_ex(&g_b, owner_wire, (int)strlen(owner_wire));
            if (r == XB_REFUSED) viol("injected owner refused at %ums", g_now - owner_t);
            owner_pending = false;
            continue;
        }
        xb_tick(&g_b, g_now);
    }
    for (int k = 0; k < owner_n; k++) if (!g_inj_aired[k]) viol("3: owner %d never aired", k);
    for (int k = 0; k < sos_n; k++) if (!g_inj_aired[32 + k]) viol("3: sos %d never aired", k);

    /* 1, 2, 8: sliding windows over what went out. */
    uint32_t total = 0, burst = 0;
    for (int k = 0; k < g_ntx; k++) {
        uint32_t hour = 0, six = 0, all6 = 0;
        for (int j = k; j >= 0 && g_tx[k].t - g_tx[j].t < 3600000u; j--) {
            hour += g_tx[j].air;
            if (g_tx[k].t - g_tx[j].t < 360000u) {
                all6 += g_tx[j].air;
                if (g_tx[j].cls >= XQ_CONV) six += g_tx[j].air;
            }
        }
        total += g_tx[k].air;
        if (all6 > burst) burst = all6;
        if (hour > BUDGET) viol("1: %ums in the hour", hour);
        if (g_tx[k].cls >= XQ_CONV && hour > BUDGET / 10 * 9)
            viol("2: third party at %ums, past 90%%", hour);
        if (six > BUDGET / 10 + 2000) viol("8: %ums of L2-L6 in six minutes", six);
    }
    /* 4: no call for help shed for anything but scope. */
    for (int s = 1; s < XQ_SHED_N; s++)
        if (s != XQ_SCOPE && g_q.count[XQ_LIFE].shed[s])
            viol("4: sos shed: %s", xq_shed_name((xq_shed_t)s));
    /* 6: repeats. */
    for (int k = 0; k < g_ntx; k++) {
        if (!g_tx[k].ptype) continue;
        for (int j = k - 1; j >= 0; j--) {
            if (g_tx[j].ptype != g_tx[k].ptype || g_tx[j].author != g_tx[k].author) continue;
            if (g_tx[k].ptype == 3 && g_tx[j].k != g_tx[k].k) break;
            if (g_tx[k].t - g_tx[j].t < 1800000u / 100 * 95)
                viol("6: a periodic packet again after %us", (g_tx[k].t - g_tx[j].t) / 1000);
            break;
        }
    }
    /* 5: at x1 nothing of a conversation admitted is lost. */
    int lost = 0;
    if (speed == 1) {
        static const xq_shed_t loss[] = { XQ_EXPIRED, XQ_EVICTED, XQ_BUDGET, XQ_FAIR };
        for (int s = 0; s < 4; s++) lost += g_q.count[XQ_CONV].shed[loss[s]];
        if (lost) viol("5: %d conversation packets lost", lost);
    }

    double mins = span / 60000.0;
    printf("speed x%d: %d heard in %.0f min, %d aired, %us of LongFast (%.0f s/h), "
           "largest 6-min burst %us, reserve low-water %lds\n", speed, g_n, mins, g_ntx,
           total / 1000, total / 1000.0 / mins * 60, burst / 1000,
           (long)(g_q.reserve_low_ms / 1000));
    for (int c = 0; c < XQ_CLASS_N; c++) {
        const xq_count_t *k = &g_q.count[c];
        if (!k->offered && !k->aired) continue;
        uint32_t w[MAXTX]; int nw = 0;
        for (int t = 0; t < g_ntx; t++) if (g_tx[t].cls == c) w[nw++] = g_tx[t].t - g_tx[t].queued;
        for (int a = 1; a < nw; a++) { uint32_t v = w[a]; int b = a - 1;
            while (b >= 0 && w[b] > v) { w[b + 1] = w[b]; b--; } w[b + 1] = v; }
        printf("  %-12s offered %lu aired %lu wait median %us p95 %us shed",
               xq_class_name((xq_class_t)c), (unsigned long)k->offered,
               (unsigned long)k->aired, nw ? w[nw / 2] / 1000 : 0,
               nw ? w[nw * 95 / 100] / 1000 : 0);
        for (int s = 1; s < XQ_SHED_N; s++)
            if (k->shed[s]) printf(" %s=%u", xq_shed_name((xq_shed_t)s), k->shed[s]);
        printf("\n");
    }
    for (int v = 0; v < g_nviol; v++) printf("VIOLATION %s\n", g_viol[v]);
    if (g_viol_total) printf("%d violations\n", g_viol_total);
    int owners = 0;
    for (int t = 0; t < g_ntx; t++) if (g_tx[t].kind == 'o') owners++;
    if (!owners) { printf("no owner message aired\n"); return 1; }
    return g_viol_total ? 1 : 0;
}
