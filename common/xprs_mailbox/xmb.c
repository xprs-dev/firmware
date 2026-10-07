/* Store and forward without a filesystem (xmb.h). */
#include "xmb.h"

#include <string.h>

#include "xprs.h"

#define MAGIC        0xA5
#define ST_LIVE      0xFF
#define ST_DEAD      0x00
#define DEFAULT_KEEP (7u * 86400u)          /* mail with no until: */

static const uint32_t k_backoff_ms[] = { 30000u, 120000u, 600000u };

/* ── Small helpers ─────────────────────────────────────────────────────── */

uint32_t xmb_hash(const char *s, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n && s[i]; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    return h ? h : 1;
}

static uint32_t hash_str(const char *s) { return xmb_hash(s, (int)strlen(s)); }

static uint16_t crc16(const uint8_t *p, int n, uint16_t c)
{
    while (n-- > 0) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

/* ts:YYYY-MM-DD_HH:MM:SS to epoch seconds, 0 when it is not one. */
static uint32_t ts_epoch(const char *v, int n)
{
    if (!v || n != 19 || v[4] != '-' || v[7] != '-' || v[10] != '_') return 0;
    int f[6], pos[6] = { 0, 5, 8, 11, 14, 17 }, len[6] = { 4, 2, 2, 2, 2, 2 };
    for (int i = 0; i < 6; i++) {
        f[i] = 0;
        for (int k = 0; k < len[i]; k++) {
            char c = v[pos[i] + k];
            if (c < '0' || c > '9') return 0;
            f[i] = f[i] * 10 + (c - '0');
        }
    }
    int y = f[0] - (f[1] <= 2);
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * ((f[1] + 9) % 12) + 2) / 5 + f[2] - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + f[3] * 3600L + f[4] * 60L + f[5]);
}

static bool field(const xprs_t *p, const char *k, const char **v, int *n)
{
    *v = xprs_get(p, k, n);
    return *v && *n > 0;
}

/* The first callsign of a comma list. */
static int first_of(const char *v, int n)
{
    int i = 0;
    while (i < n && v[i] != ',') i++;
    return i;
}

static bool list_has(const char *v, int n, const char *call)
{
    int cl = (int)strlen(call);
    for (int i = 0; i < n; ) {
        int j = i;
        while (j < n && v[j] != ',') j++;
        if (j - i == cl && xmb_hash(v + i, cl) == hash_str(call)) return true;
        i = j + 1;
    }
    return false;
}

static bool mail_kind(const char *t, int n)
{
    static const char *const k[] = { "message", "file", "reaction", "receipt", "sos" };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        if ((int)strlen(k[i]) == n && !memcmp(k[i], t, (size_t)n)) return true;
    return false;
}

/* ── The index ──────────────────────────────────────────────────────────── */

/* Fill a slot's index from its payload. */
static void index_payload(xmb_slot_t *s, const char *w, int len)
{
    s->to_h = s->from_h = s->id_h = 0;
    s->until = 0;
    if (s->kind == XMB_K_STATE) return;
    xprs_t p;
    if (!xprs_parse(w, len, &p)) return;
    const char *v;
    int n;
    if (field(&p, "f", &v, &n)) s->from_h = xmb_hash(v, n);
    if (s->kind == XMB_K_MAIL && field(&p, "d", &v, &n)) s->to_h = xmb_hash(v, first_of(v, n));
    char id[XPRS_ID_LEN];
    if (xprs_id_of(w, len, id)) s->id_h = hash_str(id);
    if (field(&p, "until", &v, &n)) s->until = ts_epoch(v, n);
    else if (s->kind == XMB_K_MAIL && field(&p, "ts", &v, &n) && ts_epoch(v, n))
        s->until = ts_epoch(v, n) + DEFAULT_KEEP;
}

static bool rec_read(const xmb_t *m, int slot, uint8_t *buf, int len)
{
    return m->st.read(m->st.ctx, (uint32_t)slot * XMB_REC, buf, len);
}

/* Clear the status byte in place: the record is dead, durably. */
static bool kill(xmb_t *m, int slot)
{
    xmb_slot_t *s = &m->s[slot];
    if (s->state != XMB_LIVE) return true;
    uint8_t w[4];
    if (!rec_read(m, slot, w, 4)) return false;
    w[3] = ST_DEAD;
    if (!m->st.write(m->st.ctx, (uint32_t)slot * XMB_REC, w, 4)) return false;
    s->state = XMB_DEAD;
    m->live--;
    return true;
}

