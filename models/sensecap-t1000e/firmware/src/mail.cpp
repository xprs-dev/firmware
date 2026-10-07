/* The card as a post office (mail.h). */
#include <Arduino.h>
#include <string.h>

#include "mail.h"
#include "nrf_flash.h"
#include "nrf_station.h"

extern "C" {
#include "xprs.h"
#include "xprsid.h"
#include "xprs_auth.h"
#include "bech32.h"
#include "xmb.h"
}

/* The flash map (update.cpp): 0xE7000..0xED000 is the free 24 KB between
 * the backup image slot and the filesystem. */
#define MAIL_BASE   0xE7000u
#define MAIL_PAGES  6

#define KEYS        32
#define LEDGER      24
#define LEDGER_DAYS 30
#define PENDQ       4
#define DELIVERQ    2
#define HERE_MS     600000UL       /* the recipient heard this recently: no need to hold */
#define SNAP_MS     3600000UL      /* the ledger is written at most hourly */

static mail_cfg_t s_cfg;
static xmb_t      s_mb;
static bool       s_up;

/* ── The store on flash ─────────────────────────────────────────────── */

static bool st_read(void *c, uint32_t off, void *buf, int len)
{
    (void)c;
    memcpy(buf, (const void *)(MAIL_BASE + off), (size_t)len);
    return true;
}

static bool st_write(void *c, uint32_t off, const void *buf, int len)
{
    (void)c;
    static uint32_t words[XMB_REC / 4];          /* nrf_flash wants words */
    if (len > (int)sizeof words) return false;
    memcpy(words, buf, (size_t)len);
    return nrf_flash_write(MAIL_BASE + off, words, (uint32_t)len / 4);
}

static bool st_erase(void *c, int pg)
{
    (void)c;
    return nrf_flash_erase(MAIL_BASE + (uint32_t)pg * XMB_PAGE);
}

/* ── Keys: owners first, then identities that certify themselves ─────── */

static struct { char call[12]; uint8_t pub[32]; } s_keys[KEYS];
static int s_keys_pos;

static bool key_of(const char *call, uint8_t pub[32])
{
    if (xauth_owner_key_of(call, pub)) return true;
    for (int i = 0; i < KEYS; i++)
        if (s_keys[i].call[0] && !strcmp(s_keys[i].call, call)) {
            memcpy(pub, s_keys[i].pub, 32);
            return true;
        }
    return false;
}

/* A t:identity whose callsign derives from its own k: (section 3) and whose
 * signature verifies under it: the key is that callsign's, whoever relayed
 * it. Nothing else teaches the card a key. */
static void key_learn(const xprs_t *p, const char *from)
{
    char npub[80] = "", hrp[8];
    uint8_t pub[40];
    size_t n = sizeof pub;
    if (!xprs_get_str(p, "k", npub, sizeof npub) || !xauth_call_matches_npub(from, npub)) return;
    if (bech32_decode(npub, hrp, pub, &n) != 0 || n != 32 || strcmp(hrp, "npub")) return;
    if (!xprsid_verify(p, pub)) return;
    uint8_t have[32];
    if (key_of(from, have) && !memcmp(have, pub, 32)) return;
    int slot = s_keys_pos;
    for (int i = 0; i < KEYS; i++) if (!strcmp(s_keys[i].call, from)) slot = i;
    snprintf(s_keys[slot].call, sizeof s_keys[slot].call, "%s", from);
    memcpy(s_keys[slot].pub, pub, 32);
    if (slot == s_keys_pos) s_keys_pos = (s_keys_pos + 1) % KEYS;
}

static bool verified(const xprs_t *p, const char *from)
{
    uint8_t pub[32];
    return key_of(from, pub) && xprsid_verify(p, pub);
}

/* ── Contacts: who the card met in the last month ──────────────────────
 * What the card carries custody mail for (the precedent is xprsindex.c's:
 * the ledger decides what is CARRIED, never the eviction class). Days come
 * from the clock; with no clock a contact is dated 0 and never ages out. */
static struct { char call[10]; uint16_t day; } s_led[LEDGER];
static bool     s_led_dirty;
static uint32_t s_led_snap_ms;
static int8_t   s_net = -1;

static uint16_t today(void) { uint32_t t = nst_now(); return t ? (uint16_t)(t / 86400u) : 0; }

static bool ledger_has(const char *call)
{
    uint16_t d = today();
    for (int i = 0; i < LEDGER; i++)
        if (s_led[i].call[0] && !strcmp(s_led[i].call, call) &&
            (!d || !s_led[i].day || d - s_led[i].day <= LEDGER_DAYS))
            return true;
    return false;
}

static void ledger_touch(const char *call)
{
    uint16_t d = today();
    int slot = -1, oldest = 0;
    for (int i = 0; i < LEDGER; i++) {
        if (!strcmp(s_led[i].call, call)) { slot = i; break; }
        if (!s_led[i].call[0]) { if (slot < 0) slot = i; continue; }
        if (s_led[i].day < s_led[oldest].day) oldest = i;
    }
    if (slot < 0) slot = oldest;
    if (strcmp(s_led[slot].call, call) || s_led[slot].day != d) s_led_dirty = true;
    snprintf(s_led[slot].call, sizeof s_led[slot].call, "%s", call);
    s_led[slot].day = d;
}

