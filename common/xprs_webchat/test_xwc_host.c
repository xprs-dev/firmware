/* Host test for xwc: the station API's rules, the scenarios the Dart core's
 * WebChatApi tests use (core/test/web_chat_*_test.dart), so both engines
 * answer the same. OpenSSL stands in for mbedtls (XPRSSIG_HOST_TEST). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#include "xwc.h"
#include "xprs.h"
#include "xprsid.h"
#include "xprssig.h"

void xprs_sha256(const uint8_t *in, size_t len, uint8_t out[32]) { SHA256(in, len, out); }

static int fails;
#define CHECK(ok, what) do { int _ok = (ok); printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", what); if (!_ok) fails++; } while (0)

/* ---- the host: a clock, keys, a kv, what was aired and filed ---- */
static uint32_t s_now = 1794000000;
static uint32_t now_cb(void) { return s_now; }
static uint32_t uptime_cb(void) { return s_now - 1794000000 + 100; }

typedef struct { char call[12]; uint8_t priv[32], pub[32]; } person_t;
static person_t s_people[8];
static int s_npeople;
static person_t *person(const char *call, uint8_t seed)
{
    for (int i = 0; i < s_npeople; i++) if (!strcmp(s_people[i].call, call)) return &s_people[i];
    person_t *p = &s_people[s_npeople++];
    snprintf(p->call, sizeof p->call, "%s", call);
    memset(p->priv, seed, 32);
    p->priv[31] = 7;
    xprssig_public_key(p->priv, p->pub);
    return p;
}
static const uint8_t *peer_key_cb(const char *call)
{
    for (int i = 0; i < s_npeople; i++) if (!strcmp(s_people[i].call, call)) return s_people[i].pub;
    return NULL;
}
static uint32_t s_rand = 12345;
static void random_cb(uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) { s_rand = s_rand * 1103515245u + 12345u; b[i] = (uint8_t)(s_rand >> 16); } }

static struct { char k[24]; char v[2048]; } s_kv[32];
static int s_nkv;
static const char *kv_get_cb(const char *k) { for (int i = 0; i < s_nkv; i++) if (!strcmp(s_kv[i].k, k)) return s_kv[i].v; return NULL; }
static bool kv_set_cb(const char *k, const char *v)
{
    for (int i = 0; i < s_nkv; i++) if (!strcmp(s_kv[i].k, k)) { snprintf(s_kv[i].v, sizeof s_kv[i].v, "%s", v); return true; }
    snprintf(s_kv[s_nkv].k, 24, "%s", k); snprintf(s_kv[s_nkv].v, 2048, "%s", v); s_nkv++; return true;
}
static char s_aired[32][256]; static int s_naired;
static char s_filed[32][256]; static int s_nfiled;
static bool air_cb(const char *w, int n) { snprintf(s_aired[s_naired++ % 32], 256, "%.*s", n, w); return true; }
/* What the archive answers: filed, held already, or busy (a full queue). */
static int s_file_result = XWC_FILED;
static int file_cb(const char *w, int n)
{
    if (s_file_result == XWC_FILE_BUSY) return XWC_FILE_BUSY;   /* not kept */
    snprintf(s_filed[s_nfiled++ % 32], 256, "%.*s", n, w);
    return s_file_result;
}
static bool aired_has(const char *s) { for (int i = 0; i < s_naired && i < 32; i++) if (strstr(s_aired[i], s)) return true; return false; }
static bool filed_has(const char *s) { for (int i = 0; i < s_nfiled && i < 32; i++) if (strstr(s_filed[i], s)) return true; return false; }

static const char *ACTS = "/tmp/xwc_test_acts.txt";
static xwc_cb_t s_cb;
static person_t *s_station;

static void fresh(void)
{
    remove(ACTS);
    s_nkv = 0; s_naired = s_nfiled = 0;
    xwc_reset();
    s_cb = (xwc_cb_t){ now_cb, uptime_cb, peer_key_cb, random_cb, kv_get_cb, kv_set_cb, air_cb, file_cb, ACTS };
    xwc_init(&s_cb, "X3STAT", s_station->priv);
}

static void ts(char out[20], int plus_min)
{
    /* The same civil arithmetic xwc uses, through a signed wire's ts. */
    uint32_t t = s_now + (uint32_t)(plus_min * 60);
    uint32_t days = t / 86400, s = t % 86400;
    int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    snprintf(out, 20, "%04u-%02u-%02u_%02u:%02u:%02u", (unsigned)y % 10000, (unsigned)m % 100,
             (unsigned)d % 100, s / 3600 % 100, s / 60 % 60, s % 60);
}