static int16_t seq_diff(uint16_t a, uint16_t b) { return (int16_t)(a - b); }

int xmb_capacity(const xmb_t *m) { return (m->st.pages - 2) * XMB_PER_PAGE; }

/* ── Writing ────────────────────────────────────────────────────────────── */

static bool page_free(const xmb_t *m, int pg)
{
    for (int i = 0; i < XMB_PER_PAGE; i++)
        if (m->s[pg * XMB_PER_PAGE + i].state != XMB_FREE) return false;
    return true;
}

static int page_live(const xmb_t *m, int pg)
{
    int n = 0;
    for (int i = 0; i < XMB_PER_PAGE; i++)
        if (m->s[pg * XMB_PER_PAGE + i].state == XMB_LIVE) n++;
    return n;
}

static bool erase(xmb_t *m, int pg)
{
    if (!m->st.erase(m->st.ctx, pg)) return false;
    for (int i = 0; i < XMB_PER_PAGE; i++) {
        xmb_slot_t *s = &m->s[pg * XMB_PER_PAGE + i];
        if (s->state == XMB_LIVE) m->live--;
        memset(s, 0, sizeof *s);
    }
    m->stats.erased++;
    return true;
}

static bool append_raw(xmb_t *m, int kind, int cls, const uint8_t *pl, int len, int *slot_out);

/* The head page is full: move to the spare, and make a new spare out of the
 * emptiest other page, its live records carried to the head first. */
static bool rotate(xmb_t *m, int hp)
{
    int spare = -1;
    for (int pg = 0; pg < m->st.pages; pg++)
        if (pg != hp && page_free(m, pg)) { spare = pg; break; }
    if (spare < 0) {
        /* No erased page (a torn erase, or a store formatted by somebody
         * else): the emptiest page is sacrificed, records and all. */
        int best = -1;
        for (int pg = 0; pg < m->st.pages; pg++)
            if (pg != hp && (best < 0 || page_live(m, pg) < page_live(m, best))) best = pg;
        if (best < 0 || !erase(m, best)) return false;
        spare = best;
    }
    m->head = spare * XMB_PER_PAGE;
    /* Another erased page is still there: it is the spare, nothing to do. */
    for (int pg = 0; pg < m->st.pages; pg++)
        if (pg != spare && page_free(m, pg)) return true;
    int victim = -1;
    for (int pg = 0; pg < m->st.pages; pg++) {
        if (pg == spare || page_free(m, pg)) continue;
        if (victim < 0 || page_live(m, pg) < page_live(m, victim)) victim = pg;
    }
    if (victim < 0) return true;
    for (int i = 0; i < XMB_PER_PAGE; i++) {
        int from = victim * XMB_PER_PAGE + i;
        xmb_slot_t keep = m->s[from];
        if (keep.state != XMB_LIVE) continue;
        uint8_t rec[XMB_REC];
        if (!rec_read(m, from, rec, XMB_REC)) return false;
        int to;
        if (!append_raw(m, rec[2] >> 4, rec[2] & 0x0F, rec + XMB_HDR, rec[1], &to)) return false;
        /* The delivery state goes with it; the age does not (it is now new). */
        m->s[to].tries = keep.tries;
        m->s[to].next_ms = keep.next_ms;
        m->stats.relocated++;
    }
    return erase(m, victim);
}

static bool append_raw(xmb_t *m, int kind, int cls, const uint8_t *pl, int len, int *slot_out)
{
    if (len <= 0 || len > XMB_PAYLOAD_MAX) return false;
    uint32_t words[XMB_REC / 4];
    uint8_t *rec = (uint8_t *)words;
    memset(rec, 0xFF, XMB_REC);
    uint16_t seq = ++m->seq;
    rec[0] = MAGIC;
    rec[1] = (uint8_t)len;
    rec[2] = (uint8_t)((kind << 4) | (cls & 0x0F));
    rec[3] = ST_LIVE;
    rec[6] = (uint8_t)seq;
    rec[7] = (uint8_t)(seq >> 8);
    memcpy(rec + XMB_HDR, pl, (size_t)len);
    uint16_t c = crc16(rec + 1, 2, 0xFFFF);
    c = crc16(rec + 6, 2 + len, c);
    rec[4] = (uint8_t)c;
    rec[5] = (uint8_t)(c >> 8);
    int slot = m->head;
    int bytes = (XMB_HDR + len + 3) & ~3;
    if (!m->st.write(m->st.ctx, (uint32_t)slot * XMB_REC, rec, bytes)) return false;
    xmb_slot_t *s = &m->s[slot];
    memset(s, 0, sizeof *s);
    s->state = XMB_LIVE;
    s->kind = (uint8_t)kind;
    s->cls = (uint8_t)cls;
    s->seq = seq;
    index_payload(s, (const char *)pl, len);
    m->live++;
    m->head++;
    if (slot_out) *slot_out = slot;
    /* The page is full: rotate() finds the next head. Relocation inside it
     * appends to a fresh page that the invariant keeps from filling. */
    if (m->head % XMB_PER_PAGE == 0 && !rotate(m, slot / XMB_PER_PAGE)) return false;
    return true;
}