/* The state blob: version, adopted network, the ledger. */
static void state_save(void)
{
    uint8_t b[3 + LEDGER * 10];
    int n = 0;
    b[n++] = 1;
    b[n++] = (uint8_t)s_net;
    b[n++] = LEDGER;
    for (int i = 0; i < LEDGER; i++) {
        memset(b + n, 0, 8);
        memcpy(b + n, s_led[i].call, strnlen(s_led[i].call, 8));
        b[n + 8] = (uint8_t)s_led[i].day;
        b[n + 9] = (uint8_t)(s_led[i].day >> 8);
        n += 10;
    }
    if (s_up && xmb_state_put(&s_mb, b, n)) {
        s_led_dirty = false;
        s_led_snap_ms = millis();
    }
}

static void state_load(void)
{
    uint8_t b[3 + LEDGER * 10];
    int n = xmb_state_get(&s_mb, b, sizeof b);
    if (n < 3 || b[0] != 1) return;
    s_net = (int8_t)b[1];
    for (int i = 0; i < b[2] && i < LEDGER && 3 + i * 10 + 10 <= n; i++) {
        const uint8_t *e = b + 3 + i * 10;
        memcpy(s_led[i].call, e, 8);
        s_led[i].call[8] = 0;
        s_led[i].day = (uint16_t)(e[8] | e[9] << 8);
    }
}

/* ── What waits for the loop ─────────────────────────────────────────── */

enum { OP_HOLD = 1, OP_DECL, OP_RECEIPT };
static struct { uint8_t op, cls; uint8_t len; char wire[XPRS_MAX_WIRE + 1]; } s_q[PENDQ];
static struct { char call[12]; xb_t *b; } s_dq[DELIVERQ];
static uint32_t s_dropped;
static bool     s_trace;
void mail_trace(bool on) { s_trace = on; }

static void enqueue(uint8_t op, uint8_t cls, const char *wire, int len)
{
    for (int i = 0; i < PENDQ; i++) {
        if (s_q[i].op) continue;
        s_q[i].op = op;
        s_q[i].cls = cls;
        s_q[i].len = (uint8_t)len;
        memcpy(s_q[i].wire, wire, (size_t)len);
        s_q[i].wire[len] = 0;
        return;
    }
    s_dropped++;
}

static void deliver_later(const char *call, xb_t *b)
{
    for (int i = 0; i < DELIVERQ; i++)
        if (s_dq[i].b && !strcmp(s_dq[i].call, call)) { s_dq[i].b = b; return; }
    for (int i = 0; i < DELIVERQ; i++)
        if (!s_dq[i].b) {
            snprintf(s_dq[i].call, sizeof s_dq[i].call, "%s", call);
            s_dq[i].b = b;
            return;
        }
}

/* ── The interface ──────────────────────────────────────────────────── */

void mail_begin(const mail_cfg_t *cfg)
{
    s_cfg = *cfg;
    static const xmb_store_t st = { MAIL_PAGES, st_read, st_write, st_erase, NULL };
    s_up = xmb_open(&s_mb, &st);
    if (s_up) state_load();
    Serial.printf("mail: %s, %d held, %d of %d records live%s\n", s_up ? "open" : "UNUSABLE",
                  s_up ? xmb_count(&s_mb, NULL) : 0, s_mb.live, xmb_capacity(&s_mb),
                  s_net >= 0 ? ", network remembered" : "");
}

/* q:mail (9.12.3): how much is held, for whom. Zero is said, not silence. */
static void answer_q_mail(const xprs_t *p, const char *from, xb_t *b)
{
    char only[12] = "", tf[32], wire[XPRS_MAX_WIRE + 1];
    xprs_get_str(p, "only", only, sizeof only);
    nst_time_field(tf, sizeof tf);
    int n = snprintf(wire, sizeof wire, "t:observation f:%s d:%s %s s:mail mail:%d%s%s", s_cfg.self,
                     from, tf, xmb_count(&s_mb, only[0] ? only : NULL), only[0] ? " only:" : "", only);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = nst_sign(wire, n, (int)sizeof wire);
    if (n > 0) xb_send(b, wire, n);
}