/* A signed wire from [who]: "%s" with ts inserted. */
static int signed_wire(char *out, int cap, person_t *who, const char *fmt, int plus_min)
{
    char t[20];
    ts(t, plus_min);
    int n = snprintf(out, (size_t)cap, fmt, who->call, t);
    return xprsid_sign(out, n, cap, who->priv);
}

static char s_buf[4096], s_why[96], s_id[XWC_ID_LEN];

static bool session(person_t *who, char token[XWC_TOKEN_LEN])
{
    char nonce[XWC_NONCE_LEN], w[300], call[XWC_CALL_LEN];
    xwc_nonce(nonce);
    char fmt[128];
    snprintf(fmt, sizeof fmt, "t:session f:%%s ts:%%s m:%s", nonce);
    int n = signed_wire(w, sizeof w, who, fmt, 0);
    return xwc_prove(w, n, token, call, s_why, sizeof s_why) == 200;
}
static int admin_post(const char *body) { return xwc_admin_post(body, true, s_buf, sizeof s_buf); }
static int gate(person_t *who, const char *fmt, int plus_min)
{
    char w[300];
    int n = signed_wire(w, sizeof w, who, fmt, plus_min);
    return xwc_gate(w, n, true, s_id, s_why, sizeof s_why);
}
static bool jfind(const char *key) { return strstr(s_buf, key) != NULL; }
static void jval(const char *key, char *out, size_t cap)
{
    char k[40]; snprintf(k, sizeof k, "\"%s\":\"", key);
    const char *p = strstr(s_buf, k); out[0] = 0;
    if (!p) return;
    p += strlen(k);
    size_t o = 0;
    while (*p && *p != '"' && o < cap - 1) out[o++] = *p++;
    out[o] = 0;
}

/* Alice claims the station and accepts moderating. */
static bool claim(person_t *alice, char token[XWC_TOKEN_LEN])
{
    if (!session(alice, token)) return false;
    char body[200];
    snprintf(body, sizeof body, "{\"action\":\"claim\",\"session\":\"%s\"}", token);
    if (admin_post(body) != 200) return false;
    char offer[8];
    jval("offer", offer, sizeof offer);
    char fmt[96];
    snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:X3STAT ts:%%s r:%s accept:mod", offer);
    return gate(alice, fmt, 0) == 1 && !strcmp(xwc_role_of("X3STAT", "X1ALIC"), "mod");
}