/* ── Opening ────────────────────────────────────────────────────────────── */

bool xmb_open(xmb_t *m, const xmb_store_t *st)
{
    memset(m, 0, sizeof *m);
    if (!st || st->pages < 3 || st->pages > XMB_PAGES_MAX) return false;
    m->st = *st;
    m->nslots = st->pages * XMB_PER_PAGE;
    int newest = -1;
    for (int i = 0; i < m->nslots; i++) {
        uint8_t rec[XMB_REC];
        if (!rec_read(m, i, rec, XMB_HDR)) return false;
        xmb_slot_t *s = &m->s[i];
        bool erased = true;
        for (int k = 0; k < XMB_HDR; k++) if (rec[k] != 0xFF) erased = false;
        if (erased) continue;                               /* XMB_FREE */
        s->state = XMB_DEAD;
        if (rec[0] != MAGIC || rec[1] == 0 || rec[1] > XMB_PAYLOAD_MAX) { m->stats.bad++; continue; }
        if (!rec_read(m, i, rec, XMB_HDR + rec[1])) return false;
        uint16_t c = crc16(rec + 1, 2, 0xFFFF);
        c = crc16(rec + 6, 2 + rec[1], c);
        s->seq = (uint16_t)(rec[6] | rec[7] << 8);
        if (newest < 0 || seq_diff(s->seq, m->s[newest].seq) > 0) newest = i;
        if (rec[4] != (uint8_t)c || rec[5] != (uint8_t)(c >> 8)) { m->stats.bad++; continue; }
        if (rec[3] != ST_LIVE) continue;
        s->state = XMB_LIVE;
        s->kind = rec[2] >> 4;
        s->cls = rec[2] & 0x0F;
        index_payload(s, (const char *)rec + XMB_HDR, rec[1]);
        m->live++;
    }
    if (newest < 0) {
        m->head = 0;
        /* A fresh area: whatever is in it that is not ours goes. */
        for (int pg = 0; pg < st->pages; pg++)
            if (!page_free(m, pg) && !erase(m, pg)) return false;
        return true;
    }
    m->seq = m->s[newest].seq;
    /* The head is the first free slot after the newest record, in its page;
     * a slot after it that is not free means a torn page: rotate past it. */
    m->head = newest + 1;
    int hp = newest / XMB_PER_PAGE;
    bool ok = m->head / XMB_PER_PAGE == hp;
    for (int i = m->head; ok && i < (hp + 1) * XMB_PER_PAGE; i++)
        if (m->s[i].state != XMB_FREE) ok = false;
    if (!ok && !rotate(m, hp)) return false;
    /* And a spare must exist besides the head's page. */
    bool spare = false;
    for (int pg = 0; pg < st->pages; pg++)
        if (pg != m->head / XMB_PER_PAGE && page_free(m, pg)) spare = true;
    if (!spare) {
        int best = -1;
        for (int pg = 0; pg < st->pages; pg++)
            if (pg != m->head / XMB_PER_PAGE && (best < 0 || page_live(m, pg) < page_live(m, best)))
                best = pg;
        if (best >= 0 && !erase(m, best)) return false;
    }
    return true;
}

/* ── Mail ───────────────────────────────────────────────────────────────── */

static bool declared(const xmb_t *m, uint32_t to_h, uint32_t now)
{
    for (int i = 0; i < m->nslots; i++) {
        const xmb_slot_t *s = &m->s[i];
        if (s->state == XMB_LIVE && s->kind == XMB_K_DECL && s->from_h == to_h &&
            (!s->until || !now || s->until > now))
            return true;
    }
    return false;
}

int xmb_class(const xmb_t *m, const char *wire, int len, const char *self, bool contact,
              uint32_t now)
{
    xprs_t p;
    const char *v, *t;
    int n, tn;
    if (!xprs_parse(wire, len, &p) || !field(&p, "t", &t, &tn) || !mail_kind(t, tn)) return 0;
    if (!field(&p, "d", &v, &n)) return 0;
    n = first_of(v, n);
    if (xmb_hash(v, n) == hash_str(self)) return 0;
    if (declared(m, xmb_hash(v, n), now)) return 3;
    return contact ? 2 : 0;
}

