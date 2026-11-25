/* xb_qos.c -- QoS.md for a constrained bearer. Read xb_qos.h first, and the
 * Dart twin (core xprs_qos.dart) when changing a rule: they move together. */

#include "xb_qos.h"

#include <stdio.h>
#include <string.h>

/* ── Profiles (section 6) ─────────────────────────────────────────────── */

const xq_profile_t XQ_PROFILE_SHORT = {
    "short", 300, 300, 8, 25, 1800, 1800, true, 4, 100, 20, 100, 600, 500,
};
const xq_profile_t XQ_PROFILE_LORA = {
    "lora", 1800, 3600, 8, 25, 1800, 1800, true, 4, 100, 20, 100, 600, 500,
};
const xq_profile_t XQ_PROFILE_LORA_1PCT = {
    "lora-1pct", 3600, 7200, 8, 25, 1800, 1800, false, 4, 100, 20, 100, 600, 500,
};
const xq_profile_t XQ_PROFILE_APRS = {
    "aprs-1200", 1800, 3600, 8, 25, 1800, 1800, false, 4, 100, 20, 100, 600, 500,
};

#define XQ_AIRED_MS   1800000u      /* 4.4: not twice in half an hour */
#define XQ_SLACK_PM   50u           /* 4.5: 5% of the interval is drift */

/* Elapsed ms, signed: the two clocks of xprsbearer.c's xb_since. */
static inline int32_t since(uint32_t now, uint32_t then)
{
    return (int32_t)(now - then);
}

/* ── Reading a wire ───────────────────────────────────────────────────── */

const char *xq_field(const char *w, int len, const char *key, int *vl)
{
    if (!w || !key) return NULL;
    int kl = (int)strlen(key);
    int i = 0;
    while (i < len) {
        int e = i;
        while (e < len && w[e] != ' ') e++;
        /* m: is the last field and the rest of the wire is its text. */
        if (e - i >= 2 && w[i] == 'm' && w[i + 1] == ':') break;
        if (e - i > kl && memcmp(w + i, key, (size_t)kl) == 0 && w[i + kl] == ':') {
            if (vl) *vl = e - i - kl - 1;
            return w + i + kl + 1;
        }
        i = e + 1;
    }
    return NULL;
}

static bool is(const char *v, int vl, const char *s)
{
    return v && (int)strlen(s) == vl && memcmp(v, s, (size_t)vl) == 0;
}

static int num(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* Howard Hinnant's days_from_civil. */
static int32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint32_t xq_ts_epoch(const char *v, int vl)
{
    if (!v || vl != 19 || v[4] != '-' || v[7] != '-' || v[10] != '_' ||
        v[13] != ':' || v[16] != ':')
        return 0;
    int y = num(v, 4), mo = num(v + 5, 2), d = num(v + 8, 2);
    int h = num(v + 11, 2), mi = num(v + 14, 2), s = num(v + 17, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
        mi < 0 || mi > 59 || s < 0 || s > 60)
        return 0;
    int32_t days = days_from_civil(y, mo, d);
    return (uint32_t)days * 86400u + (uint32_t)(h * 3600 + mi * 60 + s);
}

uint32_t xq_call_hash(const char *c, int len)
{
    if (!c || len <= 0) return 0;
    uint32_t h = 2166136261u;
    int n = 0;
    for (int i = 0; i < len && c[i] && c[i] != '-' && c[i] != ' '; i++) {
        char ch = c[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        h = (h ^ (uint8_t)ch) * 16777619u;
        n++;
    }
    if (!n) return 0;
    return h ? h : 1u;
}

static uint32_t author_of(const char *w, int len)
{
    int vl = 0;
    const char *f = xq_field(w, len, "f", &vl);
    return f ? xq_call_hash(f, vl) : 0;
}

static uint32_t hash_bytes(const char *v, int vl)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < vl; i++) h = (h ^ (uint8_t)v[i]) * 16777619u;
    return h;
}

static uint32_t k_hash(const char *w, int len)
{
    int vl = 0;
    const char *k = xq_field(w, len, "k", &vl);
    return k ? hash_bytes(k, vl) : 0;
}

