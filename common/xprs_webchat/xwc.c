/* xwc.c -- see xwc.h. Plain C: no IDF, no sockets. */
#include "xwc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xprs.h"
#include "xprsid.h"
#include "xprssig.h"

/* ---- limits ------------------------------------------------------------ */
#if defined(ESP_PLATFORM) && !(defined(CONFIG_SPIRAM) && CONFIG_SPIRAM)
#define ACTS_MAX      96      /* every act, every group this station runs */
#else
#define ACTS_MAX      192     /* every act, every group this station runs */
#endif
#define MEMBERS_MAX   24      /* roles per group */
#define AWAY_MAX      12
#define REMOVED_MAX   48
#define NONCES_MAX    8
#define SESSIONS_MAX  12
#define NONCE_LIFE    (5 * 60)
#define SESSION_LIFE  (24 * 60 * 60)
#define FUTURE_SLACK  (5 * 60)
#define MAX_UNTIL     (366u * 24 * 60 * 60)

/* ---- state ------------------------------------------------------------- */
static xwc_cb_t s_cb;
static char s_station[XWC_CALL_LEN];
static const uint8_t *s_priv;

enum { K_NONE = 0, K_GRANT, K_REVOKE, K_ACCEPT, K_LEAVE, K_HIDE, K_REMOVE };
enum { R_NONE = 0, R_INVITED, R_MEMBER, R_MOD, R_ADMIN };

typedef struct {
    char     id[XWC_ID_LEN];
    char     group[XWC_CALL_LEN];
    char     signer[XWC_CALL_LEN];
    char     target[XWC_CALL_LEN];   /* a callsign, or an r: identifier */
    uint32_t ts, until, since;
    uint8_t  kind, role;             /* role: R_MEMBER or R_MOD on a grant */
    uint8_t  accept_mod;             /* accept:mod */
} act_t;
static act_t s_acts[ACTS_MAX];
static int   s_nacts;

typedef struct {
    char    call[XWC_CALL_LEN];
    uint8_t role;
    char    offer[XWC_ID_LEN];
    uint8_t offer_role;
} member_t;
typedef struct { char call[XWC_CALL_LEN]; uint32_t until; } away_t;
typedef struct {
    char     group[XWC_CALL_LEN];
    bool     built;
    uint32_t built_at, next_change;
    member_t m[MEMBERS_MAX]; int nm;
    away_t   away[AWAY_MAX]; int na;
    char     removed[REMOVED_MAX][XWC_ID_LEN]; int nr;
} roster_t;
static roster_t s_rosters[XWC_ROOMS + 1];

typedef struct { char nonce[XWC_NONCE_LEN]; uint32_t until; } nonce_t;
typedef struct { char token[XWC_TOKEN_LEN]; char call[XWC_CALL_LEN]; uint32_t until; } session_t;
static nonce_t   s_nonces[NONCES_MAX];
static session_t s_sessions[SESSIONS_MAX];

typedef struct {
    char    call[XWC_CALL_LEN];
    char    name[XWC_ROOM_NAME];
    char    about[XWC_ROOM_ABOUT];
    bool    priv_room;              /* visibility: private */
    bool    anyone;                 /* posting: anyone */
    uint8_t kind;                   /* RK_CHAT, RK_FORUM, RK_GALLERY */
    uint8_t priv[32], pub[32];
    bool    have_key;
} room_t;
/* What a room is (spec/API-HTTP.md `kind`): a conversation, topics and
 * comments beside it, or a category of photographs. */
enum { RK_CHAT = 0, RK_FORUM, RK_GALLERY };
static const char *const k_kinds[] = { "chat", "forum", "gallery" };
static room_t s_rooms[XWC_ROOMS];
static int    s_nrooms;

/* ---- small helpers ----------------------------------------------------- */
/* The newest moment a verified visitor said it was: a board with no clock
 * (nothing synced it yet) still has to stamp the acts it signs, and the
 * person it is answering just told it the time. */
static uint32_t s_hint_ts;
static uint32_t now_s(void)
{
    uint32_t t = s_cb.now ? s_cb.now() : 0;
    return t ? t : s_hint_ts;
}
/* Lifetimes (a nonce, a session) age by a clock that never jumps. */
static uint32_t mono(void) { return s_cb.uptime ? s_cb.uptime() : now_s(); }
static void ts_seen(const xprs_t *p)
{
    char v[24];
    if (!xprs_get_str(p, "ts", v, sizeof v)) return;
    uint32_t t = 0;
    int Y, M, D, h, m, sec;
    if (sscanf(v, "%4d-%2d-%2d_%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) == 6 && Y >= 2024) {
        int64_t y = Y; y -= M <= 2;
        int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
        int64_t doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
        int64_t days = era * 146097 + (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
        t = (uint32_t)(days * 86400 + h * 3600 + m * 60 + sec);
    }
    if (t > s_hint_ts) s_hint_ts = t;
}

static void base_call(const char *in, char out[XWC_CALL_LEN])
{
    int o = 0;
    for (const char *c = in; c && *c && *c != '-' && o < XWC_CALL_LEN - 1; c++) {
        if (*c == ' ') break;
        out[o++] = (char)toupper((unsigned char)*c);
    }
    out[o] = 0;
}

static bool call_ok(const char *c)
{
    size_t n = strlen(c);
    if (n < 3 || n > 12) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)c[i]) && c[i] != '-') return false;
    return true;
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* YYYY-MM-DD_HH:MM:SS -> epoch, 0 when it is not one. */
static uint32_t ts_parse(const char *s)
{
    int Y, M, D, h, m, sec;
    if (!s || sscanf(s, "%4d-%2d-%2d_%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6)
        return 0;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return 0;
    int64_t days = days_from_civil(Y, M, D);
    int64_t t = days * 86400 + h * 3600 + m * 60 + sec;
    return t > 0 ? (uint32_t)t : 0;
}

static void ts_format(uint32_t t, char out[20])
{
    int64_t z = (int64_t)t / 86400 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    unsigned s = (unsigned)(t % 86400);
    snprintf(out, 20, "%04u-%02u-%02u_%02u:%02u:%02u",
             (unsigned)y % 10000, (unsigned)m % 100, (unsigned)d % 100,
             s / 3600 % 100, s / 60 % 60, s % 60);
}

static void hex_enc(const uint8_t *b, size_t n, char *out)
{
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
    out[2 * n] = 0;
}
static bool hex_dec(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) < 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        char t[3] = { s[2 * i], s[2 * i + 1], 0 };
        if (!isxdigit((unsigned char)t[0]) || !isxdigit((unsigned char)t[1])) return false;
        sscanf(t, "%x", &v);
        out[i] = (uint8_t)v;
    }
    return true;
}

/* The callsign a key derives (XPRS.md 3): the first four bech32 characters
 * of the npub, i.e. the top twenty bits of the x-only key, upper-cased. */
static void callsign_of_pub(const uint8_t pub[32], const char *prefix, char out[XWC_CALL_LEN])
{
    static const char *CS = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    uint32_t bits = ((uint32_t)pub[0] << 16) | ((uint32_t)pub[1] << 8) | pub[2];
    snprintf(out, XWC_CALL_LEN, "%s%c%c%c%c", prefix,
             toupper((unsigned char)CS[(bits >> 19) & 31]),
             toupper((unsigned char)CS[(bits >> 14) & 31]),
             toupper((unsigned char)CS[(bits >> 9) & 31]),
             toupper((unsigned char)CS[(bits >> 4) & 31]));
}