/* Make room for one record of class [cls]: the oldest of the lowest class
 * goes, never one better than the newcomer. */
static bool make_room(xmb_t *m, int cls)
{
    if (m->live < xmb_capacity(m)) return true;
    int v = -1;
    for (int i = 0; i < m->nslots; i++) {
        const xmb_slot_t *s = &m->s[i];
        if (s->state != XMB_LIVE || s->kind != XMB_K_MAIL) continue;
        if (v < 0 || s->cls < m->s[v].cls ||
            (s->cls == m->s[v].cls && seq_diff(s->seq, m->s[v].seq) < 0))
            v = i;
    }
    if (v < 0 || m->s[v].cls > cls) return false;
    if (!kill(m, v)) return false;
    m->stats.evicted++;
    return true;
}

int xmb_hold(xmb_t *m, const char *wire, int len, int cls, uint32_t now)
{
    if (cls < 2 || len <= 0 || len > XMB_PAYLOAD_MAX) { m->stats.refused++; return -1; }
    xmb_slot_t probe;
    memset(&probe, 0, sizeof probe);
    probe.kind = XMB_K_MAIL;
    index_payload(&probe, wire, len);
    if (!probe.to_h || !probe.id_h || (probe.until && now && probe.until <= now)) {
        m->stats.refused++;
        return -1;
    }
    int to = 0, from = 0;
    for (int i = 0; i < m->nslots; i++) {
        const xmb_slot_t *s = &m->s[i];
        if (s->state != XMB_LIVE || s->kind != XMB_K_MAIL) continue;
        if (s->id_h == probe.id_h) return -1;                 /* held already */
        if (s->to_h == probe.to_h) to++;
        if (s->from_h == probe.from_h) from++;
    }
    if (to >= XMB_PER_TO || from >= XMB_PER_FROM || !make_room(m, cls)) {
        m->stats.refused++;
        return -1;
    }
    int slot;
    if (!append_raw(m, XMB_K_MAIL, cls, (const uint8_t *)wire, len, &slot)) return -1;
    m->stats.held++;
    return slot;
}

bool xmb_decl(xmb_t *m, const char *wire, int len, const char *self, uint32_t now)
{
    xprs_t p;
    const char *v, *f;
    int n, fn;
    if (!xprs_parse(wire, len, &p) || !field(&p, "f", &f, &fn)) return false;
    uint32_t from_h = xmb_hash(f, fn);
    if (field(&p, "remove", &v, &n)) {
        if (n != 7 || memcmp(v, "mailbox", 7) || !field(&p, "r", &v, &n)) return false;
        uint32_t id_h = xmb_hash(v, n);
        for (int i = 0; i < m->nslots; i++) {
            xmb_slot_t *s = &m->s[i];
            if (s->state == XMB_LIVE && s->kind == XMB_K_DECL && s->id_h == id_h &&
                s->from_h == from_h)
                return kill(m, i);
        }
        return false;
    }
    if (!field(&p, "hold", &v, &n) || !list_has(v, n, self)) return false;
    xmb_slot_t probe;
    memset(&probe, 0, sizeof probe);
    probe.kind = XMB_K_DECL;
    index_payload(&probe, wire, len);
    if (probe.until && now && probe.until <= now) return false;
    int decls = 0, oldest = -1;
    for (int i = 0; i < m->nslots; i++) {
        const xmb_slot_t *s = &m->s[i];
        if (s->state != XMB_LIVE || s->kind != XMB_K_DECL) continue;
        if (s->id_h == probe.id_h) return true;              /* known */
        decls++;
        if (oldest < 0 || seq_diff(s->seq, m->s[oldest].seq) < 0) oldest = i;
    }
    if (decls >= XMB_DECLS && !kill(m, oldest)) return false;
    /* A declaration is worth more than any mail: it is what makes mail
     * class 3. Room is made from mail of any class. */
    if (m->live >= xmb_capacity(m) && !make_room(m, 3)) return false;
    return append_raw(m, XMB_K_DECL, 0, (const uint8_t *)wire, len, NULL);
}