static uint32_t id_num(const char *id)
{
    uint32_t v = 0;
    for (int i = 0; id && id[i] && i < 8; i++) {
        char c = id[i];
        int d = c >= '0' && c <= '9' ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint32_t)d;
    }
    return v ? v : 1u;
}

uint8_t xq_periodic_type(const char *w, int len)
{
    int vl = 0;
    const char *t = xq_field(w, len, "t", &vl);
    if (is(t, vl, "observation")) return 1;
    if (is(t, vl, "service"))     return 2;
    if (is(t, vl, "identity"))    return 3;
    if (is(t, vl, "track"))       return 4;
    return 0;
}

xq_class_t xq_classify(const char *w, int len, const xq_ctx_t *c)
{
    int vl = 0;
    const char *t = xq_field(w, len, "t", &vl);
    if (is(t, vl, "sos") || is(t, vl, "warning")) return XQ_LIFE;
    if (c && c->own) return XQ_OWNER;
    if (is(t, vl, "identity")) return XQ_KEYS;
    if (is(t, vl, "observation") || is(t, vl, "service") || is(t, vl, "track"))
        return XQ_PRESENCE;
    /* An ask is a transaction: somebody waits for the answer. What it brings
     * back is bulk by its age, which xq_admit decides. */
    if (is(t, vl, "command") || is(t, vl, "result") || is(t, vl, "request"))
        return XQ_TXN;
    if (is(t, vl, "mailbox")) return XQ_BULK;
    return XQ_CONV;
}

uint8_t xq_urg_rank(const char *w, int len, bool stranger)
{
    int vl = 0;
    const char *u = xq_field(w, len, "urg", &vl);
    if (is(u, vl, "urgent")) return stranger ? 1 : 0;
    if (is(u, vl, "high")) return 1;
    if (is(u, vl, "low")) return 3;
    return 2;
}

/* ── Rolling buckets ──────────────────────────────────────────────────── */

static uint32_t roll_steps(uint32_t *head_ms, uint32_t step, int n, uint32_t now)
{
    int32_t s = since(now, *head_ms);
    uint32_t gap = s > 0 ? (uint32_t)s : 0u;
    if (gap >= step * (uint32_t)n) {
        *head_ms = now;
        return (uint32_t)n;           /* everything rolled off */
    }
    uint32_t k = gap / step;
    *head_ms += k * step;
    return k;
}

static void pace_roll(xq_t *q, uint32_t now)
{
    uint32_t k = roll_steps(&q->pace_head_ms, XQ_PACE_MS, XQ_PACE_N, now);
    for (uint32_t i = 0; i < k; i++) {
        q->pace_head = (uint8_t)((q->pace_head + 1) % XQ_PACE_N);
        q->pace[q->pace_head] = 0;
    }
}

static void auth_roll(xq_t *q, uint32_t now)
{
    uint32_t k = roll_steps(&q->auth_head_ms, XQ_AUTH_MS, XQ_AUTH_N, now);
    for (uint32_t i = 0; i < k; i++) {
        q->auth_head = (uint8_t)((q->auth_head + 1) % XQ_AUTH_N);
        for (int a = 0; a < XQ_AUTHORS_N; a++) q->authors[a].b[q->auth_head] = 0;
    }
}

static uint32_t author_sum(const xq_author_t *a)
{
    uint32_t s = 0;
    for (int i = 0; i < XQ_AUTH_N; i++) s += a->b[i];
    return s;
}

static void add16(uint16_t *v, uint32_t ms)
{
    uint32_t x = (uint32_t)*v + ms;
    *v = x > 65535u ? 65535u : (uint16_t)x;
}

uint32_t xq_paced_ms(xq_t *q, uint32_t now)
{
    pace_roll(q, now);
    uint32_t s = 0;
    for (int i = 0; i < XQ_PACE_N; i++) s += q->pace[i];
    return s;
}

