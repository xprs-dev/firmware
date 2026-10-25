/* See xfw.h. */
#include "xfw.h"
#include "xprs.h"
#include "xprsid.h"
#include "xprs_folder.h"
#include "rns_link.h"
#include "rns_resource.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ── cadence ─────────────────────────────────────────────────────────── */
#define ASK_FIRST_S      120u          /* after boot */
#define ASK_EVERY_S      (6u * 3600u)
#define ASK_JITTER_S     600u
#define ASK_WINDOW_DAYS  30u
#define CMD_WAIT_S       45u           /* a 202 for our cmd:file */
#define LINK_WAIT_S      20u           /* an LRPROOF */
#define LINK_TRIES       3
#define STALL_S          4u            /* no part: ask again */
#define FETCH_MAX_S      (40u * 60u)   /* the whole transfer */
#define FAIL_HOLD_S      (6u * 3600u)  /* a digest that failed is left alone */
#define PARKED_MAX       8

typedef enum { ST_IDLE, ST_ASKED, ST_LINKING, ST_GETTING, ST_FINISHING } state_t;

static xfw_cfg_t s_cfg;
static bool s_on;
static char s_pub_call[XPRS_CALL_LEN_MAX];
static uint8_t s_key[32];
static bool s_have_key;

/* What the publisher said: the newest image for this board and channel. */
static struct {
    bool used;
    xfolder_entry_t e;
    xfolder_image_t img;
    char from[XPRS_CALL_LEN_MAX];       /* who replayed it: a holder */
} s_best;

/* Digests that failed, and when: not tried again for FAIL_HOLD_S. */
static struct { uint8_t sha[32]; uint32_t at_s; } s_failed[3];
static int s_failed_n;

/* Wires parked by xfw_on_wire for xfw_poll (the verify is a curve op). */
static struct { char w[XPRS_MAX_WIRE + 1]; int len; bool used; } s_parked[PARKED_MAX];

/* The fetch under way. */
static struct {
    state_t  state;
    char     holder[XPRS_CALL_LEN_MAX];
    char     ask_id[XPRS_ID_LEN];
    uint32_t at_s, started_s, last_part_s;
    int      link_tries;
    rns_link_t link;
    rns_res_t *res;
    bool     sink_open;
    uint32_t got;
    char     why[48];
} s_f;

static uint32_t s_next_ask_s, s_asks, s_fetches, s_installs, s_fails;
static int s_try;                         /* which holder is being asked */
static uint32_t s_retry_s;                /* nobody to ask: try again then */
static char s_last[160];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    snprintf(s_last, sizeof s_last, "%s", line);
    if (s_cfg.say) s_cfg.say(line);
}

static void lock(void)   { if (s_cfg.lock) s_cfg.lock(); }
static void unlock(void) { if (s_cfg.unlock) s_cfg.unlock(); }

/* ── the publisher's callsign ────────────────────────────────────────── */

/* X1 + the first four characters of the key's npub: the derived callsign
 * (XPRS.md 2, 6.1.3), as xauth_call_matches_npub reads it. */