bool xmb_receipt(xmb_t *m, const char *id, const char *from)
{
    uint32_t id_h = hash_str(id), from_h = hash_str(from);
    for (int i = 0; i < m->nslots; i++) {
        xmb_slot_t *s = &m->s[i];
        if (s->state == XMB_LIVE && s->kind == XMB_K_MAIL && s->id_h == id_h && s->to_h == from_h) {
            if (!kill(m, i)) return false;
            m->stats.released++;
            return true;
        }
    }
    return false;
}

void xmb_expire(xmb_t *m, uint32_t now)
{
    if (!now) return;
    for (int i = 0; i < m->nslots; i++) {
        xmb_slot_t *s = &m->s[i];
        if (s->state == XMB_LIVE && s->kind != XMB_K_STATE && s->until && s->until <= now &&
            kill(m, i))
            m->stats.expired++;
    }
}

int xmb_due(xmb_t *m, const char *to, uint32_t now_ms, uint32_t now, int *out, int max)
{
    xmb_expire(m, now);
    uint32_t to_h = hash_str(to);
    int n = 0;
    bool used[XMB_PAGES_MAX * XMB_PER_PAGE] = { false };
    while (n < max) {
        int best = -1;
        for (int i = 0; i < m->nslots; i++) {
            const xmb_slot_t *s = &m->s[i];
            if (used[i] || s->state != XMB_LIVE || s->kind != XMB_K_MAIL || s->to_h != to_h) continue;
            if (s->next_ms && (int32_t)(now_ms - s->next_ms) < 0) continue;
            if (best < 0 || seq_diff(s->seq, m->s[best].seq) > 0) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        out[n++] = best;
    }
    return n;
}

int xmb_read(const xmb_t *m, int slot, char *out, int cap)
{
    if (slot < 0 || slot >= m->nslots || m->s[slot].state != XMB_LIVE) return 0;
    uint8_t rec[XMB_REC];
    if (!rec_read(m, slot, rec, XMB_HDR) || rec[1] >= cap) return 0;
    int len = rec[1];
    if (!m->st.read(m->st.ctx, (uint32_t)slot * XMB_REC + XMB_HDR, out, len)) return 0;
    out[len] = 0;
    return len;
}

void xmb_aired(xmb_t *m, int slot, uint32_t now_ms)
{
    if (slot < 0 || slot >= m->nslots) return;
    xmb_slot_t *s = &m->s[slot];
    int k = s->tries < 2 ? s->tries : 2;
    s->next_ms = now_ms + k_backoff_ms[k];
    if (!s->next_ms) s->next_ms = 1;
    if (s->tries < 255) s->tries++;
    m->stats.aired++;
}

int xmb_count(const xmb_t *m, const char *to)
{
    uint32_t to_h = to ? hash_str(to) : 0;
    int n = 0;
    for (int i = 0; i < m->nslots; i++) {
        const xmb_slot_t *s = &m->s[i];
        if (s->state == XMB_LIVE && s->kind == XMB_K_MAIL && (!to || s->to_h == to_h)) n++;
    }
    return n;
}

/* ── The state blob ─────────────────────────────────────────────────────── */

bool xmb_state_put(xmb_t *m, const void *blob, int len)
{
    int old = -1;
    for (int i = 0; i < m->nslots; i++)
        if (m->s[i].state == XMB_LIVE && m->s[i].kind == XMB_K_STATE) old = i;
    if (old >= 0) {
        uint8_t rec[XMB_REC];
        if (rec_read(m, old, rec, XMB_HDR) && rec[1] == len &&
            rec_read(m, old, rec, XMB_HDR + len) && !memcmp(rec + XMB_HDR, blob, (size_t)len))
            return true;                                     /* unchanged: no write */
    }
    if (m->live >= xmb_capacity(m) && !make_room(m, 3)) return false;
    int slot;
    if (!append_raw(m, XMB_K_STATE, 0, (const uint8_t *)blob, len, &slot)) return false;
    /* The old one dies only once the new one is written. */
    for (int i = 0; i < m->nslots; i++)
        if (i != slot && m->s[i].state == XMB_LIVE && m->s[i].kind == XMB_K_STATE) kill(m, i);
    return true;
}

int xmb_state_get(const xmb_t *m, void *blob, int cap)
{
    for (int i = 0; i < m->nslots; i++) {
        if (m->s[i].state != XMB_LIVE || m->s[i].kind != XMB_K_STATE) continue;
        uint8_t rec[XMB_REC];
        if (!rec_read(m, i, rec, XMB_HDR) || rec[1] > cap) return 0;
        if (!m->st.read(m->st.ctx, (uint32_t)i * XMB_REC + XMB_HDR, blob, rec[1])) return 0;
        return rec[1];
    }
    return 0;
}