uint32_t xq_author_spent(xq_t *q, uint32_t author, uint32_t now)
{
    if (!author) return 0;
    auth_roll(q, now);
    for (int a = 0; a < XQ_AUTHORS_N; a++)
        if (q->authors[a].author == author) return author_sum(&q->authors[a]);
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void xq_init(xq_t *q, const xq_profile_t *p, xprs_reach_t reach)
{
    if (!q) return;
    memset(q, 0, sizeof *q);
    q->p = p ? p : &XQ_PROFILE_SHORT;
    q->reach = reach;
    q->reserve_low_ms = -1;
}

void xq_set_hooks(xq_t *q, const xq_hooks_t *h)
{
    if (!q) return;
    if (h) q->hooks = *h;
    else memset(&q->hooks, 0, sizeof q->hooks);
}

void xq_reset(xq_t *q)
{
    if (!q) return;
    const xq_profile_t *p = q->p;
    xprs_reach_t r = q->reach;
    xq_hooks_t h = q->hooks;
    xq_init(q, p, r);
    q->hooks = h;
}

/* ── 4.5 ──────────────────────────────────────────────────────────────── */

bool xq_crowded(const xq_t *q, const xq_ctx_t *c)
{
    return c->stations >= q->p->crowded_stations ||
           c->util_pct > q->p->crowded_util_pct;
}

uint32_t xq_repeat_ms(const xq_t *q, const xq_ctx_t *c)
{
    return (uint32_t)(xq_crowded(q, c) ? q->p->repeat_crowded_s
                                       : q->p->repeat_s) * 1000u;
}

static xq_repeat_t *repeat_find(xq_t *q, uint32_t author, uint8_t type)
{
    for (int i = 0; i < XQ_REPEAT_N; i++)
        if (q->repeat[i].type == type && q->repeat[i].author == author)
            return &q->repeat[i];
    return NULL;
}

static bool repeat_due(xq_t *q, const char *w, int len, uint8_t type,
                       const xq_ctx_t *c)
{
    xq_repeat_t *r = repeat_find(q, author_of(w, len), type);
    if (!r) return true;
    /* A real change is not a repeat: an identity with another key (XPRS.md
     * 29.1) goes out at once. */
    if (type == 3 && k_hash(w, len) != r->k) return true;
    uint32_t need = xq_repeat_ms(q, c) / 1000u * (1000u - XQ_SLACK_PM);
    return since(c->now_ms, r->t_ms) >= (int32_t)need;
}

/* ── Admission (4.4) ──────────────────────────────────────────────────── */

void xq_note(xq_t *q, const char *w, int len, uint32_t now)
{
    if (!q || !w) return;
    int vl = 0;
    const char *v = xq_field(w, len, "ts", &vl);
    uint32_t ts = xq_ts_epoch(v, vl);
    if (!ts) return;
    if (!q->newest_ts || ts > q->newest_ts ||
        since(now, q->newest_ts_at_ms) > 600000) {
        q->newest_ts = ts;
        q->newest_ts_at_ms = now;
    }
}

/* UTC seconds now, as well as this station can say: with no clock, the
 * newest ts: heard lately (4.4). 0 when there is nothing to go by. */
static uint32_t now_s(const xq_t *q, const xq_ctx_t *c)
{
    if (c->epoch_s) return c->epoch_s;
    if (!q->newest_ts) return 0;
    int32_t d = since(c->now_ms, q->newest_ts_at_ms);
    return q->newest_ts + (uint32_t)(d > 0 ? d / 1000 : 0);
}

static bool aired_recently(const xq_t *q, uint32_t id, uint32_t now)
{
    if (!id) return false;
    for (int i = 0; i < XQ_AIRED_N; i++)
        if (q->aired[i].id == id && since(now, q->aired[i].t_ms) < (int32_t)XQ_AIRED_MS)
            return true;
    return false;
}

static void upper_copy(char *out, int cap, const char *v, int vl)
{
    int n = vl < cap - 1 ? vl : cap - 1;
    for (int i = 0; i < n; i++) {
        char ch = v[i];
        out[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
    }
    out[n] = 0;
}

static xq_decision_t decide(xq_t *q, const char *w, int len, const char *id,
                            const xq_ctx_t *c, xq_class_t cls)
{
    xq_decision_t d = { cls, XQ_OK };

    /* Scope binds every bearer and every class, sos included: a local call
     * for help is for the room (XPRS.md 9.11). */
    if (!xprs_reach_admits(xprs_scope_reach_wire(w, len), q->reach)) {
        d.shed = XQ_SCOPE;
        return d;
    }
    if (cls == XQ_LIFE) return d;

    /* 4.5, owner traffic included: our own beacon is a beacon. */
    uint8_t pt = xq_periodic_type(w, len);
    if (pt && !repeat_due(q, w, len, pt, c)) {
        d.shed = XQ_REPEAT;
        return d;
    }
    if (cls == XQ_OWNER) return d;

    char dest[16] = "";
    int dl = 0;
    const char *dv = xq_field(w, len, "d", &dl);
    if (dv && dl > 0) upper_copy(dest, sizeof dest, dv, dl);
    if (dest[0] && q->hooks.reached_cheaper && q->hooks.reached_cheaper(dest)) {
        d.shed = XQ_REACHED;
        return d;
    }
    if (!c->budget_ms) return d;

    /* ── Constrained bearers only from here ── */
    if (cls == XQ_TXN &&
        !(dest[0] && (!q->hooks.heard_here || q->hooks.heard_here(dest)))) {
        d.shed = XQ_NOT_HERE;
        return d;
    }
    char idb[XPRS_ID_LEN];
    if (!id && xprs_id_of(w, len, idb)) id = idb;
    if (id && aired_recently(q, id_num(id), c->now_ms)) {
        d.shed = XQ_AIRED;
        return d;
    }
    /* Old news is bulk; bulk only on spare capacity (4.4, 4.7). */
    bool bulk = cls == XQ_BULK;
    uint32_t now = now_s(q, c);
    int vl = 0;
    const char *tv = xq_field(w, len, "ts", &vl);
    uint32_t ts = xq_ts_epoch(tv, vl);
    if (!bulk && now && ts && now > ts) {
        uint32_t fresh = cls == XQ_TXN ? q->p->fresh_txn_s : q->p->fresh_bulk_s;
        if (now - ts > fresh) bulk = true;
    }
    if (bulk) {
        d.cls = XQ_BULK;
        uint64_t spent_pm = (uint64_t)c->spent_ms * 1000u / c->budget_ms;
        uint32_t free_pm = spent_pm >= 1000u ? 0u : (uint32_t)(1000u - spent_pm);
        if (!q->p->bulk_on || xq_crowded(q, c) || free_pm <= q->p->bulk_min_free_pm)
            d.shed = XQ_OLD_NEWS;
    }
    return d;
}

xq_decision_t xq_admit(xq_t *q, const char *w, int len, const char *id,
                       const xq_ctx_t *c, bool count)
{
    xq_decision_t d = { XQ_CONV, XQ_OK };
    if (!q || !w || len <= 0 || !c) return d;
    xq_note(q, w, len, c->now_ms);
    d = decide(q, w, len, id, c, xq_classify(w, len, c));
    if (count) {
        xq_count_t *k = &q->count[d.cls];
        k->offered++;
        if (d.shed == XQ_OK) k->admitted++;
        else if (k->shed[d.shed] < 65535u) k->shed[d.shed]++;
    }
    return d;
}

/* ── The budget (4.2, 4.6, 4.7) ───────────────────────────────────────── */

static uint32_t share(uint32_t budget, uint32_t pm)
{
    return (uint32_t)((uint64_t)budget * pm / 1000u);
}

uint32_t xq_fair_cap(xq_t *q, uint32_t author, uint32_t budget, uint32_t now)
{
    auth_roll(q, now);
    uint32_t third = share(budget, 1000u - q->p->reserve_pm);
    /* The authors who spent this hour, [author] counted whether or not. */
    int n = 0;
    bool listed = false;
    uint32_t spends[XQ_AUTHORS_N];
    uint32_t who[XQ_AUTHORS_N];
    for (int a = 0; a < XQ_AUTHORS_N; a++) {
        if (!q->authors[a].author) continue;
        uint32_t s = author_sum(&q->authors[a]);
        if (!s && q->authors[a].author != author) continue;
        if (q->authors[a].author == author) listed = true;
        spends[n] = s;
        who[n] = q->authors[a].author;
        n++;
    }
    int count = n + (listed ? 0 : 1);
    int div = count > q->p->fair_min_div ? count : q->p->fair_min_div;
    uint32_t equal = third / (uint32_t)div;
    uint32_t light = 0;
    int light_n = 0;
    for (int i = 0; i < n; i++) {
        if (who[i] != author && spends[i] < equal) {
            light += spends[i];
            light_n++;
        }
    }
    int heavy = count - light_n;
    if (q->p->fair_min_div - light_n > heavy) heavy = q->p->fair_min_div - light_n;
    if (heavy < 1) heavy = 1;
    return third > light ? (third - light) / (uint32_t)heavy : 0;
}

xq_shed_t xq_may_air(xq_t *q, xq_class_t cls, uint32_t author, uint32_t air,
                     const xq_ctx_t *c)
{
    if (!q || !c || !c->budget_ms) return XQ_OK;
    uint32_t budget = c->budget_ms, spent = c->spent_ms;
    switch (cls) {
    case XQ_LIFE:
        return spent + air <= budget ? XQ_OK : XQ_BUDGET;
    case XQ_OWNER:
        return spent + air <= share(budget, 1000u - q->p->life_pm) ? XQ_OK
                                                                   : XQ_BUDGET;
    default:
        break;
    }
    if (spent + air > share(budget, 1000u - q->p->reserve_pm)) return XQ_BUDGET;
    /* Background traffic (keys, presence, bulk) has only part of the pace,
     * so the rest is always there for conversation and transactions: an old
     * ts: sorts ahead of a fresh message by 4.3, and the Dart harness
     * measured messages waiting five minutes behind history pages until
     * this was added. */
    uint32_t pace = share(budget, q->p->pace_pm);
    if (cls >= XQ_KEYS) pace = share(pace, q->p->background_pm);
    if (xq_paced_ms(q, c->now_ms) + air > pace) return XQ_BUDGET;
    if (author &&
        xq_author_spent(q, author, c->now_ms) + air >
            xq_fair_cap(q, author, budget, c->now_ms))
        return XQ_FAIR;
    return XQ_OK;
}

static xq_author_t *author_slot(xq_t *q, uint32_t author)
{
    int best = -1;
    uint32_t best_s = 0xFFFFFFFFu;
    for (int a = 0; a < XQ_AUTHORS_N; a++) {
        if (q->authors[a].author == author) return &q->authors[a];
        uint32_t s = q->authors[a].author ? author_sum(&q->authors[a]) : 0;
        if (!q->authors[a].author) s = 0;
        if (s < best_s) { best_s = s; best = a; }
    }
    /* Full: the author with the least airtime this hour gives up its row,
     * which forgets the least. */
    memset(&q->authors[best], 0, sizeof q->authors[best]);
    q->authors[best].author = author;
    return &q->authors[best];
}

void xq_aired(xq_t *q, const char *w, int len, const char *id, xq_class_t cls,
              uint32_t author, uint32_t air, uint32_t spent_after,
              uint32_t budget, uint32_t now)
{
    if (!q) return;
    if (air) {
        if (cls >= XQ_CONV) {
            pace_roll(q, now);
            add16(&q->pace[q->pace_head], air);
            if (author) {
                auth_roll(q, now);
                add16(&author_slot(q, author)->b[q->auth_head], air);
            }
        }
        if (budget) {
            int32_t left = (int32_t)budget - (int32_t)spent_after;
            if (q->reserve_low_ms < 0 || left < q->reserve_low_ms)
                q->reserve_low_ms = left;
        }
    }
    if (w && len > 0) {
        uint8_t pt = xq_periodic_type(w, len);
        if (pt) {
            uint32_t a = author_of(w, len);
            xq_repeat_t *r = repeat_find(q, a, pt);
            if (!r) {
                r = &q->repeat[0];
                for (int i = 0; i < XQ_REPEAT_N; i++) {
                    if (!q->repeat[i].type) { r = &q->repeat[i]; break; }
                    if (since(q->repeat[i].t_ms, r->t_ms) < 0) r = &q->repeat[i];
                }
            }
            r->author = a;
            r->type = pt;
            r->k = k_hash(w, len);
            r->t_ms = now;
        }
        char idb[XPRS_ID_LEN];
        if (!id && xprs_id_of(w, len, idb)) id = idb;
        uint32_t n = id ? id_num(id) : 0;
        if (n) {
            int slot = -1;
            for (int i = 0; i < XQ_AIRED_N; i++)
                if (q->aired[i].id == n) { slot = i; break; }
            if (slot < 0) {
                slot = q->aired_pos;
                q->aired_pos = (uint8_t)((q->aired_pos + 1) % XQ_AIRED_N);
            }
            q->aired[slot].id = n;
            q->aired[slot].t_ms = now;
        }
    }
    if (w && len > 0) {
        q->count[cls].aired++;
    } else {
        q->frames++;
        q->frames_ms += air;
    }
}

void xq_revoke(xq_t *q, xq_class_t cls, xq_shed_t why)
{
    if (!q) return;
    if (q->count[cls].admitted) q->count[cls].admitted--;
    xq_count_shed(q, cls, why);
}

void xq_count_shed(xq_t *q, xq_class_t cls, xq_shed_t why)
{
    if (q && q->count[cls].shed[why] < 65535u) q->count[cls].shed[why]++;
}

uint32_t xq_limit_ms(const xq_t *q, xq_class_t cls, const xq_ctx_t *c)
{
    switch (cls) {
    case XQ_LIFE:     return 600000u;
    case XQ_OWNER:    return 1800000u;
    case XQ_CONV:     return 1800000u;
    case XQ_TXN:      return 120000u;
    case XQ_KEYS:
    case XQ_PRESENCE: return xq_repeat_ms(q, c);
    default:          return 60000u;
    }
}

/* ── Names and the report ─────────────────────────────────────────────── */

const char *xq_class_name(xq_class_t c)
{
    static const char *n[] = { "life", "owner", "conversation", "transaction",
                               "keys", "presence", "bulk" };
    return (unsigned)c < XQ_CLASS_N ? n[c] : "?";
}

const char *xq_shed_name(xq_shed_t s)
{
    static const char *n[] = { "none", "scope", "repeat", "reached", "notHere",
                               "oldNews", "aired", "fairShare", "budget",
                               "expired", "evicted" };
    return (unsigned)s < XQ_SHED_N ? n[s] : "?";
}

int xq_json(const xq_t *q, char *out, int cap)
{
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!q || !q->p) return 0;
    int n = snprintf(out, (size_t)cap, "{\"profile\":\"%s\",\"reserveLowMs\":%ld,"
                     "\"frames\":%lu,\"framesMs\":%lu,\"classes\":{", q->p->name,
                     (long)q->reserve_low_ms, (unsigned long)q->frames,
                     (unsigned long)q->frames_ms);
    bool first = true;
    for (int c = 0; c < XQ_CLASS_N && n < cap; c++) {
        const xq_count_t *k = &q->count[c];
        if (!k->offered && !k->aired) continue;
        n += snprintf(out + n, (size_t)(cap - n),
                      "%s\"%s\":{\"offered\":%lu,\"admitted\":%lu,\"aired\":%lu",
                      first ? "" : ",", xq_class_name((xq_class_t)c),
                      (unsigned long)k->offered, (unsigned long)k->admitted,
                      (unsigned long)k->aired);
        first = false;
        bool any = false;
        for (int s = 1; s < XQ_SHED_N && n < cap; s++) {
            if (!k->shed[s]) continue;
            n += snprintf(out + n, (size_t)(cap - n), "%s\"%s\":%u",
                          any ? "," : ",\"shed\":{", xq_shed_name((xq_shed_t)s),
                          (unsigned)k->shed[s]);
            any = true;
        }
        if (n < cap) n += snprintf(out + n, (size_t)(cap - n), any ? "}}" : "}");
    }
    if (n < cap) n += snprintf(out + n, (size_t)(cap - n), "}}");
    return n < cap ? n : cap - 1;
}