void xfw_publisher_call(const uint8_t key[32], char out[XPRS_CALL_LEN_MAX])
{
    static const char cs[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    out[0] = 'X'; out[1] = '1';
    uint32_t acc = 0;
    int bits = 0, o = 2, i = 0;
    while (o < 6) {
        acc = (acc << 8) | key[i++];
        bits += 8;
        while (bits >= 5 && o < 6) {
            bits -= 5;
            char c = cs[(acc >> bits) & 31];
            out[o++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
    }
    out[6] = 0;
}

/* Callsign equality up to a suffix or the end of the field (a space). */
static bool call_end(char c) { return c == 0 || c == '-' || c == ' '; }
static bool same_call(const char *a, const char *b)
{
    while (!call_end(*a) && !call_end(*b)) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return false;
        a++; b++;
    }
    return call_end(*a) && call_end(*b);
}

/* ── start ───────────────────────────────────────────────────────────── */

void xfw_start(const xfw_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_on = true;
    s_have_key = s_cfg.publisher_key && s_cfg.publisher_key(s_key);
    if (s_have_key) xfw_publisher_call(s_key, s_pub_call);
    uint32_t now = s_cfg.now_s();
    s_next_ask_s = now + ASK_FIRST_S + (uint32_t)(rand() % (ASK_JITTER_S / 4 + 1));
    memset(&s_f, 0, sizeof s_f);
    say("fwfetch: %s, publisher %s, channel %s, board %s",
        s_cfg.enabled && s_cfg.enabled() ? "on" : "off",
        s_have_key ? s_pub_call : "(no fwkey)",
        s_cfg.channel ? s_cfg.channel() : "?", s_cfg.board ? s_cfg.board : "?");
}

/* ── wires ───────────────────────────────────────────────────────────── */

void xfw_on_wire(const char *wire, int len)
{
    if (!s_on || !wire || len <= 0 || len > XPRS_MAX_WIRE) return;
    /* Cheap tests only here: the type and who it is from or for. */
    if (strncmp(wire, "t:file ", 7) == 0) {
        /* The publisher's, and about THIS board: a release airs a
         * description per board in one burst, and parking space is for
         * the one that matters. */
        if (!s_have_key || !s_cfg.board) return;
        const char *f = strstr(wire, " f:");
        if (!f || !same_call(f + 3, s_pub_call)) return;
        const char *nm = strstr(wire, " name:");
        if (!nm) return;
        size_t bl = strlen(s_cfg.board);
        if (strncmp(nm + 6, s_cfg.board, bl) != 0 || nm[6 + bl] != '-') return;
    } else if (strncmp(wire, "t:result ", 9) == 0) {
        if (s_f.state != ST_ASKED) return;
        const char *r = strstr(wire, " r:");
        if (!r || strncmp(r + 3, s_f.ask_id, strlen(s_f.ask_id)) != 0) return;
    } else {
        return;
    }
    lock();
    for (int i = 0; i < PARKED_MAX; i++) {
        if (s_parked[i].used) continue;
        memcpy(s_parked[i].w, wire, (size_t)len);
        s_parked[i].w[len] = 0;
        s_parked[i].len = len;
        s_parked[i].used = true;
        break;
    }
    unlock();
}

static bool failed_recently(const uint8_t sha[32], uint32_t now)
{
    for (int i = 0; i < s_failed_n; i++)
        if (memcmp(s_failed[i].sha, sha, 32) == 0 && now - s_failed[i].at_s < FAIL_HOLD_S)
            return true;
    return false;
}

static void note_failed(const uint8_t sha[32], uint32_t now)
{
    int k = s_failed_n < 3 ? s_failed_n++ : 0;
    memcpy(s_failed[k].sha, sha, 32);
    s_failed[k].at_s = now;
}

/* A description from the publisher: is it a better image for us? */
static void consider(const xprs_t *p, const char *from, uint32_t now)
{
    if (!xprsid_verify(p, s_key)) { say("fwfetch: a t:file as %s did not verify", s_pub_call); return; }
    xfolder_entry_t e;
    if (!xfolder_desc_member(p, &e)) return;
    char tag[16] = "";
    xprs_get_str(p, "tag", tag, sizeof tag);
    const char *chan = s_cfg.channel ? s_cfg.channel() : "stable";
    if (strcasecmp(tag, chan) != 0) return;
    xfolder_image_t img;
    const char *running = s_cfg.running_version ? s_cfg.running_version() : "";
    if (!xfolder_newer_for(&e, s_cfg.board, running, &img)) return;
    if (failed_recently(e.sha, now)) return;
    if (s_best.used && xfolder_version_cmp(img.version, s_best.img.version) <= 0) return;
    s_best.used = true;
    s_best.e = e;
    s_best.img = img;
    snprintf(s_best.from, sizeof s_best.from, "%s", from ? from : "");
    say("fwfetch: %s %s is published (%u bytes), running %s",
        img.board, img.version, (unsigned)e.size, running);
}

static void reset_fetch(const char *why)
{
    if (s_f.res) { rns_res_free(s_f.res); s_f.res = NULL; }
    if (s_f.sink_open && s_cfg.sink_abort) s_cfg.sink_abort();
    memset(&s_f, 0, sizeof s_f);
    if (why) snprintf(s_f.why, sizeof s_f.why, "%s", why);
}

static void fetch_failed(const char *why, uint32_t now)
{
    s_try = 0;
    say("fwfetch: %s failed: %s", s_best.used ? s_best.img.version : "?", why);
    if (s_best.used) note_failed(s_best.e.sha, now);
    s_best.used = false;
    s_fails++;
    reset_fetch(why);
}

/* ── the sink, as the resource sees it ───────────────────────────────── */

static bool sink_write(const uint8_t *d, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_f.sink_open) return false;
    if (!s_cfg.sink_write(d, len)) return false;
    s_f.got += (uint32_t)len;
    s_f.last_part_s = s_cfg.now_s();
    return true;
}

static void sink_done(bool ok, const char *why, void *ctx)
{
    (void)ctx;
    if (!ok) { snprintf(s_f.why, sizeof s_f.why, "resource: %s", why); s_f.state = ST_FINISHING; return; }
    s_f.state = ST_FINISHING;
    s_f.why[0] = 0;
}

static void send_pkt(const uint8_t *pkt, size_t len, void *ctx)
{
    (void)ctx;
    if (s_cfg.send_packet) s_cfg.send_packet(pkt, len);
}

/* ── the fetch ───────────────────────────────────────────────────────── */

/* True when an ask went out; false when there was nobody to ask yet (the
 * uplink or its peers are not up), which is asked again in a minute rather
 * than next period. */
static bool ask_publisher(void)
{
    char arch[XPRS_CALL_LEN_MAX];
    if (!s_cfg.pick_archiver || !s_cfg.pick_archiver(arch, sizeof arch)) {
        say("fwfetch: no archiver to ask for %s's folder yet", s_pub_call);
        return false;
    }
    uint32_t ep = s_cfg.epoch_s ? s_cfg.epoch_s() : 0;
    if (!ep) return false;                  /* a signed ask needs a time */
    char ts[24], since[24];
    time_t t = (time_t)ep, ts0 = (time_t)(ep - ASK_WINDOW_DAYS * 86400u);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d_%H:%M:%S", &tmv);
    gmtime_r(&ts0, &tmv);
    strftime(since, sizeof since, "%Y-%m-%d_%H:%M:%S", &tmv);
    char w[XPRS_MAX_WIRE + 1];
    int n = snprintf(w, sizeof w, "t:command f:%s d:%s ts:%s cmd:history kind:file only:%s since:%s",
                 s_cfg.self ? s_cfg.self : "", arch, ts, s_pub_call, since);
    if (n <= 0 || n >= (int)sizeof w) return false;
    n = s_cfg.sign(w, n, (int)sizeof w);
    if (n <= 0 || !s_cfg.send_to(arch, w, n)) return false;
    s_asks++;
    snprintf(s_best.from, sizeof s_best.from, "%s", arch);
    say("fwfetch: asked %s for %s's firmware folder", arch, s_pub_call);
    return true;
}

/* Who to ask for the bytes, in order: the publisher itself when it can be
 * addressed (whoever published the folder holds its files, 12.9.2), then
 * the archiver that replayed the description, then any archiver. One try
 * each; a refusal moves to the next, so an archiver that keeps the
 * description but not the image does not end the fetch. */
static bool holder_at(int i, char *out, size_t cap)
{
    const char *c = NULL;
    char arch[XPRS_CALL_LEN_MAX];
    if (i == 0) c = s_pub_call;
    else if (i == 1) c = s_best.from[0] ? s_best.from : NULL;
    else if (i == 2 && s_cfg.pick_archiver && s_cfg.pick_archiver(arch, sizeof arch)) c = arch;
    if (!c || !c[0]) return false;
    /* The same station twice in the list is tried once. */
    for (int k = 0; k < i; k++) {
        char prev[XPRS_CALL_LEN_MAX];
        if (holder_at(k, prev, sizeof prev) && same_call(prev, c)) return false;
    }
    if (!s_cfg.peer_route(c, NULL, NULL, NULL, NULL)) {
        if (s_cfg.want) s_cfg.want(c);      /* next time it may be */
        return false;
    }
    snprintf(out, cap, "%s", c);
    return true;
}

static void start_fetch(uint32_t now)
{
    char hbuf[XPRS_CALL_LEN_MAX];
    const char *holder = NULL;
    while (s_try < 3) {
        if (holder_at(s_try, hbuf, sizeof hbuf)) { holder = hbuf; break; }
        s_try++;
    }
    if (!holder) {
        /* Nobody addressable yet is not a failed image: the route may come
         * with the next announce. Try again in a minute, keep the digest. */
        say("fwfetch: nobody addressable to fetch %s from yet", s_best.img.version);
        s_try = 0;
        s_retry_s = now + 60;
        return;
    }
    uint32_t ep = s_cfg.epoch_s ? s_cfg.epoch_s() : 0;
    if (!ep) return;
    char ts[24];
    time_t t = (time_t)ep;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d_%H:%M:%S", &tmv);
    char w[XPRS_MAX_WIRE + 1];
    int n;
    /* The reference is the digest as base64url, the form a sender emits. */
    char ref[48];
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    {
        uint32_t acc = 0; int bits = 0, o = 0;
        for (int i = 0; i < 32; i++) {
            acc = (acc << 8) | s_best.e.sha[i]; bits += 8;
            while (bits >= 6) { bits -= 6; ref[o++] = b64[(acc >> bits) & 63]; }
        }
        if (bits) ref[o++] = b64[(acc << (6 - bits)) & 63];
        ref[o] = 0;
    }
    n = snprintf(w, sizeof w, "t:command f:%s d:%s ts:%s cmd:file file:%s.bin",
                 s_cfg.self ? s_cfg.self : "", holder, ts, ref);
    if (n <= 0 || n >= (int)sizeof w) return;
    n = s_cfg.sign(w, n, (int)sizeof w);
    if (n <= 0) return;
    xprs_t p;
    if (!xprs_parse(w, n, &p)) return;
    memset(&s_f, 0, sizeof s_f);
    xprs_id(&p, s_f.ask_id);
    snprintf(s_f.holder, sizeof s_f.holder, "%s", holder);
    if (!s_cfg.send_to(holder, w, n)) { say("fwfetch: could not ask %s for the image", holder); memset(&s_f, 0, sizeof s_f); return; }
    s_f.state = ST_ASKED;
    s_f.at_s = s_f.started_s = now;
    s_fetches++;
    say("fwfetch: asked %s for %s %s (%u bytes)", holder, s_best.img.board,
        s_best.img.version, (unsigned)s_best.e.size);
}

static void open_link(uint32_t now)
{
    uint8_t dest[RNS_HASH_LEN], pub[RNS_PUB_LEN], hop[RNS_HASH_LEN];
    bool have_hop = false;
    if (!s_cfg.peer_route(s_f.holder, dest, pub, hop, &have_hop)) {
        fetch_failed("holder has no files route", now);
        return;
    }
    rns_link_init(&s_f.link, dest, pub, have_hop ? hop : NULL);
    uint8_t pkt[RNS_MTU];
    int n = rns_link_request(&s_f.link, pkt, sizeof pkt);
    if (n <= 0 || !s_cfg.send_packet(pkt, (size_t)n)) { fetch_failed("link request not sent", now); return; }
    s_f.state = ST_LINKING;
    s_f.at_s = now;
    s_f.link_tries++;
}

static void on_result(const xprs_t *p, uint32_t now)
{
    char code[8] = "", m[64] = "";
    xprs_get_str(p, "code", code, sizeof code);
    xprs_get_str(p, "m", m, sizeof m);
    int c = atoi(code);
    if (c == 202) { open_link(now); return; }
    say("fwfetch: %s answered cmd:file with %d%s%.40s", s_f.holder, c, m[0] ? " " : "", m);
    reset_fetch(NULL);
    s_try++;
    if (s_try < 3) { start_fetch(now); return; }
    s_try = 0;
    fetch_failed("every holder refused", now);
}

void xfw_on_link_packet(const rns_packet_t *p)
{
    if (!s_on) return;
    lock();
    if (s_f.state == ST_LINKING) {
        if (rns_link_owns(&s_f.link, p) && p->packet_type == RNS_PACKET_PROOF &&
            p->context == RNS_CTX_LRPROOF) {
            uint8_t pkt[RNS_MTU];
            int n = rns_link_proof(&s_f.link, p, pkt, sizeof pkt);
            if (n > 0 && s_cfg.send_packet(pkt, (size_t)n)) {
                /* The sink first, so a slot that cannot be opened costs no
                 * transfer; then GET_FILE. */
                if (!s_cfg.sink_begin(s_best.img.version, s_best.e.size, s_best.e.sha)) {
                    snprintf(s_f.why, sizeof s_f.why, "no slot for the image");
                    s_f.state = ST_FINISHING;
                } else {
                    s_f.sink_open = true;
                    rns_res_sink_t sk = { sink_write, sink_done, NULL };
                    s_f.res = rns_res_new(&s_f.link, &sk);
                    uint8_t get[33]; get[0] = 0x01; memcpy(get + 1, s_best.e.sha, 32);
                    n = rns_link_encrypt(&s_f.link, RNS_CTX_NONE, get, sizeof get, pkt, sizeof pkt);
                    if (n > 0 && s_f.res && s_cfg.send_packet(pkt, (size_t)n)) {
                        s_f.state = ST_GETTING;
                        s_f.at_s = s_f.last_part_s = s_cfg.now_s();
                    } else {
                        snprintf(s_f.why, sizeof s_f.why, "GET_FILE not sent");
                        s_f.state = ST_FINISHING;
                    }
                }
            }
        }
    } else if (s_f.state == ST_GETTING && s_f.res && rns_link_owns(&s_f.link, p)) {
        if (p->context == RNS_CTX_NONE && p->packet_type == RNS_PACKET_DATA) {
            uint8_t plain[64];
            int pn = rns_link_decrypt(&s_f.link, p, plain, sizeof plain);
            if (pn > 0 && plain[0] == 0x81) {
                snprintf(s_f.why, sizeof s_f.why, "holder does not have it");
                s_f.state = ST_FINISHING;
            }
        } else {
            rns_res_feed(s_f.res, p, send_pkt, NULL);
        }
    }
    unlock();
}

/* ── the cadence ─────────────────────────────────────────────────────── */

void xfw_poll(void)
{
    if (!s_on) return;
    uint32_t now = s_cfg.now_s();
    lock();

    /* Parked wires: descriptions and the answer to our ask. */
    for (int i = 0; i < PARKED_MAX; i++) {
        if (!s_parked[i].used) continue;
        s_parked[i].used = false;
        xprs_t p;
        if (!xprs_parse(s_parked[i].w, s_parked[i].len, &p)) continue;
        char type[8], from[XPRS_CALL_LEN_MAX] = "";
        xprs_type(&p, type, sizeof type);
        xprs_get_str(&p, "f", from, sizeof from);
        if (strcmp(type, "file") == 0) consider(&p, s_best.from[0] ? s_best.from : from, now);
        else if (strcmp(type, "result") == 0 && s_f.state == ST_ASKED &&
                 same_call(from, s_f.holder)) on_result(&p, now);
    }

    bool enabled = s_cfg.enabled && s_cfg.enabled() && s_have_key;
    switch (s_f.state) {
    case ST_IDLE:
        if (enabled && now >= s_next_ask_s && !(s_cfg.busy && s_cfg.busy())) {
            s_next_ask_s = ask_publisher()
                ? now + ASK_EVERY_S + (uint32_t)(rand() % (ASK_JITTER_S + 1))
                : now + 60;
        }
        if (enabled && s_best.used && now >= s_retry_s && !(s_cfg.busy && s_cfg.busy()))
            start_fetch(now);
        break;
    case ST_ASKED:
        if (now - s_f.at_s > CMD_WAIT_S) {
            say("fwfetch: %s did not answer cmd:file", s_f.holder);
            reset_fetch(NULL);
            if (++s_try < 3) start_fetch(now);
            else { s_try = 0; fetch_failed("no answer to cmd:file", now); }
        }
        break;
    case ST_LINKING:
        if (now - s_f.at_s > LINK_WAIT_S) {
            if (s_f.link_tries < LINK_TRIES) open_link(now);
            else fetch_failed("no link to the holder", now);
        }
        break;
    case ST_GETTING:
        if (now - s_f.started_s > FETCH_MAX_S) { snprintf(s_f.why, sizeof s_f.why, "transfer took too long"); s_f.state = ST_FINISHING; break; }
        if (s_f.res && now - s_f.last_part_s >= STALL_S) {
            s_f.last_part_s = now;
            rns_res_retry(s_f.res, send_pkt, NULL);
        }
        break;
    case ST_FINISHING:
        if (s_f.why[0]) { fetch_failed(s_f.why, now); break; }
        say("fwfetch: %s %s arrived (%u bytes), installing", s_best.img.board,
            s_best.img.version, (unsigned)s_f.got);
        if (s_f.res) { rns_res_free(s_f.res); s_f.res = NULL; }
        s_installs++;
        {
            bool ok = s_cfg.sink_finish();       /* restarts the station when it succeeds */
            s_f.sink_open = false;
            if (!ok) fetch_failed("install refused", now);
            else reset_fetch(NULL);
        }
        break;
    }
    unlock();
}

const char *xfw_state(char *buf, size_t cap)
{
    static const char *names[] = { "idle", "asked", "linking", "getting", "finishing" };
    char st[120] = "";
    if (s_f.res) rns_res_state(s_f.res, st, sizeof st);
    snprintf(buf, cap, "%s publisher=%s chan=%s best=%s state=%s%s%s got=%u asks=%u fetches=%u installs=%u fails=%u last=%s",
             s_on && s_cfg.enabled && s_cfg.enabled() ? "on" : "off",
             s_have_key ? s_pub_call : "-", s_cfg.channel ? s_cfg.channel() : "-",
             s_best.used ? s_best.img.version : "-", names[s_f.state],
             st[0] ? " " : "", st, (unsigned)s_f.got,
             (unsigned)s_asks, (unsigned)s_fetches, (unsigned)s_installs, (unsigned)s_fails, s_last);
    return buf;
}