void mail_heard(const char *wire, int len, xb_t *b, bool direct)
{
    if (!s_up) return;
    xprs_t p;
    char t[16] = "", f[12] = "", d[16] = "", q[24] = "", r[12] = "";
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "t", t, sizeof t) ||
        !xprs_get_str(&p, "f", f, sizeof f) || !strcmp(f, s_cfg.self))
        return;
    xprs_get_str(&p, "d", d, sizeof d);

    if (!strcmp(t, "identity")) key_learn(&p, f);
    if (direct) {
        ledger_touch(f);
        if (xmb_count(&s_mb, f) > 0) deliver_later(f, b);
    }
    if (!strcmp(d, s_cfg.self)) {
        if (!strcmp(t, "request") && xprs_get_str(&p, "q", q, sizeof q) && strstr(q, "mail"))
            answer_q_mail(&p, f, b);
        return;
    }
    if (!strcmp(t, "mailbox")) {
        if (verified(&p, f)) enqueue(OP_DECL, 0, wire, len);
        return;
    }
    /* Any holder that hears the recipient's receipt releases (12.8.1). */
    if (!strcmp(t, "receipt") && xprs_get_str(&p, "r", r, sizeof r) && xmb_count(&s_mb, f) > 0 &&
        verified(&p, f))
        enqueue(OP_RECEIPT, 0, wire, len);
    char to[12] = "";
    snprintf(to, sizeof to, "%.*s", (int)strcspn(d, ","), d);
    bool contact = ledger_has(f) || (to[0] && ledger_has(to));
    int cls = xmb_class(&s_mb, wire, len, s_cfg.self, contact, nst_now());
    if (s_trace) Serial.printf("mail: %s for %s from %s: class %d%s\n", t, to, f, cls,
                               cls && s_cfg.heard_directly(to, HERE_MS) ? ", recipient here" : "");
    if (cls && !s_cfg.heard_directly(to, HERE_MS) && len <= XMB_PAYLOAD_MAX)
        enqueue(OP_HOLD, (uint8_t)cls, wire, len);
}

void mail_tick(void)
{
    if (!s_up) return;
    /* One write per turn: an erase is 85 ms and the radio is waiting. */
    for (int i = 0; i < PENDQ; i++) {
        if (!s_q[i].op) continue;
        xprs_t p;
        char r[12] = "", f[12] = "";
        switch (s_q[i].op) {
        case OP_HOLD:
            if (xmb_hold(&s_mb, s_q[i].wire, s_q[i].len, s_q[i].cls, nst_now()) >= 0)
                Serial.printf("mail: holding (class %u) %.60s\n", s_q[i].cls, s_q[i].wire);
            break;
        case OP_DECL: {
            /* A copy heard again on the other bearer changes nothing. */
            int before = s_mb.live;
            if (xmb_decl(&s_mb, s_q[i].wire, s_q[i].len, s_cfg.self, nst_now()) && s_mb.live != before)
                Serial.printf("mail: %.70s\n", s_q[i].wire);
            break; }
        case OP_RECEIPT:
            if (xprs_parse(s_q[i].wire, s_q[i].len, &p) && xprs_get_str(&p, "r", r, sizeof r) &&
                xprs_get_str(&p, "f", f, sizeof f) && xmb_receipt(&s_mb, r, f))
                Serial.printf("mail: %s took %s, released\n", f, r);
            break;
        }
        s_q[i].op = 0;
        return;
    }
    for (int i = 0; i < DELIVERQ; i++) {
        if (!s_dq[i].b) continue;
        int out[XMB_PAGE_OUT];
        int n = xmb_due(&s_mb, s_dq[i].call, millis(), nst_now(), out, XMB_PAGE_OUT);
        for (int k = 0; k < n; k++) {
            char w[XPRS_MAX_WIRE + 1];
            int len = xmb_read(&s_mb, out[k], w, sizeof w);
            if (len > 0 && xb_send(s_dq[i].b, w, len)) {
                xmb_aired(&s_mb, out[k], millis());
                Serial.printf("mail: to %s %.60s\n", s_dq[i].call, w);
            }
        }
        s_dq[i].b = NULL;
        return;
    }
    if (s_led_dirty && (!s_led_snap_ms || millis() - s_led_snap_ms > SNAP_MS)) state_save();
}

bool mail_busy(void)
{
    for (int i = 0; i < PENDQ; i++) if (s_q[i].op) return true;
    for (int i = 0; i < DELIVERQ; i++) if (s_dq[i].b) return true;
    return false;
}

int mail_count(void) { return s_up ? xmb_count(&s_mb, NULL) : 0; }

int mail_net_get(void) { return s_net; }

void mail_net_set(int net)
{
    if (s_net == net) return;
    s_net = (int8_t)net;
    state_save();
}

void mail_report(void)
{
    const xmb_stats_t *st = &s_mb.stats;
    int contacts = 0;
    for (int i = 0; i < LEDGER; i++) if (s_led[i].call[0]) contacts++;
    Serial.printf("mail: %d held, %d/%d records, held %lu refused %lu evicted %lu released %lu "
                  "aired %lu relocated %lu erased %lu bad %lu dropped %lu, %d contacts\n",
                  mail_count(), s_mb.live, xmb_capacity(&s_mb), (unsigned long)st->held,
                  (unsigned long)st->refused, (unsigned long)st->evicted, (unsigned long)st->released,
                  (unsigned long)st->aired, (unsigned long)st->relocated, (unsigned long)st->erased,
                  (unsigned long)st->bad, (unsigned long)s_dropped, contacts);
}