int main(void)
{
    s_station = person("X3STAT", 0x5a);
    person_t *alice = person("X1ALIC", 0x11), *bob = person("X1BOBB", 0x22), *carol = person("X1CARO", 0x33);
    char tok[XWC_TOKEN_LEN], bt[XWC_TOKEN_LEN], body[400], fmt[160];

    printf("services\n");
    fresh();
    xwc_services_json(s_buf, sizeof s_buf, "chat,traffic", false);
    CHECK(jfind("\"station\":\"X3STAT\"") && jfind("\"rooms\":true") && jfind("\"alerts\":\"moderators\""),
          "names the station, offers sessions, admin, moderation, rooms, the alert policy");
    CHECK(!jfind("gallery"), "a board on its own flash offers no gallery");
    xwc_services_json(s_buf, sizeof s_buf, "chat,traffic", true);
    CHECK(jfind("\"gallery\":true,\"storage\":true}"), "with a memory card it does (page.gallery, page.storage)");

    printf("sessions\n");
    fresh();
    CHECK(session(alice, tok) && !strcmp(xwc_session_call(tok), "X1ALIC"), "the right key gets a session");
    {
        char nonce[XWC_NONCE_LEN], w[300], call[12]; xwc_nonce(nonce);
        snprintf(fmt, sizeof fmt, "t:session f:X1ALIC ts:%%s m:%s", nonce);
        char t[20]; ts(t, 0); int n = snprintf(w, sizeof w, fmt, t);
        n = xprsid_sign(w, n, sizeof w, bob->priv);
        CHECK(xwc_prove(w, n, tok, call, s_why, sizeof s_why) == 403, "another key under that callsign is refused");
        xwc_nonce(nonce);
        snprintf(w, sizeof w, "t:session f:X1NOBO ts:%s m:%s", t, nonce);
        n = xprsid_sign(w, (int)strlen(w), sizeof w, bob->priv);
        CHECK(xwc_prove(w, n, tok, call, s_why, sizeof s_why) == 409, "an unknown identity is 409: send it first");
    }

    printf("the administrator\n");
    fresh();
    CHECK(session(alice, tok), "alice has a session");
    snprintf(body, sizeof body, "{\"action\":\"claim\",\"session\":\"%s\"}", tok);
    CHECK(xwc_admin_post(body, false, s_buf, sizeof s_buf) == 403, "a claim from the internet is refused");
    CHECK(admin_post(body) == 200 && jfind("\"admin\":\"X1ALIC\"") && jfind("\"you\":true") &&
          jfind("\"role\":\"invited\"") && jfind("\"offer_role\":\"mod\""),
          "a claim from the network takes, and offers the moderator role");
    CHECK(aired_has("grant:X1ALIC role:mod") && aired_has("f:X3STAT d:X3STAT") && aired_has(" sig:"),
          "the offer is a t:moderate signed and aired by the station");
    {
        char offer[8]; jval("offer", offer, sizeof offer);
        snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:X3STAT ts:%%s r:%s accept:mod", offer);
        CHECK(gate(alice, fmt, 0) == 1 && !strcmp(xwc_role_of("X3STAT", "X1ALIC"), "mod"), "alice accepts: a moderator");
    }
    CHECK(session(bob, bt), "bob has a session");
    snprintf(body, sizeof body, "{\"action\":\"claim\",\"session\":\"%s\"}", bt);
    CHECK(admin_post(body) == 409, "nobody else can claim it now");
    xwc_admin_json(s_buf, sizeof s_buf, bt, true);
    CHECK(jfind("\"you\":false") && !jfind("settings"), "and does not see the settings");
    snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{\"local_private\":false,\"who_may_post\":\"local\"}}", tok);
    CHECK(admin_post(body) == 200 && !xwc_local_private() && !strcmp(xwc_who_may_post(), "local"), "the administrator sets settings");
    snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{\"who_may_alert\":\"nobody\"}}", tok);
    CHECK(admin_post(body) == 400, "an unknown value is refused");
    snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{\"who_may_post\":\"everyone\"}}", tok);
    admin_post(body);

    printf("where the station is (pos, acc)\n");
    {
        char pv[24]; uint32_t acc = 1;
        CHECK(!xwc_station_pos(pv, sizeof pv, &acc), "no position until one is set");
        static const char *const bad[] = {
            "\"pos\":\"91,0\"", "\"pos\":\"38.7,-9.1,3\"", "\"pos\":\"38.123456,-9.1\"",
            "\"pos\":\"Lisbon\"", "\"pos\":\"38.,-9\"", "\"acc\":-1", "\"acc\":3000001",
            "\"acc\":\"300km\"",
        };
        int refused = 0;
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{%s}}", tok, bad[i]);
            if (admin_post(body) == 400) refused++;
        }
        CHECK(refused == (int)(sizeof bad / sizeof bad[0]), "malformed positions and radii are refused");
        snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{\"pos\":\"39,-8\",\"acc\":171000}}", tok);
        CHECK(admin_post(body) == 200 && jfind("\"pos\":\"39,-8\",\"acc\":171000"), "a country's centre and radius are kept");
        CHECK(xwc_station_pos(pv, sizeof pv, &acc) && !strcmp(pv, "39,-8") && acc == 171000,
              "and given to the beacon as set");
        snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{\"pos\":\"-33.86882,151.20929\",\"acc\":0}}", tok);
        CHECK(admin_post(body) == 200 && xwc_station_pos(pv, sizeof pv, &acc) && acc == 0, "a spot to five decimals");
        snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{\"pos\":\"\"}}", tok);
        CHECK(admin_post(body) == 200 && !xwc_station_pos(pv, sizeof pv, &acc), "cleared, the station says nothing");
    }

    printf("moderation\n");
    snprintf(body, sizeof body, "{\"action\":\"appoint\",\"session\":\"%s\",\"call\":\"X1BOBB\"}", tok);
    CHECK(admin_post(body) == 200 && jfind("\"invited\":[\"X1BOBB\"]"), "the administrator appoints bob");
    xwc_admin_json(s_buf, sizeof s_buf, bt, true);
    {
        char offer[8]; jval("offer", offer, sizeof offer);
        snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:X3STAT ts:%%s r:%s accept:mod", offer);
        CHECK(gate(bob, fmt, 0) == 1 && !strcmp(xwc_role_of("X3STAT", "X1BOBB"), "mod"), "bob accepts");
    }
    xwc_admin_json(s_buf, sizeof s_buf, tok, true);
    CHECK(jfind("\"moderators\":[\"X1ALIC\",\"X1BOBB\"]"), "both are listed as moderators");
    CHECK(gate(carol, "t:message f:%s ts:%s m:spam", 0) == 0, "carol's message goes the ordinary way");
    char spam_id[8]; snprintf(spam_id, sizeof spam_id, "%s", s_id);
    snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:X3STAT ts:%%s r:%s remove:message", spam_id);
    CHECK(gate(bob, fmt, 1) == 1 && aired_has("remove:message"), "a moderator removes it, and the act is aired");
    CHECK(xwc_hide_row("X3STAT", spam_id, "X1CARO", "message", false, true), "the removed message is hidden from history");
    xwc_moderation_json(s_buf, sizeof s_buf, "X3STAT");
    CHECK(strstr(s_buf, spam_id) != NULL, "and listed as removed");
    CHECK(gate(carol, "t:moderate f:%s d:X3STAT ts:%s revoke:X1BOBB", 1) == -403, "carol, no moderator, cannot act");
    CHECK(gate(bob, "t:moderate f:%s d:X3STAT ts:%s grant:X1CARO role:mod", 1) == -403, "only the administrator appoints");
    CHECK(gate(bob, "t:moderate f:%s d:X5OTHR ts:%s revoke:X1CARO", 1) == -403, "only this station's rooms");
    {
        char until[20]; ts(until, 60);
        snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:X3STAT ts:%%s revoke:X1CARO until:%s", until);
        CHECK(gate(bob, fmt, 1) == 1, "bob sends carol away for an hour");
    }
    CHECK(gate(carol, "t:message f:%s ts:%s m:let me back", 2) == -403, "carol cannot post");
    CHECK(gate(carol, "t:sos f:%s ts:%s m:help", 2) == 0, "but her call for help goes through (10.7)");
    CHECK(xwc_hide_row("X3STAT", "aaaaaa", "X1CARO", "message", false, true) &&
          !xwc_hide_row("X3STAT", "bbbbbb", "X1CARO", "sos", false, true),
          "her messages are hidden, her call for help is not (10.7)");
    xwc_admin_json(s_buf, sizeof s_buf, tok, true);
    CHECK(jfind("\"away\":[{\"call\":\"X1CARO\",\"until\":\""), "the administrator sees who was sent away, and until when");
    s_now += 61 * 60;
    CHECK(gate(carol, "t:message f:%s ts:%s m:back", 0) == 0, "the suspension lapses on its own");
    CHECK(gate(bob, "t:moderate f:%s d:X3STAT ts:%s revoke:X1CARO", 1) == 1, "sent away until lifted");
    CHECK(gate(carol, "t:message f:%s ts:%s m:again", 2) == -403, "still refused");
    s_now += 120;
    snprintf(body, sizeof body, "{\"action\":\"lift\",\"session\":\"%s\",\"call\":\"X1CARO\"}", tok);
    CHECK(admin_post(body) == 200 && jfind("\"away\":[]"), "the administrator lifts it");
    CHECK(gate(carol, "t:message f:%s ts:%s m:thanks", 0) == 0, "carol posts again");
    snprintf(body, sizeof body, "{\"action\":\"dismiss\",\"session\":\"%s\",\"call\":\"X1BOBB\"}", tok);
    CHECK(admin_post(body) == 200 && strcmp(xwc_role_of("X3STAT", "X1BOBB"), "mod") != 0, "and dismisses bob");
    CHECK(gate(bob, "t:moderate f:%s d:X3STAT ts:%s r:aaaaaa remove:message", 1) == -403, "who cannot act any more");

    printf("the record survives a restart\n");
    {
        xwc_cb_t cb = s_cb;
        xwc_reset();
        xwc_init(&cb, "X3STAT", s_station->priv);
        int n = xwc_load();
        CHECK(n > 5 && !strcmp(xwc_role_of("X3STAT", "X1ALIC"), "mod") && strcmp(xwc_role_of("X3STAT", "X1BOBB"), "mod") != 0,
              "acts replayed from the file: alice moderates, bob does not");
        CHECK(session(alice, tok), "alice proves her key again");
    }

    printf("hosted rooms\n");
    fresh();
    CHECK(claim(alice, tok), "alice claims and moderates");
    CHECK(session(bob, bt), "bob has a session");
    snprintf(body, sizeof body, "{\"action\":\"create\",\"session\":\"%s\",\"name\":\"Block C\"}", bt);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 403, "only the administrator creates a room");
    snprintf(body, sizeof body, "{\"action\":\"create\",\"session\":\"%s\",\"name\":\"Block C\",\"about\":\"residents\",\"visibility\":\"public\",\"posting\":\"members\"}", tok);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200, "the administrator creates one");
    char room[12]; jval("room", room, sizeof room);
    CHECK(room[0] == 'X' && room[1] == '5' && strlen(room) == 6, "it is an X5 group the station holds");
    CHECK(filed_has("grant:X1ALIC role:mod") && !aired_has(room), "its acts are filed, never aired");
    CHECK(!strcmp(xwc_role_of(room, room), "admin") && !strcmp(xwc_role_of(room, "X1ALIC"), "invited"), "the station is its admin, alice is offered moderating");
    xwc_rooms_json(s_buf, sizeof s_buf, tok);
    {
        char offer[8]; jval("offer", offer, sizeof offer);
        snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:%s ts:%%s r:%s accept:mod", room, offer);
        CHECK(gate(alice, fmt, 0) == 1 && !strcmp(xwc_role_of(room, "X1ALIC"), "mod"), "alice accepts moderating the room");
    }
    snprintf(fmt, sizeof fmt, "t:message f:%%s d:%s ts:%%s m:hi", room);
    CHECK(gate(bob, fmt, 1) == -403, "bob, no member, cannot post in a members-only room");
    snprintf(body, sizeof body, "{\"action\":\"add\",\"session\":\"%s\",\"call\":\"%s\",\"member\":\"X1BOBB\"}", tok, room);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200 && jfind("\"invited\":[\"X1BOBB\"]"), "the administrator adds bob");
    xwc_rooms_json(s_buf, sizeof s_buf, bt);
    CHECK(jfind("\"role\":\"invited\"") && jfind("\"can_post\":false"), "bob sees the invitation");
    {
        char offer[8]; jval("offer", offer, sizeof offer);
        snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:%s ts:%%s r:%s accept:member", room, offer);
        CHECK(gate(bob, fmt, 1) == 1 && !strcmp(xwc_role_of(room, "X1BOBB"), "member"), "bob accepts");
    }
    s_nfiled = 0;
    snprintf(fmt, sizeof fmt, "t:message f:%%s d:%s ts:%%s m:hi", room);
    CHECK(gate(bob, fmt, 2) == 1 && filed_has("m:hi") && !aired_has("m:hi"), "a member's message is filed, not aired");
    CHECK(xwc_may_read_room(room, NULL), "a public room is read by anybody");
    snprintf(body, sizeof body, "{\"action\":\"update\",\"session\":\"%s\",\"call\":\"%s\",\"visibility\":\"private\"}", tok, room);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200 && jfind("\"posting\":\"members\""), "made private: always members-only");
    {
        char ct[XWC_TOKEN_LEN];
        CHECK(session(carol, ct) && !xwc_may_read_room(room, ct) && xwc_may_read_room(room, bt) && xwc_may_read_room(room, tok),
              "a private room is served to members and the administrator only");
        xwc_rooms_json(s_buf, sizeof s_buf, ct);
        CHECK(!jfind(room), "and not even listed to carol");
    }
    snprintf(fmt, sizeof fmt, "t:moderate f:%%s d:%s ts:%%s revoke:X1BOBB", room);
    CHECK(gate(alice, fmt, 3) == 1 && filed_has("revoke:X1BOBB"), "a room moderator sends bob away");
    snprintf(fmt, sizeof fmt, "t:message f:%%s d:%s ts:%%s m:again", room);
    CHECK(gate(bob, fmt, 4) == -403, "bob cannot post there now");
    snprintf(body, sizeof body, "{\"action\":\"close\",\"session\":\"%s\",\"call\":\"%s\"}", tok, room);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200 && !jfind(room), "closed");

    printf("forum rooms\n");
    fresh();
    CHECK(claim(alice, tok), "alice claims");
    snprintf(body, sizeof body, "{\"action\":\"create\",\"session\":\"%s\",\"name\":\"Talk\",\"posting\":\"anyone\"}", tok);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200 && jfind("\"kind\":\"chat\""), "a room is a chat room by default");
    jval("room", room, sizeof room);
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s title:hello m:x", room);
    CHECK(gate(bob, fmt, 0) == -403, "posts go in a forum room");
    CHECK(xwc_topics_begin(room, tok, s_buf, sizeof s_buf) == 400, "and a chat room has no index");
    snprintf(body, sizeof body, "{\"action\":\"update\",\"session\":\"%s\",\"call\":\"%s\",\"kind\":\"forum\"}", tok, room);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200 && jfind("\"kind\":\"forum\""), "made a forum");
    snprintf(body, sizeof body, "{\"action\":\"update\",\"session\":\"%s\",\"call\":\"%s\",\"kind\":\"wiki\"}", tok, room);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 400, "an unknown kind is refused");
    s_nfiled = 0;
    CHECK(gate(bob, fmt, 0) == 1, "bob opens a topic");
    char topic[8]; snprintf(topic, sizeof topic, "%s", s_id);
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s title:Hello m:x", room);
    CHECK(gate(bob, fmt, 1) == -400, "a title is a label");
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s m:x", room);
    CHECK(gate(bob, fmt, 1) == -400, "a post with neither title nor r: is refused");
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s r:%s m:agreed", room, topic);
    CHECK(gate(carol, fmt, 2) == 1, "carol comments");
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s title:hello m:x again", room);
    CHECK(gate(bob, fmt, 3) == 1, "bob edits his topic");
    CHECK(xwc_topics_begin(room, NULL, s_buf, sizeof s_buf) == 200, "a public forum's index is anybody's");
    for (int i = s_nfiled - 1; i >= 0; i--) xwc_topics_row(s_filed[i], (int)strlen(s_filed[i]), s_now + (uint32_t)i * 60);
    CHECK(xwc_topics_end(s_buf, sizeof s_buf) == 200 && jfind("\"title\":\"hello\"") &&
          jfind("\"replies\":1") && jfind("\"edited\":true") && strstr(s_buf, "},{") == NULL,
          "one topic, two editions, one comment");

    printf("gallery rooms\n");
    fresh();
    CHECK(claim(alice, tok), "alice claims");
    CHECK(session(bob, bt), "bob has a session");
    snprintf(body, sizeof body, "{\"action\":\"create\",\"session\":\"%s\",\"name\":\"Photos\",\"kind\":\"gallery\"}", tok);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200 && jfind("\"kind\":\"gallery\""), "the administrator creates a gallery");
    jval("room", room, sizeof room);
    s_nfiled = 0;
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s title:field-day-2026 m:On the hill", room);
    CHECK(gate(alice, fmt, 0) == 1, "the administrator opens an event");
    char eid[8]; snprintf(eid, sizeof eid, "%s", s_id);
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s r:%s m:no title", room, eid);
    CHECK(gate(alice, fmt, 1) == -400, "an event has a title; a gallery takes no comment");
    snprintf(fmt, sizeof fmt, "t:message f:%%s d:%s ts:%%s r:%s file:nYxKzGm4vT2pQ8dW5jR7cL0aFbNs9hUe3oXiC6EkM1w.jpg m:the mast", room, eid);
    CHECK(gate(alice, fmt, 2) == 1, "a picture: the preview on a message");
    char pid[8]; snprintf(pid, sizeof pid, "%s", s_id);
    snprintf(fmt, sizeof fmt, "t:file f:%%s d:%s ts:%%s r:%s file:Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.jpg size:2.1MB name:mast.jpg", room, pid);
    CHECK(gate(alice, fmt, 2) == 1, "the original on its t:file companion");
    snprintf(fmt, sizeof fmt, "t:message f:%%s d:%s ts:%%s r:%s file:nYxKzGm4vT2pQ8dW5jR7cL0aFbNs9hUe3oXiC6EkM1w.jpg", room, eid);
    CHECK(gate(bob, fmt, 3) == -403, "a visitor adds no picture");
    snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s title:mine m:x", room);
    CHECK(gate(bob, fmt, 3) == -403 && strstr(s_why, "moderators"), "nor an event");
    snprintf(fmt, sizeof fmt, "t:reaction f:%%s d:%s ts:%%s r:%s add:like", room, pid);
    CHECK(gate(bob, fmt, 3) == 1, "a visitor likes a picture");
    snprintf(fmt, sizeof fmt, "t:reaction f:%%s d:%s ts:%%s r:%s add:like", room, eid);
    CHECK(gate(bob, fmt, 3) == 1, "and an event");
    snprintf(fmt, sizeof fmt, "t:status f:%%s d:%s ts:%%s m:x", room);
    CHECK(gate(alice, fmt, 4) == -403 && strstr(s_why, "folders, items and likes"), "a gallery takes nothing else");
    CHECK(!aired_has(room) && filed_has("t:file"), "filed, never aired");
    CHECK(xwc_topics_begin(room, bt, s_buf, sizeof s_buf) == 200, "the events are the room's index");
    for (int i = s_nfiled - 1; i >= 0; i--) xwc_topics_row(s_filed[i], (int)strlen(s_filed[i]), s_now + (uint32_t)i * 60);
    CHECK(xwc_topics_end(s_buf, sizeof s_buf) == 200 && jfind("\"title\":\"field-day-2026\"") && strstr(s_buf, "},{") == NULL,
          "one event, listed");
    snprintf(body, sizeof body, "{\"action\":\"update\",\"session\":\"%s\",\"call\":\"%s\",\"visibility\":\"private\"}", tok, room);
    CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200, "made private");
    snprintf(fmt, sizeof fmt, "t:reaction f:%%s d:%s ts:%%s r:%s add:like", room, eid);
    CHECK(gate(carol, fmt, 5) == -403, "a private gallery is liked by its members only");
    {
        xwc_cb_t cb = s_cb;
        xwc_reset();
        xwc_init(&cb, "X3STAT", s_station->priv);
        xwc_load();
        CHECK(xwc_room_of(room) != NULL, "the room survives a restart");
        CHECK(session(alice, tok), "alice proves her key again");
        xwc_rooms_json(s_buf, sizeof s_buf, tok);
        CHECK(jfind("\"kind\":\"gallery\""), "still a gallery");
    }

    printf("an archive that cannot take it\n");
    {
        /* Still the gallery room from above (the last room made). */
        snprintf(fmt, sizeof fmt, "t:reaction f:%%s d:%s ts:%%s r:%s add:like", room, eid);
        s_file_result = XWC_FILE_BUSY;
        int before = s_nfiled;
        CHECK(gate(alice, fmt, 6) == -503 && !strcmp(s_why, "busy, try again") && s_nfiled == before,
              "a full write queue: 503, and nothing claimed filed");
        s_file_result = XWC_FILE_HELD;
        CHECK(gate(alice, fmt, 6) == 1, "kept already (heard before): still 200");
        s_file_result = XWC_FILED;
        CHECK(gate(alice, fmt, 6) == 1, "and kept: 200");
    }

    printf("mailbox declarations and receipts (9.12, 9.7.1)\n");
    fresh();
    {
        person_t *dave = person("X1DAVE", 0x44), *eve = person("X1EVEE", 0x55);
        char w[300], t[20];
        ts(t, 0);
        int n = snprintf(w, sizeof w, "t:mailbox f:X1DAVE ts:%s hold:X3STAT", t);
        n = xprsid_sign(w, n, sizeof w, eve->priv);
        CHECK(xwc_gate(w, n, true, s_id, s_why, sizeof s_why) == -400 && !strcmp(s_why, "forged"),
              "a declaration another key signed in his name is refused (the conformance case)");
        CHECK(gate(dave, "t:mailbox f:%s ts:%s hold:X3STAT", 0) == 0, "his own goes the ordinary way");
        n = snprintf(w, sizeof w, "t:mailbox f:X1DAVE ts:%s hold:X3STAT", t);
        CHECK(xwc_gate(w, n, true, s_id, s_why, sizeof s_why) == -400, "an unsigned one is refused");
        n = snprintf(w, sizeof w, "t:mailbox f:X1NOBO ts:%s hold:X3STAT", t);
        n = xprsid_sign(w, n, sizeof w, eve->priv);
        CHECK(xwc_gate(w, n, true, s_id, s_why, sizeof s_why) == -409, "one for a callsign with no known key: send the identity first");
        n = snprintf(w, sizeof w, "t:receipt f:X1DAVE d:X1EVEE ts:%s r:abcdef", t);
        n = xprsid_sign(w, n, sizeof w, eve->priv);
        CHECK(xwc_gate(w, n, true, s_id, s_why, sizeof s_why) == -400, "a receipt in his name by another key too");
        CHECK(gate(dave, "t:receipt f:%s d:X1EVEE ts:%s r:abcdef", 0) == 0, "his own receipt goes through");
    }

    printf("music and library rooms\n");
    fresh();
    CHECK(claim(alice, tok), "alice claims");
    {
        /* The station's own key is its operator (as the Dart core). */
        char st[XWC_TOKEN_LEN];
        CHECK(session(s_station, st), "the station proves its own key");
        xwc_admin_json(s_buf, sizeof s_buf, st, false);
        CHECK(jfind("\"you\":true"), "and is the administrator, beside the one who claimed it");
        snprintf(body, sizeof body, "{\"action\":\"create\",\"session\":\"%s\",\"name\":\"x\",\"kind\":\"video\"}", st);
        CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 400 && jfind("music or library"), "an unknown kind is refused");
        const char *kinds[2] = { "music", "library" };
        for (int k = 0; k < 2; k++) {
            char what[96];
            snprintf(body, sizeof body, "{\"action\":\"create\",\"session\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\"}", st, kinds[k], kinds[k]);
            snprintf(what, sizeof what, "the station makes a %s room", kinds[k]);
            CHECK(xwc_rooms_post(body, s_buf, sizeof s_buf) == 200, what);
            jval("room", room, sizeof room);
            snprintf(fmt, sizeof fmt, "t:blog f:%%s d:%s ts:%%s title:first-album m:demo", room);
            CHECK(gate(s_station, fmt, 0) == 1, "and opens a folder");
            char aid[8]; snprintf(aid, sizeof aid, "%s", s_id);
            snprintf(fmt, sizeof fmt, "t:message f:%%s d:%s ts:%%s r:%s m:opening-song", room, aid);
            CHECK(gate(s_station, fmt, 1) == 1, "an item with no preview: a message naming it");
            char tid[8]; snprintf(tid, sizeof tid, "%s", s_id);
            snprintf(fmt, sizeof fmt, "t:file f:%%s d:%s ts:%%s r:%s file:Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.mp3 size:4.2MB name:opening-song.mp3", room, tid);
            CHECK(gate(s_station, fmt, 1) == 1, "the file on its t:file companion");
            snprintf(fmt, sizeof fmt, "t:message f:%%s d:%s ts:%%s r:%s m:mine", room, aid);
            CHECK(gate(bob, fmt, 2) == -403, "a visitor adds nothing");
            snprintf(fmt, sizeof fmt, "t:reaction f:%%s d:%s ts:%%s r:%s add:like", room, tid);
            CHECK(gate(bob, fmt, 2) == 1, "and likes");
            snprintf(fmt, sizeof fmt, "t:status f:%%s d:%s ts:%%s m:x", room);
            CHECK(gate(s_station, fmt, 3) == -403 && strstr(s_why, "folders, items and likes"), "a collection takes nothing else");
            CHECK(xwc_topics_begin(room, NULL, s_buf, sizeof s_buf) == 200, "and has an index");
            xwc_topics_end(s_buf, sizeof s_buf);
        }
        CHECK(xwc_is_uploader(st) && xwc_is_uploader(tok), "the station and its administrator store the station's own files");
        CHECK(session(bob, bt) && !xwc_is_uploader(bt), "a visitor does not");
        xwc_cb_t cb = s_cb;
        xwc_reset();
        xwc_init(&cb, "X3STAT", s_station->priv);
        xwc_load();
        CHECK(session(alice, tok), "after a restart");
        xwc_rooms_json(s_buf, sizeof s_buf, tok);
        CHECK(jfind("\"kind\":\"music\"") && jfind("\"kind\":\"library\""), "the kinds are kept");
    }

    printf("the station proving itself (/xprs/prove)\n");
    fresh();
    {
        const char *m = "00112233445566778899aabbccddeeff.n1700000000123";
        char w[400];
        int n = xwc_station_session(m, w, sizeof w);
        char head[64];
        char t[20]; ts(t, 0);
        snprintf(head, sizeof head, "t:session f:X3STAT ts:%s sig:", t);
        CHECK(n > 0 && !strncmp(w, head, strlen(head)), "t:session f:<station> ts:<now>, sig: before m:");
        CHECK(n > 0 && strstr(w, " m:") && !strcmp(strstr(w, " m:") + 3, m), "m: the link id, a dot, the nonce, last");
        xprs_t p;
        CHECK(n > 0 && xprs_parse(w, n, &p) && xprsid_verify(&p, s_station->pub), "signed by the station's own key");
        CHECK(xwc_station_session(m, w, 40) == 0, "no room: no proof rather than half of one");
    }

    printf("alerts\n");
    fresh();
    CHECK(claim(alice, tok), "alice claims");
    CHECK(gate(bob, "t:warning f:%s kind:outage sev:watch ts:%s m:power cut", 0) == -403, "by default a visitor may not post an alert");
    CHECK(gate(alice, "t:warning f:%s kind:outage sev:watch ts:%s m:power cut", 0) == 0, "the administrator may");
    CHECK(gate(bob, "t:warning f:%s ts:%s r:3c7e21 remove:warning", 0) == 0, "an author always withdraws their own");
    snprintf(body, sizeof body, "{\"action\":\"set\",\"session\":\"%s\",\"settings\":{\"who_may_alert\":\"everyone\"}}", tok);
    admin_post(body);
    CHECK(gate(bob, "t:warning f:%s kind:outage sev:watch ts:%s m:power cut", 0) == 0, "opened to everyone, bob may");
    CHECK(gate(bob, "t:sos f:%s ts:%s m:help", 0) == 0, "an sos is never gated here");

    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