/* ---- a bounded JSON writer ---------------------------------------------- */
typedef struct { char *b; size_t cap, n; } jb_t;
static void jb_init(jb_t *j, char *b, size_t cap) { j->b = b; j->cap = cap; j->n = 0; if (cap) b[0] = 0; }
static void jb(jb_t *j, const char *fmt, ...)
{
    if (j->n + 1 >= j->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(j->b + j->n, j->cap - j->n, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    j->n += (size_t)w < j->cap - j->n ? (size_t)w : j->cap - j->n - 1;
}
/* A string value: quoted, with `"` and `\` escaped; control bytes dropped. */
static void jb_str(jb_t *j, const char *s)
{
    jb(j, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') jb(j, "\\%c", c);
        else if (c >= 0x20 && c != 0x7f) jb(j, "%c", c);
    }
    jb(j, "\"");
}

/* `"key":"value"` out of a JSON body. False when absent. */
static bool jget(const char *body, const char *key, char *out, size_t cap)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *j = body ? strstr(body, k) : NULL;
    if (!j) return false;
    j += strlen(k);
    while (*j == ' ' || *j == ':' || *j == '\t') j++;
    size_t o = 0;
    if (*j == '"') {
        j++;
        while (*j && *j != '"' && o < cap - 1) {
            if (*j == '\\' && j[1]) j++;
            out[o++] = *j++;
        }
    } else {
        while (*j && *j != ',' && *j != '}' && *j != ' ' && o < cap - 1) out[o++] = *j++;
    }
    out[o] = 0;
    return true;
}
static bool jhas(const char *body, const char *key)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    return body && strstr(body, k) != NULL;
}

/* Printable ASCII only, no quotes or backslashes, one space between words. */
static void clean_text(const char *in, char *out, size_t cap)
{
    size_t o = 0; bool sp = true;
    for (; in && *in && o < cap - 1; in++) {
        unsigned char c = (unsigned char)*in;
        if (c <= ' ' || c == 0x7f) { if (!sp && o < cap - 2) { out[o++] = ' '; sp = true; } continue; }
        if (c == '"' || c == '\\' || c > 0x7e) continue;
        out[o++] = (char)c; sp = false;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* ---- keys and verification -------------------------------------------- */
static const uint8_t *key_of(const char *call)
{
    char c[XWC_CALL_LEN];
    base_call(call, c);
    const uint8_t *k = xwc_room_pub(c);
    if (k) return k;
    return s_cb.peer_key ? s_cb.peer_key(c) : NULL;
}

static bool verified(const xprs_t *p, const char *from)
{
    const uint8_t *k = key_of(from);
    if (!k || !xprsid_verify(p, k)) return false;
    ts_seen(p);
    return true;
}

/* ---- the rooms' metadata, in the host's kv ------------------------------ */
static void rooms_load(void)
{
    s_nrooms = 0;
    const char *v = s_cb.kv_get ? s_cb.kv_get("wc_rooms") : NULL;
    if (!v) return;
    /* CALL\x1fname\x1fabout\x1fV\x1fP\x1fK\x1e ... (K, the kind, came
     * later: a record without it is a chat room) */
    const char *p = v;
    while (*p && s_nrooms < XWC_ROOMS) {
        room_t *r = &s_rooms[s_nrooms];
        memset(r, 0, sizeof *r);
        char *f[6] = { r->call, r->name, r->about, NULL, NULL, NULL };
        size_t caps[6] = { sizeof r->call, sizeof r->name, sizeof r->about, 4, 4, 4 };
        char flag[3][4] = { "", "", "" };
        f[3] = flag[0]; f[4] = flag[1]; f[5] = flag[2];
        for (int i = 0; i < 6; i++) {
            size_t o = 0;
            while (*p && *p != 0x1f && *p != 0x1e) { if (o < caps[i] - 1) f[i][o++] = *p; p++; }
            f[i][o] = 0;
            if (*p == 0x1f) p++;
        }
        if (*p == 0x1e) p++;
        if (!r->call[0]) continue;
        r->priv_room = flag[0][0] == 'p';
        r->anyone = flag[1][0] == 'a';
        r->kind = flag[2][0] == 'f' ? RK_FORUM : flag[2][0] == 'g' ? RK_GALLERY : RK_CHAT;
        char key[24], hex[80];
        snprintf(key, sizeof key, "wc_rk_%s", r->call);
        const char *h = s_cb.kv_get(key);
        if (h && snprintf(hex, sizeof hex, "%s", h) > 0 && hex_dec(hex, r->priv, 32) &&
            xprssig_public_key(r->priv, r->pub))
            r->have_key = true;
        s_nrooms++;
    }
}

static bool rooms_save(void)
{
    static char buf[XWC_ROOMS * (XWC_CALL_LEN + XWC_ROOM_NAME + XWC_ROOM_ABOUT + 10) + 1];
    size_t n = 0;
    for (int i = 0; i < s_nrooms; i++) {
        room_t *r = &s_rooms[i];
        int w = snprintf(buf + n, sizeof buf - n, "%s\x1f%s\x1f%s\x1f%c\x1f%c\x1f%c\x1e",
                         r->call, r->name, r->about, r->priv_room ? 'p' : 'u',
                         r->anyone ? 'a' : 'm', k_kinds[r->kind][0]);
        if (w < 0 || (size_t)w >= sizeof buf - n) return false;
        n += (size_t)w;
    }
    return s_cb.kv_set && s_cb.kv_set("wc_rooms", buf);
}

static room_t *room_find(const char *call)
{
    char c[XWC_CALL_LEN];
    base_call(call, c);
    for (int i = 0; i < s_nrooms; i++)
        if (strcmp(s_rooms[i].call, c) == 0) return &s_rooms[i];
    return NULL;
}

const char *xwc_room_of(const char *v)
{
    if (!v) return NULL;
    room_t *r = room_find(v);
    return r ? r->call : NULL;
}

const uint8_t *xwc_room_pub(const char *call)
{
    room_t *r = call ? room_find(call) : NULL;
    return r && r->have_key ? r->pub : NULL;
}

bool xwc_has_rooms(void) { return s_priv != NULL; }

/* ---- the record: acts, kept and replayed (XPRS.md 10.4) ---------------- */
static roster_t *roster_slot(const char *group)
{
    for (int i = 0; i < XWC_ROOMS + 1; i++)
        if (s_rosters[i].group[0] && strcmp(s_rosters[i].group, group) == 0) return &s_rosters[i];
    for (int i = 0; i < XWC_ROOMS + 1; i++)
        if (!s_rosters[i].group[0]) {
            memset(&s_rosters[i], 0, sizeof s_rosters[i]);
            snprintf(s_rosters[i].group, XWC_CALL_LEN, "%s", group);
            return &s_rosters[i];
        }
    return &s_rosters[0];    /* over the limit: share, deliberately crude */
}

static void rosters_dirty(void)
{
    for (int i = 0; i < XWC_ROOMS + 1; i++) s_rosters[i].built = false;
}

static member_t *member_find(roster_t *r, const char *call)
{
    for (int i = 0; i < r->nm; i++) if (strcmp(r->m[i].call, call) == 0) return &r->m[i];
    return NULL;
}
static member_t *member_get(roster_t *r, const char *call)
{
    member_t *m = member_find(r, call);
    if (m) return m;
    if (r->nm >= MEMBERS_MAX) return NULL;
    m = &r->m[r->nm++];
    memset(m, 0, sizeof *m);
    snprintf(m->call, XWC_CALL_LEN, "%s", call);
    return m;
}
static void away_set(roster_t *r, const char *call, uint32_t until)
{
    for (int i = 0; i < r->na; i++)
        if (strcmp(r->away[i].call, call) == 0) { r->away[i].until = until; return; }
    if (r->na >= AWAY_MAX) return;
    snprintf(r->away[r->na].call, XWC_CALL_LEN, "%s", call);
    r->away[r->na++].until = until;
}
static void away_clear(roster_t *r, const char *call)
{
    for (int i = 0; i < r->na; i++)
        if (strcmp(r->away[i].call, call) == 0) { r->away[i] = r->away[--r->na]; return; }
}
static bool away_now(roster_t *r, const char *call, uint32_t now)
{
    for (int i = 0; i < r->na; i++)
        if (strcmp(r->away[i].call, call) == 0)
            return r->away[i].until == 0 || r->away[i].until > now;
    return false;
}

static int act_cmp(const void *a, const void *b)
{
    const act_t *x = *(const act_t *const *)a, *y = *(const act_t *const *)b;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    /* On a shared ts an acceptance walks after everything else, so a grant
     * always precedes its own acceptance (26.4: membership begins at the
     * acceptance "if the grant was in force at that moment"). A total
     * order, so every walk is the same walk. The id breaks the rest. */
    int xa = x->kind == K_ACCEPT, ya = y->kind == K_ACCEPT;
    if (xa != ya) return xa - ya;
    return strcmp(x->id, y->id);
}

static void roster_build(roster_t *r, uint32_t now)
{
    const char *group = r->group;
    r->nm = r->na = r->nr = 0;
    r->next_change = 0;
    /* A: what the admin voided -- revoke:X since:T by the group itself.
     * The walk's array lives off the stack: a replay runs inside an HTTP
     * handler, under the index's query, on a task sized for JSON. */
    static const act_t *live[ACTS_MAX];
    int nlive = 0;
    for (int i = 0; i < s_nacts; i++) {
        const act_t *a = &s_acts[i];
        if (strcmp(a->group, group) != 0) continue;
        bool voided = false;
        for (int k = 0; k < s_nacts; k++) {
            const act_t *v = &s_acts[k];
            if (strcmp(v->group, group) != 0 || strcmp(v->signer, group) != 0) continue;
            if (v->kind != K_REVOKE || !v->since) continue;
            if (strcmp(v->target, a->signer) == 0 && a->ts >= v->since) { voided = true; break; }
        }
        if (!voided) live[nlive++] = a;
    }
    /* B: in order. */
    qsort(live, (size_t)nlive, sizeof live[0], act_cmp);
    /* C: walk, authority judged at the act's moment. */
    for (int i = 0; i < nlive; i++) {
        const act_t *a = live[i];
        bool is_admin = strcmp(a->signer, group) == 0;
        member_t *sm = member_find(r, a->signer);
        bool is_mod = is_admin || (sm && sm->role == R_MOD);
        bool self_act = a->kind == K_ACCEPT || a->kind == K_LEAVE;
        if (!is_admin && !is_mod && !self_act) continue;
        switch (a->kind) {
        case K_GRANT:
            if (!is_admin) break;
            {
                member_t *m = member_get(r, a->target);
                if (!m) break;
                m->role = R_INVITED;
                snprintf(m->offer, XWC_ID_LEN, "%s", a->id);
                m->offer_role = a->role;
                away_clear(r, a->target);       /* 10.10: a grant lifts */
            }
            break;
        case K_ACCEPT:
            if (sm && sm->role == R_INVITED && strcmp(sm->offer, a->target) == 0) {
                sm->role = (sm->offer_role == R_MOD && a->accept_mod) ? R_MOD : R_MEMBER;
                sm->offer[0] = 0;
            }
            break;
        case K_LEAVE:
            if (sm) { sm->role = R_NONE; sm->offer[0] = 0; }
            away_clear(r, a->signer);
            break;
        case K_REVOKE:
            if (a->until) {
                if (a->until > now) {
                    away_set(r, a->target, a->until);
                    if (!r->next_change || a->until < r->next_change) r->next_change = a->until;
                } else away_clear(r, a->target);
            } else {
                member_t *m = member_find(r, a->target);
                if (m) { m->role = R_NONE; m->offer[0] = 0; }
                away_set(r, a->target, 0);
            }
            break;
        case K_HIDE:
        case K_REMOVE:
            if (r->nr < REMOVED_MAX) snprintf(r->removed[r->nr++], XWC_ID_LEN, "%.6s", a->target);
            break;
        }
    }
    r->built = true;
    r->built_at = now;
}

static roster_t *roster(const char *group)
{
    roster_t *r = roster_slot(group);
    uint32_t now = now_s();
    if (!r->built || (r->next_change && now >= r->next_change)) roster_build(r, now);
    return r;
}

static int role_in(const char *group, const char *call)
{
    char c[XWC_CALL_LEN];
    base_call(call, c);
    if (strcmp(c, group) == 0) return R_ADMIN;
    roster_t *r = roster(group);
    member_t *m = member_find(r, c);
    if (!m) return R_NONE;
    if (away_now(r, c, now_s())) return R_NONE;
    return m->role;
}

static const char *role_name(int r)
{
    switch (r) {
    case R_INVITED: return "invited";
    case R_MEMBER:  return "member";
    case R_MOD:     return "mod";
    case R_ADMIN:   return "admin";
    default:        return "none";
    }
}
const char *xwc_role_of(const char *group, const char *call) { return role_name(role_in(group, call)); }

static bool removed_in(const char *group, const char *id)
{
    roster_t *r = roster(group);
    for (int i = 0; i < r->nr; i++) if (strcmp(r->removed[i], id) == 0) return true;
    return false;
}

/* One wire, possibly several acts (grant:A,B). Returns acts added. */
static int acts_take(const xprs_t *p, const char *wire, int len, bool persist)
{
    char group[XWC_CALL_LEN], signer[XWC_CALL_LEN], v[128], id[XPRS_ID_LEN];
    if (!xprs_get_str(p, "d", v, sizeof v)) return 0;
    base_call(v, group);
    if (!xprs_get_str(p, "f", v, sizeof v)) return 0;
    base_call(v, signer);
    if (!xprs_get_str(p, "ts", v, sizeof v)) return 0;
    uint32_t ts = ts_parse(v);
    if (!ts) return 0;
    uint32_t now = now_s();
    if (now && ts > now + FUTURE_SLACK) return 0;
    xprs_id(p, id);
    for (int i = 0; i < s_nacts; i++) if (strcmp(s_acts[i].id, id) == 0) return 0;

    act_t t;
    memset(&t, 0, sizeof t);
    snprintf(t.id, XWC_ID_LEN, "%s", id);
    snprintf(t.group, XWC_CALL_LEN, "%s", group);
    snprintf(t.signer, XWC_CALL_LEN, "%s", signer);
    t.ts = ts;
    if (xprs_get_str(p, "until", v, sizeof v)) { t.until = ts_parse(v); if (t.until && t.until - ts > MAX_UNTIL) t.until = 0; }
    if (xprs_get_str(p, "since", v, sizeof v)) t.since = ts_parse(v);
    if (xprs_get_str(p, "role", v, sizeof v)) t.role = strcmp(v, "mod") == 0 ? R_MOD : (strcmp(v, "sub") == 0 ? 99 : R_MEMBER);
    else t.role = R_MEMBER;

    int added = 0;
    char list[128];
    const char *lists[2] = { "grant", "revoke" };
    int kinds[2] = { K_GRANT, K_REVOKE };
    for (int L = 0; L < 2; L++) {
        if (!xprs_get_str(p, lists[L], list, sizeof list)) continue;
        if (kinds[L] == K_GRANT && t.role == 99) continue;    /* role:sub: not a person */
        char *save = NULL;
        for (char *c = strtok_r(list, ",", &save); c; c = strtok_r(NULL, ",", &save)) {
            if (s_nacts >= ACTS_MAX) {
                /* Losing a grant is safe, losing a revocation is not (26.5). */
                int drop = -1;
                for (int i = 0; i < s_nacts; i++) if (s_acts[i].kind != K_REVOKE) { drop = i; break; }
                if (drop < 0) drop = 0;
                memmove(&s_acts[drop], &s_acts[drop + 1], (size_t)(s_nacts - drop - 1) * sizeof s_acts[0]);
                s_nacts--;
            }
            act_t *a = &s_acts[s_nacts++];
            *a = t;
            a->kind = (uint8_t)kinds[L];
            base_call(c, a->target);
            added++;
        }
    }
    if (xprs_get_str(p, "r", v, sizeof v) && strlen(v) == XWC_ID_LEN - 1) {
        char word[16];
        int kind = 0;
        if (xprs_get_str(p, "accept", word, sizeof word)) { kind = K_ACCEPT; t.accept_mod = strcmp(word, "mod") == 0; }
        else if (xprs_get_str(p, "hide", word, sizeof word) && strcmp(word, "message") == 0) kind = K_HIDE;
        else if (xprs_get_str(p, "remove", word, sizeof word) && strcmp(word, "message") == 0) kind = K_REMOVE;
        if (kind && s_nacts < ACTS_MAX) {
            act_t *a = &s_acts[s_nacts++];
            *a = t;
            a->kind = (uint8_t)kind;
            snprintf(a->target, XWC_CALL_LEN, "%.*s", XWC_ID_LEN - 1, v);
            added++;
        }
    } else if (xprs_get_str(p, "leave", v, sizeof v) && s_nacts < ACTS_MAX) {
        act_t *a = &s_acts[s_nacts++];
        *a = t;
        a->kind = K_LEAVE;
        added++;
    }
    if (added) {
        rosters_dirty();
        if (persist && s_cb.acts_path) {
            FILE *f = fopen(s_cb.acts_path, "a");
            if (f) { fprintf(f, "%.*s\n", len, wire); fclose(f); }
        }
    }
    return added;
}

int xwc_load(void)
{
    int n = 0;
    if (!s_cb.acts_path) return 0;
    FILE *f = fopen(s_cb.acts_path, "r");
    if (!f) return 0;
    char line[XPRS_MAX_WIRE + 2];
    while (fgets(line, sizeof line, f)) {
        int len = (int)strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        xprs_t p;
        if (len && xprs_parse(line, len, &p)) n += acts_take(&p, line, len, false);
    }
    fclose(f);
    return n;
}

/* Sign [body] as [signer] (the station or a room) about [group], take it in,
 * and hand it to [out]. Returns the wire length or 0. */
static int signed_act(const char *signer, const uint8_t *priv, const char *group,
                      const char *body, char *out, int cap)
{
    char ts[20];
    ts_format(now_s(), ts);
    int n = snprintf(out, (size_t)cap, "t:moderate f:%s d:%s ts:%s %s", signer, group, ts, body);
    if (n <= 0 || n >= cap) return 0;
    n = xprsid_sign(out, n, cap, priv);
    xprs_t p;
    if (!xprs_parse(out, n, &p) || !xprs_get(&p, "sig", NULL)) return 0;
    if (!acts_take(&p, out, n, true)) return 0;
    return n;
}

static bool station_act(const char *body)
{
    char w[XPRS_MAX_WIRE + 1];
    if (!s_priv) return false;
    int n = signed_act(s_station, s_priv, s_station, body, w, sizeof w);
    if (!n) return false;
    if (s_cb.air) s_cb.air(w, n);
    return true;
}

static bool room_act(room_t *r, const char *body)
{
    char w[XPRS_MAX_WIRE + 1];
    if (!r->have_key) return false;
    int n = signed_act(r->call, r->priv, r->call, body, w, sizeof w);
    if (!n) return false;
    if (s_cb.file) s_cb.file(w, n);
    return true;
}

/* ---- init --------------------------------------------------------------- */
void xwc_init(const xwc_cb_t *cb, const char *station_call, const uint8_t *station_priv)
{
    s_cb = *cb;
    base_call(station_call ? station_call : "", s_station);
    s_priv = station_priv;
    rooms_load();
}

void xwc_reset(void)
{
    s_nacts = 0;
    memset(s_rosters, 0, sizeof s_rosters);
    memset(s_nonces, 0, sizeof s_nonces);
    memset(s_sessions, 0, sizeof s_sessions);
    s_nrooms = 0;
}

/* ---- sessions ------------------------------------------------------------ */
void xwc_nonce(char out[XWC_NONCE_LEN])
{
    uint8_t b[8];
    s_cb.random_bytes(b, sizeof b);
    hex_enc(b, sizeof b, out);
    uint32_t now = mono();
    int slot = 0;
    for (int i = 0; i < NONCES_MAX; i++) {
        if (!s_nonces[i].nonce[0] || s_nonces[i].until <= now) { slot = i; break; }
        if (s_nonces[i].until < s_nonces[slot].until) slot = i;
    }
    snprintf(s_nonces[slot].nonce, XWC_NONCE_LEN, "%s", out);
    s_nonces[slot].until = now + NONCE_LIFE;
}

int xwc_prove(const char *wire, int len, char token[XWC_TOKEN_LEN],
              char call[XWC_CALL_LEN], char *why, size_t why_cap)
{
    xprs_t p;
    char type[16], v[64];
    if (!xprs_parse(wire, len, &p)) { snprintf(why, why_cap, "malformed"); return 400; }
    xprs_type(&p, type, sizeof type);
    if (strcmp(type, "session") != 0) { snprintf(why, why_cap, "malformed"); return 400; }
    if (!xprs_get_str(&p, "f", v, sizeof v)) { snprintf(why, why_cap, "no f:"); return 400; }
    base_call(v, call);
    if (!xprs_get_str(&p, "m", v, sizeof v)) { snprintf(why, why_cap, "no such challenge"); return 403; }
    uint32_t now = mono();
    int found = -1;
    for (int i = 0; i < NONCES_MAX; i++)
        if (s_nonces[i].nonce[0] && strcmp(s_nonces[i].nonce, v) == 0 && s_nonces[i].until > now) found = i;
    if (found < 0) { snprintf(why, why_cap, "no such challenge"); return 403; }
    const uint8_t *k = key_of(call);
    if (!k) { snprintf(why, why_cap, "identity unknown"); return 409; }
    if (!xprsid_verify(&p, k)) { snprintf(why, why_cap, "not signed by that key"); return 403; }
    ts_seen(&p);
    s_nonces[found].nonce[0] = 0;
    uint8_t b[16];
    s_cb.random_bytes(b, sizeof b);
    hex_enc(b, sizeof b, token);
    int slot = 0;
    for (int i = 0; i < SESSIONS_MAX; i++) {
        if (!s_sessions[i].token[0] || s_sessions[i].until <= now) { slot = i; break; }
        if (s_sessions[i].until < s_sessions[slot].until) slot = i;
    }
    snprintf(s_sessions[slot].token, XWC_TOKEN_LEN, "%s", token);
    snprintf(s_sessions[slot].call, XWC_CALL_LEN, "%s", call);
    s_sessions[slot].until = now + SESSION_LIFE;
    return 200;
}

const char *xwc_session_call(const char *token)
{
    if (!token || !token[0]) return NULL;
    uint32_t now = mono();
    for (int i = 0; i < SESSIONS_MAX; i++)
        if (s_sessions[i].token[0] && strcmp(s_sessions[i].token, token) == 0 && s_sessions[i].until > now)
            return s_sessions[i].call;
    return NULL;
}

/* ---- the administrator -------------------------------------------------- */
static char s_admin[XWC_CALL_LEN + 80];   /* "CALL hexkey" cached from kv */

static const char *admin_rec(void)
{
    const char *v = s_cb.kv_get ? s_cb.kv_get("wc_admin") : NULL;
    if (!v || !*v) return NULL;
    snprintf(s_admin, sizeof s_admin, "%s", v);
    return s_admin;
}

const char *xwc_admin_call(void)
{
    static char call[XWC_CALL_LEN];
    const char *rec = admin_rec();
    if (!rec) return NULL;
    size_t i = 0;
    while (rec[i] && rec[i] != ' ' && i < XWC_CALL_LEN - 1) { call[i] = rec[i]; i++; }
    call[i] = 0;
    return call[0] ? call : NULL;
}

/* The session's callsign when it is the administrator's, by the key it was
 * claimed with. */
static const char *admin_of(const char *token)
{
    const char *call = xwc_session_call(token);
    const char *rec = admin_rec();
    if (!call || !rec) return NULL;
    size_t n = strlen(call);
    if (strncmp(rec, call, n) != 0 || rec[n] != ' ') return NULL;
    const uint8_t *k = key_of(call);
    if (!k) return NULL;
    char hex[65];
    hex_enc(k, 32, hex);
    return strcmp(rec + n + 1, hex) == 0 ? call : NULL;
}

static bool is_staff(const char *call)
{
    char c[XWC_CALL_LEN];
    base_call(call, c);
    const char *rec = admin_rec();
    if (rec) {
        size_t n = strlen(c);
        const uint8_t *k = key_of(c);
        char hex[65];
        if (k) {
            hex_enc(k, 32, hex);
            if (strncmp(rec, c, n) == 0 && rec[n] == ' ' && strcmp(rec + n + 1, hex) == 0) return true;
        }
    }
    return role_in(s_station, c) == R_MOD;
}

static const char *kv_or(const char *key, const char *def)
{
    const char *v = s_cb.kv_get ? s_cb.kv_get(key) : NULL;
    return v && *v ? v : def;
}
bool        xwc_local_private(void) { return strcmp(kv_or("wc_lp", "1"), "0") != 0; }
const char *xwc_who_may_post(void)  { return kv_or("wc_post", "everyone"); }
const char *xwc_who_may_alert(void) { return kv_or("wc_alert", "moderators"); }

static void moderation_state(jb_t *j, const char *token, bool you)
{
    const char *call = xwc_session_call(token);
    roster_t *r = roster(s_station);
    int role = call ? role_in(s_station, call) : R_NONE;
    jb(j, ",\"station\":\"%s\",\"role\":\"%s\"", s_station, role_name(role));
    if (role == R_INVITED && call) {
        char c[XWC_CALL_LEN];
        base_call(call, c);
        member_t *m = member_find(r, c);
        if (m && m->offer[0]) jb(j, ",\"offer\":\"%s\",\"offer_role\":\"%s\"", m->offer,
                                 m->offer_role == R_MOD ? "mod" : "member");
    }
    if (!you && role != R_MOD) return;
    uint32_t now = now_s();
    jb(j, ",\"moderators\":[");
    int k = 0;
    for (int i = 0; i < r->nm; i++)
        if (r->m[i].role == R_MOD && !away_now(r, r->m[i].call, now)) jb(j, "%s\"%s\"", k++ ? "," : "", r->m[i].call);
    jb(j, "],\"invited\":[");
    k = 0;
    for (int i = 0; i < r->nm; i++)
        if (r->m[i].role == R_INVITED) jb(j, "%s\"%s\"", k++ ? "," : "", r->m[i].call);
    jb(j, "],\"away\":[");
    k = 0;
    for (int i = 0; i < r->na; i++) {
        if (r->away[i].until && r->away[i].until <= now) continue;
        jb(j, "%s{\"call\":\"%s\"", k++ ? "," : "", r->away[i].call);
        if (r->away[i].until) { char ts[20]; ts_format(r->away[i].until, ts); ts[10] = 'T'; jb(j, ",\"until\":\"%sZ\"", ts); }
        jb(j, "}");
    }
    jb(j, "]");
}

static int admin_state(jb_t *j, const char *token, bool local)
{
    const char *admin = xwc_admin_call();
    bool you = admin_of(token) != NULL;
    jb(j, "{\"ok\":true,\"admin\":");
    if (admin) jb_str(j, admin); else jb(j, "null");
    jb(j, ",\"you\":%s,\"local\":%s,\"claimable\":%s", you ? "true" : "false",
       local ? "true" : "false", (!admin && local) ? "true" : "false");
    if (you)
        jb(j, ",\"settings\":{\"local_private\":%s,\"who_may_post\":\"%s\",\"who_may_alert\":\"%s\"}",
           xwc_local_private() ? "true" : "false", xwc_who_may_post(), xwc_who_may_alert());
    moderation_state(j, token, you);
    jb(j, "}");
    return 200;
}

int xwc_admin_json(char *buf, size_t cap, const char *token, bool local)
{
    jb_t j;
    jb_init(&j, buf, cap);
    return admin_state(&j, token, local);
}

static int refuse(char *buf, size_t cap, int status, const char *why)
{
    jb_t j;
    jb_init(&j, buf, cap);
    jb(&j, "{\"ok\":false,\"error\":");
    jb_str(&j, why);
    jb(&j, "}");
    return status;
}

int xwc_admin_post(const char *body, bool local, char *buf, size_t cap)
{
    char action[16], token[XWC_TOKEN_LEN] = "", who[XWC_CALL_LEN], v[XWC_CALL_LEN];
    if (!jget(body, "action", action, sizeof action)) return refuse(buf, cap, 400, "malformed");
    jget(body, "session", token, sizeof token);
    if (strcmp(action, "claim") == 0) {
        const char *call = xwc_session_call(token);
        if (!call) return refuse(buf, cap, 403, "prove your key first (a session)");
        if (xwc_admin_call()) return refuse(buf, cap, 409, "this station has an administrator");
        if (!local) return refuse(buf, cap, 403, "claim it from the station's own network");
        const uint8_t *k = key_of(call);
        if (!k) return refuse(buf, cap, 409, "identity unknown");
        char rec[XWC_CALL_LEN + 80];
        char hex[65];
        hex_enc(k, 32, hex);
        snprintf(rec, sizeof rec, "%s %s", call, hex);
        if (!s_cb.kv_set || !s_cb.kv_set("wc_admin", rec)) return refuse(buf, cap, 500, "could not keep it");
        /* The administrator moderates too: offered, and accepted by their
         * own signature in the page (10.3.1). */
        if (s_priv && role_in(s_station, call) != R_MOD) {
            char b[40];
            snprintf(b, sizeof b, "grant:%s role:mod", call);
            station_act(b);
        }
        jb_t j; jb_init(&j, buf, cap);
        return admin_state(&j, token, local);
    }
    if (!admin_of(token)) return refuse(buf, cap, 403, "not the administrator");
    if (strcmp(action, "release") == 0) {
        s_cb.kv_set("wc_admin", "");
        jb_t j; jb_init(&j, buf, cap);
        return admin_state(&j, token, local);
    }
    if (strcmp(action, "set") == 0) {
        if (!jhas(body, "settings")) return refuse(buf, cap, 400, "no settings");
        if (jget(body, "local_private", v, sizeof v)) {
            if (strcmp(v, "true") && strcmp(v, "false")) return refuse(buf, cap, 400, "local_private must be true or false");
            s_cb.kv_set("wc_lp", v[0] == 't' ? "1" : "0");
        }
        if (jget(body, "who_may_post", v, sizeof v)) {
            if (strcmp(v, "everyone") && strcmp(v, "local") && strcmp(v, "nobody"))
                return refuse(buf, cap, 400, "who_may_post must be everyone, local or nobody");
            s_cb.kv_set("wc_post", v);
        }
        if (jget(body, "who_may_alert", v, sizeof v)) {
            if (strcmp(v, "moderators") && strcmp(v, "everyone"))
                return refuse(buf, cap, 400, "who_may_alert must be moderators or everyone");
            s_cb.kv_set("wc_alert", v);
        }
        jb_t j; jb_init(&j, buf, cap);
        return admin_state(&j, token, local);
    }
    if (strcmp(action, "appoint") == 0 || strcmp(action, "dismiss") == 0 || strcmp(action, "lift") == 0) {
        if (!jget(body, "call", v, sizeof v)) return refuse(buf, cap, 400, "a callsign, please");
        base_call(v, who);
        if (!call_ok(who)) return refuse(buf, cap, 400, "a callsign, please");
        if (strcmp(who, s_station) == 0) return refuse(buf, cap, 400, "not the station itself");
        char b[40];
        snprintf(b, sizeof b, action[0] == 'a' ? "grant:%s role:mod" : "grant:%s", who);
        if (!station_act(b)) return refuse(buf, cap, 409, "this station cannot sign");
        jb_t j; jb_init(&j, buf, cap);
        return admin_state(&j, token, local);
    }
    return refuse(buf, cap, 400, "action must be claim, release, set, appoint, dismiss or lift");
}

/* ---- hosted rooms --------------------------------------------------------- */
static bool room_member(room_t *r, const char *call)
{
    if (!call) return false;
    int role = role_in(r->call, call);
    return role == R_MEMBER || role == R_MOD || role == R_ADMIN;
}

bool xwc_may_read_room(const char *room, const char *token)
{
    room_t *r = room_find(room);
    if (!r) return false;
    if (!r->priv_room) return true;
    return room_member(r, xwc_session_call(token)) || admin_of(token) != NULL;
}

static void room_json(jb_t *j, room_t *r, const char *who, bool admin)
{
    int role = who ? role_in(r->call, who) : R_NONE;
    roster_t *ro = roster(r->call);
    bool members_only = r->priv_room || !r->anyone;
    bool sent_away = who && away_now(ro, who, now_s());
    jb(j, "{\"call\":\"%s\",\"name\":", r->call);
    jb_str(j, r->name);
    jb(j, ",\"about\":");
    jb_str(j, r->about);
    jb(j, ",\"kind\":\"%s\",\"visibility\":\"%s\",\"posting\":\"%s\",\"role\":\"%s\"",
       k_kinds[r->kind], r->priv_room ? "private" : "public", r->anyone ? "anyone" : "members", role_name(role));
    if (role == R_INVITED && who) {
        member_t *m = member_find(ro, who);
        if (m && m->offer[0]) jb(j, ",\"offer\":\"%s\",\"offer_role\":\"%s\"", m->offer,
                                 m->offer_role == R_MOD ? "mod" : "member");
    }
    bool can = who && !sent_away && (!members_only || role == R_MEMBER || role == R_MOD || role == R_ADMIN);
    jb(j, ",\"can_post\":%s", can ? "true" : "false");
    if (admin || role == R_MOD) {
        static const int roles[3] = { R_MEMBER, R_MOD, R_INVITED };
        static const char *names[3] = { "members", "moderators", "invited" };
        for (int t = 0; t < 3; t++) {
            jb(j, ",\"%s\":[", names[t]);
            int k = 0;
            for (int i = 0; i < ro->nm; i++)
                if (ro->m[i].role == roles[t]) jb(j, "%s\"%s\"", k++ ? "," : "", ro->m[i].call);
            jb(j, "]");
        }
    }
    jb(j, "}");
}

static int rooms_list(jb_t *j, const char *token, const char *created)
{
    char who[XWC_CALL_LEN] = "";
    const char *sc = xwc_session_call(token);
    if (sc) base_call(sc, who);
    bool admin = admin_of(token) != NULL;
    jb(j, "{\"ok\":true,\"rooms\":[");
    int k = 0;
    for (int i = 0; i < s_nrooms; i++) {
        room_t *r = &s_rooms[i];
        if (r->priv_room && !admin && (!who[0] || role_in(r->call, who) == R_NONE)) continue;
        if (k++) jb(j, ",");
        room_json(j, r, who[0] ? who : NULL, admin);
    }
    jb(j, "]");
    if (created) jb(j, ",\"room\":\"%s\"", created);
    jb(j, "}");
    return 200;
}

int xwc_rooms_json(char *buf, size_t cap, const char *token)
{
    jb_t j; jb_init(&j, buf, cap);
    return rooms_list(&j, token, NULL);
}

int xwc_rooms_post(const char *body, char *buf, size_t cap)
{
    char action[16], token[XWC_TOKEN_LEN] = "", v[XWC_ROOM_ABOUT + 40];
    if (!jget(body, "action", action, sizeof action)) return refuse(buf, cap, 400, "malformed");
    jget(body, "session", token, sizeof token);
    if (!admin_of(token)) return refuse(buf, cap, 403, "only the station's administrator manages rooms");
    static const char *const shape = "visibility is public or private; posting members or anyone; kind chat, forum or gallery";
    char vis[12] = "", post[12] = "", kind[12] = "";
    int rk = -1;
    if (jget(body, "visibility", vis, sizeof vis) && strcmp(vis, "public") && strcmp(vis, "private"))
        return refuse(buf, cap, 400, shape);
    if (jget(body, "posting", post, sizeof post) && strcmp(post, "members") && strcmp(post, "anyone"))
        return refuse(buf, cap, 400, shape);
    if (jget(body, "kind", kind, sizeof kind)) {
        for (int k = 0; k < (int)(sizeof k_kinds / sizeof k_kinds[0]); k++)
            if (strcmp(kind, k_kinds[k]) == 0) rk = k;
        if (rk < 0) return refuse(buf, cap, 400, shape);
    }

    if (strcmp(action, "create") == 0) {
        if (s_nrooms >= XWC_ROOMS) return refuse(buf, cap, 409, "this station holds as many rooms as it can");
        char name[XWC_ROOM_NAME] = "";
        if (jget(body, "name", v, sizeof v)) clean_text(v, name, sizeof name);
        if (!name[0]) return refuse(buf, cap, 400, "a room needs a name");
        room_t *r = &s_rooms[s_nrooms];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%s", name);
        if (jget(body, "about", v, sizeof v)) clean_text(v, r->about, sizeof r->about);
        r->priv_room = strcmp(vis, "private") == 0;
        r->anyone = !r->priv_room && (!post[0] || strcmp(post, "anyone") == 0);
        r->kind = rk < 0 ? RK_CHAT : (uint8_t)rk;
        /* The room's key, and the callsign it derives (10.1), unique here. */
        for (int tries = 0; tries < 8; tries++) {
            s_cb.random_bytes(r->priv, 32);
            if (!xprssig_public_key(r->priv, r->pub)) continue;
            callsign_of_pub(r->pub, "X5", r->call);
            if (!room_find(r->call) || room_find(r->call) == r) { r->have_key = true; break; }
        }
        if (!r->have_key) return refuse(buf, cap, 503, "could not make a key; try again");
        char key[24], hex[65];
        snprintf(key, sizeof key, "wc_rk_%s", r->call);
        hex_enc(r->priv, 32, hex);
        if (!s_cb.kv_set(key, hex)) return refuse(buf, cap, 500, "could not keep the key");
        s_nrooms++;
        if (!rooms_save()) { s_nrooms--; return refuse(buf, cap, 500, "could not keep the room"); }
        const char *admin = xwc_admin_call();
        if (admin) {
            char b[40];
            snprintf(b, sizeof b, "grant:%s role:mod", admin);
            room_act(r, b);
        }
        jb_t j; jb_init(&j, buf, cap);
        return rooms_list(&j, token, r->call);
    }
    if (!jget(body, "call", v, sizeof v)) return refuse(buf, cap, 404, "no such room");
    room_t *r = room_find(v);
    if (!r) return refuse(buf, cap, 404, "no such room");
    if (strcmp(action, "update") == 0) {
        if (jhas(body, "name")) {
            char name[XWC_ROOM_NAME] = "";
            if (jget(body, "name", v, sizeof v)) clean_text(v, name, sizeof name);
            if (!name[0]) return refuse(buf, cap, 400, "a room needs a name");
            snprintf(r->name, sizeof r->name, "%s", name);
        }
        if (jhas(body, "about")) { v[0] = 0; jget(body, "about", v, sizeof v); clean_text(v, r->about, sizeof r->about); }
        if (vis[0]) r->priv_room = strcmp(vis, "private") == 0;
        if (post[0]) r->anyone = strcmp(post, "anyone") == 0;
        if (rk >= 0) r->kind = (uint8_t)rk;
        if (r->priv_room) r->anyone = false;
        rooms_save();
        jb_t j; jb_init(&j, buf, cap);
        return rooms_list(&j, token, r->call);
    }
    if (strcmp(action, "close") == 0) {
        int i = (int)(r - s_rooms);
        memmove(&s_rooms[i], &s_rooms[i + 1], (size_t)(s_nrooms - i - 1) * sizeof s_rooms[0]);
        s_nrooms--;
        rooms_save();
        jb_t j; jb_init(&j, buf, cap);
        return rooms_list(&j, token, NULL);
    }
    if (strcmp(action, "add") == 0 || strcmp(action, "appoint") == 0 || strcmp(action, "remove") == 0) {
        char who[XWC_CALL_LEN], m[XWC_CALL_LEN];
        if (!jget(body, "member", m, sizeof m)) return refuse(buf, cap, 400, "a callsign, please");
        base_call(m, who);
        if (!call_ok(who) || strcmp(who, r->call) == 0) return refuse(buf, cap, 400, "a callsign, please");
        char b[40];
        snprintf(b, sizeof b, action[0] == 'a' && action[1] == 'd' ? "grant:%s"
                              : action[0] == 'a' ? "grant:%s role:mod" : "revoke:%s", who);
        if (!room_act(r, b)) return refuse(buf, cap, 409, "this station does not hold that room");
        jb_t j; jb_init(&j, buf, cap);
        return rooms_list(&j, token, r->call);
    }
    return refuse(buf, cap, 400, "action must be create, update, close, add, appoint or remove");
}

bool xwc_is_uploader(const char *token)
{
    if (admin_of(token)) return true;
    const char *c = xwc_session_call(token);
    if (!c) return false;
    if (is_staff(c)) return true;
    for (int i = 0; i < s_nrooms; i++) {
        int role = role_in(s_rooms[i].call, c);
        if (role == R_MOD || role == R_ADMIN) return true;
    }
    return false;
}

/* ---- the index of a forum or gallery room ------------------------------- */
/* Claimed for one answer and let go: the fold's tables are the size of the
 * window, and a board has no RAM to keep them between asks. */
typedef struct {
    char     id[XWC_ID_LEN];        /* the edition shown, the newest */
    char     title[41];
    char     from[XWC_CALL_LEN];
    char     tags[32];
    uint32_t ts, last;
    uint16_t replies, editions;
} topic_t;
typedef struct { char id[XWC_ID_LEN]; uint8_t topic; } edition_t;
typedef struct { char thread[XWC_ID_LEN]; uint32_t ts; } comment_t;
typedef struct {
    char      room[XWC_CALL_LEN];
    xprs_t    p;                    /* here, not on the caller's stack: the
                                       rows arrive inside the index's query */
    topic_t   t[XWC_TOPICS_MAX];   int nt;
    edition_t e[XWC_TOPICS_MAX * 2]; int ne;
    comment_t c[XWC_TOPICS_WINDOW]; int nc;
} topics_t;
static topics_t *s_tp;

int xwc_topics_begin(const char *call, const char *token, char *buf, size_t cap)
{
    room_t *r = call ? room_find(call) : NULL;
    if (!r || !xwc_may_read_room(r->call, token)) return refuse(buf, cap, 404, "no such room");
    if (r->kind != RK_FORUM && r->kind != RK_GALLERY)
        return refuse(buf, cap, 400, "not a forum or gallery room");
    free(s_tp);
    s_tp = calloc(1, sizeof *s_tp);
    if (!s_tp) return refuse(buf, cap, 503, "no room for the index now; try again");
    snprintf(s_tp->room, sizeof s_tp->room, "%s", r->call);
    return 200;
}

void xwc_topics_row(const char *wire, int len, uint32_t ts)
{
    topics_t *T = s_tp;
    if (!T || !xprs_parse(wire, len, &T->p)) return;
    const xprs_t *p = &T->p;
    char type[8], v[48], from[XWC_CALL_LEN], id[XPRS_ID_LEN], title[41];
    xprs_type(p, type, sizeof type);
    if (strcmp(type, "blog") != 0) return;
    /* A part alone is never the post (7.6); this engine holds no sets. */
    if (xprs_get(p, "n", NULL) || xprs_get(p, "remove", NULL)) return;
    if (!xprs_get_str(p, "f", v, sizeof v)) return;
    base_call(v, from);
    if (away_now(roster(T->room), from, now_s())) return;
    xprs_id(p, id);
    if (removed_in(T->room, id)) return;
    if (xprs_get_str(p, "title", title, sizeof title)) {
        /* Editions of one topic: the same author, the same title (18.1). */
        int k = 0;
        while (k < T->nt && (strcmp(T->t[k].from, from) || strcmp(T->t[k].title, title))) k++;
        if (k == T->nt) {
            if (T->nt >= XWC_TOPICS_MAX) return;
            topic_t *t = &T->t[T->nt++];
            snprintf(t->title, sizeof t->title, "%s", title);
            snprintf(t->from, sizeof t->from, "%s", from);
        }
        topic_t *t = &T->t[k];
        if (!t->editions++ || ts >= t->ts) {
            snprintf(t->id, sizeof t->id, "%s", id);
            t->ts = ts;
            if (!xprs_get_str(p, "tag", t->tags, sizeof t->tags)) t->tags[0] = 0;
        }
        if (T->ne < XWC_TOPICS_MAX * 2) {
            snprintf(T->e[T->ne].id, XWC_ID_LEN, "%s", id);
            T->e[T->ne++].topic = (uint8_t)k;
        }
    } else if (xprs_get(p, "r", NULL) && T->nc < XWC_TOPICS_WINDOW) {
        /* A comment's topic: root: when it says, else the post it answers. */
        if (!xprs_get_str(p, "root", v, sizeof v)) xprs_get_str(p, "r", v, sizeof v);
        snprintf(T->c[T->nc].thread, XWC_ID_LEN, "%.6s", v);
        T->c[T->nc++].ts = ts;
    }
}

static int topic_cmp(const void *a, const void *b)
{
    const topic_t *x = a, *y = b;
    return x->last == y->last ? 0 : x->last > y->last ? -1 : 1;
}

int xwc_topics_end(char *buf, size_t cap)
{
    topics_t *T = s_tp;
    if (!T) return refuse(buf, cap, 500, "no index begun");
    for (int i = 0; i < T->nc; i++)
        for (int k = 0; k < T->ne; k++) {
            if (strcmp(T->e[k].id, T->c[i].thread) != 0) continue;
            topic_t *t = &T->t[T->e[k].topic];
            t->replies++;
            if (T->c[i].ts > t->last) t->last = T->c[i].ts;
            break;
        }
    for (int k = 0; k < T->nt; k++) if (T->t[k].ts > T->t[k].last) T->t[k].last = T->t[k].ts;
    qsort(T->t, (size_t)T->nt, sizeof T->t[0], topic_cmp);
    jb_t j; jb_init(&j, buf, cap);
    jb(&j, "{\"ok\":true,\"room\":\"%s\",\"topics\":[", T->room);
    /* A topic whole or not at all: the answer stays JSON when the buffer is
     * short, and what falls off is the oldest. */
    char one[200];
    for (int k = 0; k < T->nt; k++) {
        topic_t *t = &T->t[k];
        jb_t o; jb_init(&o, one, sizeof one);
        jb(&o, "%s{\"id\":\"%s\",\"title\":", k ? "," : "", t->id);
        jb_str(&o, t->title);
        jb(&o, ",\"from\":\"%s\",\"ts\":%lu,\"replies\":%u,\"last\":%lu,\"edited\":%s",
           t->from, (unsigned long)t->ts, (unsigned)t->replies, (unsigned long)t->last,
           t->editions > 1 ? "true" : "false");
        if (t->tags[0]) { jb(&o, ",\"tags\":"); jb_str(&o, t->tags); }
        jb(&o, "}");
        if (o.n + 1 >= sizeof one || j.n + o.n + 3 >= cap) break;
        jb(&j, "%s", one);
    }
    jb(&j, "]}");
    free(T);
    s_tp = NULL;
    return 200;
}

/* ---- the rules on reading and sending ---------------------------------- */
bool xwc_hide_row(const char *group, const char *id, const char *from,
                  const char *type, bool scope_local, bool local_peer)
{
    if (!local_peer && xwc_local_private() && scope_local) return true;
    char g[XWC_CALL_LEN];
    base_call(group ? group : s_station, g);
    if (!g[0]) return false;
    if (id && removed_in(g, id)) return true;
    bool safety = type && (!strcmp(type, "sos") || !strcmp(type, "warning") || !strcmp(type, "info"));
    if (from && !safety) {
        char c[XWC_CALL_LEN];
        base_call(from, c);
        if (away_now(roster(g), c, now_s())) return true;
    }
    return false;
}

int xwc_moderation_json(char *buf, size_t cap, const char *group)
{
    jb_t j; jb_init(&j, buf, cap);
    char g[XWC_CALL_LEN];
    base_call(group ? group : s_station, g);
    roster_t *r = roster(g);
    uint32_t now = now_s();
    jb(&j, "\"moderation\":{\"removed\":[");
    for (int i = 0; i < r->nr; i++) jb(&j, "%s\"%s\"", i ? "," : "", r->removed[i]);
    jb(&j, "],\"away\":[");
    int k = 0;
    for (int i = 0; i < r->na; i++)
        if (!r->away[i].until || r->away[i].until > now) jb(&j, "%s\"%s\"", k++ ? "," : "", r->away[i].call);
    jb(&j, "]}");
    return (int)j.n;
}

/* `title:` is a label (XPRS.md 18.1): lowercase letters, digits and `-`,
 * forty at most, a letter or digit first. */
static bool label_ok(const char *v, int n)
{
    if (n < 1 || n > 40 || v[0] == '-') return false;
    for (int i = 0; i < n; i++)
        if (!((v[i] >= 'a' && v[i] <= 'z') || (v[i] >= '0' && v[i] <= '9') || v[i] == '-')) return false;
    return true;
}

/* What a `t:blog` in a forum (or gallery) room must be (18, 18.5): a topic
 * has a title; a comment has none and names its post; a correction or a
 * withdrawal names what it acts on. NULL when it is one of those. */
static const char *forum_shape(const xprs_t *p)
{
    int n = 0;
    const char *t = xprs_get(p, "title", &n);
    if (t) return label_ok(t, n) ? NULL : "title: is a label: lowercase letters, digits and -";
    if (xprs_get(p, "r", NULL) || xprs_get(p, "edit", NULL)) return NULL;
    return "a post needs a title: to open a topic, or r: to comment on one";
}

int xwc_gate(const char *wire, int len, bool local_peer,
             char id[XWC_ID_LEN], char *why, size_t why_cap)
{
    xprs_t p;
    char type[16], fromv[XWC_CALL_LEN + 4], from[XWC_CALL_LEN], d[XWC_CALL_LEN + 4] = "", v[32];
    id[0] = 0;
    if (!xprs_parse(wire, len, &p)) return 0;
    xprs_type(&p, type, sizeof type);
    if (!xprs_get_str(&p, "f", fromv, sizeof fromv)) return 0;
    base_call(fromv, from);
    xprs_get_str(&p, "d", d, sizeof d);
    xprs_id(&p, id);
    const char *room = xwc_room_of(d);

    if (strcmp(type, "moderate") == 0) {
        char target[XWC_CALL_LEN];
        base_call(d, target);
        if (strcmp(target, s_station) != 0 && !room) { snprintf(why, why_cap, "only this station's rooms"); return -403; }
        if (strcmp(from, s_station) == 0 || strcmp(from, target) == 0) { snprintf(why, why_cap, "not as this station"); return -403; }
        if (!verified(&p, from)) { snprintf(why, why_cap, "not signed by a key this station knows"); return -403; }
        bool self = xprs_get(&p, "accept", NULL) || xprs_get(&p, "leave", NULL);
        if (!self) {
            if (xprs_get(&p, "grant", NULL)) { snprintf(why, why_cap, "only the administrator appoints"); return -403; }
            if (role_in(target, from) != R_MOD) { snprintf(why, why_cap, "not a moderator here"); return -403; }
        }
        if (!acts_take(&p, wire, len, true)) { snprintf(why, why_cap, "the act was not taken"); return -400; }
        if (room) { if (s_cb.file) s_cb.file(wire, len); }
        else if (s_cb.air) s_cb.air(wire, len);
        return 1;
    }

    const char *post = xwc_who_may_post();
    if (strcmp(post, "nobody") == 0) { snprintf(why, why_cap, "this station is read-only"); return -403; }
    if (strcmp(post, "local") == 0 && !local_peer) { snprintf(why, why_cap, "only from this station's network"); return -403; }

    if (room) {
        room_t *r = room_find(room);
        bool blog = strcmp(type, "blog") == 0, like = strcmp(type, "reaction") == 0;
        const char *bad;
        if (r->kind == RK_GALLERY) {
            /* A category of photographs (spec/API-HTTP.md): events are titled
             * posts, pictures are messages with a preview and their t:file
             * companions, likes are reactions. Only the room's staff posts;
             * readers like. */
            if (blog) {
                if ((bad = forum_shape(&p))) { snprintf(why, why_cap, "%s", bad); return -400; }
                if (!xprs_get(&p, "title", NULL) && !xprs_get(&p, "n", NULL)) {
                    snprintf(why, why_cap, "an event is a post with a title");
                    return -400;
                }
            } else if (strcmp(type, "message") && strcmp(type, "file") && !like) {
                snprintf(why, why_cap, "a gallery takes events, pictures and likes");
                return -403;
            }
        } else if (blog) {
            if (r->kind != RK_FORUM) { snprintf(why, why_cap, "posts go in a forum room"); return -403; }
            if ((bad = forum_shape(&p))) { snprintf(why, why_cap, "%s", bad); return -400; }
        } else if (strcmp(type, "message") && !like) {
            snprintf(why, why_cap, "only messages and reactions in a room");
            return -403;
        }
        if (!verified(&p, from)) { snprintf(why, why_cap, "identity unknown"); return -409; }
        if (away_now(roster(room), from, now_s())) { snprintf(why, why_cap, "you were sent away from this room"); return -403; }
        if (r->kind == RK_GALLERY) {
            int role = role_in(r->call, from);
            bool staff = role == R_MOD || role == R_ADMIN || is_staff(from);
            if (!like && !staff) { snprintf(why, why_cap, "only the moderators add events and pictures here"); return -403; }
            /* A like from anyone who may read the room. */
            if (like && r->priv_room && !room_member(r, from) && !staff) {
                snprintf(why, why_cap, "only members like in this room");
                return -403;
            }
        } else if ((r->priv_room || !r->anyone) && !room_member(r, from)) {
            snprintf(why, why_cap, "only members post in this room");
            return -403;
        }
        if (s_cb.file) s_cb.file(wire, len);
        return 1;
    }

    /* Somebody sent away may not post, except a call for help (10.7). */
    if (strcmp(type, "sos") != 0 && away_now(roster(s_station), from, now_s())) {
        snprintf(why, why_cap, "you were sent away from this station's rooms");
        return -403;
    }
    if ((strcmp(type, "warning") == 0 || strcmp(type, "info") == 0) &&
        !(xprs_get_str(&p, "remove", v, sizeof v) && strncmp(v, type, strlen(type)) == 0) &&
        strcmp(xwc_who_may_alert(), "everyone") != 0 && !is_staff(from)) {
        snprintf(why, why_cap, "on this station only moderators post alerts");
        return -403;
    }
    return 0;
}

int xwc_services_json(char *buf, size_t cap, const char *tabs_csv, bool storage)
{
    jb_t j; jb_init(&j, buf, cap);
    jb(&j, "\"station\":\"%s\",\"page\":{\"tabs\":[", s_station);
    char tabs[96];
    snprintf(tabs, sizeof tabs, "%s", tabs_csv ? tabs_csv : "chat,traffic");
    char *save = NULL;
    int k = 0;
    for (char *t = strtok_r(tabs, ",", &save); t; t = strtok_r(NULL, ",", &save)) jb(&j, "%s\"%s\"", k++ ? "," : "", t);
    jb(&j, "],\"session\":true,\"mailbox\":true,\"admin\":true,\"moderation\":%s,\"rooms\":%s,\"alerts\":\"%s\"",
       s_priv ? "true" : "false", s_priv ? "true" : "false", xwc_who_may_alert());
    /* And keeps photographs: gallery rooms, where a card gives it the room. */
    if (s_priv && storage) jb(&j, ",\"gallery\":true");
    jb(&j, "}");
    return (int)j.n;
}
