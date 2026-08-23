/* xprs_auth.c -- see the header. The order of the checks is the point:
 * cheap and silent first, expensive and answerable last, so a stranger
 * shouting at the station costs it a string compare and not a curve
 * operation. */
#include "xprs_auth.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "bech32.h"
#include "esp_log.h"
#include "nostr_keys.h"
#include "xprs_config.h"
#include "xprsid.h"
#include "xprssig.h"

static const char *TAG = "xauth";

/* Answered commands: id -> what we said, and when. Eight is plenty; a
 * command that is still being repeated ten minutes later is a network
 * problem, not a memory problem. */
static struct {
    char     id[8];
    uint32_t when;
    int      code;
} s_seen[8];

/* Do two callsigns name the SAME KEY?
 *
 * A callsign is a prefix plus characters derived from the key (section 3):
 * X1 for a person, X3 for a station, X4 for a device, X5 for a nameless
 * one. The same npub therefore yields X1Q3Q5 on a phone and X3Q3Q5 on a
 * board, and an allow-list that compared the whole string would refuse the
 * very operator it was written for. What the key decides is everything
 * after the prefix, so that is what is compared -- and the device suffix
 * (X1QZ3N-7 is X1QZ3N's tablet, 3.1) is ignored on both sides. */
static bool key_eq(const char *a, const char *b)
{
    if (!a || !b) return false;
    if (a[0] == 'X' || a[0] == 'x') a += 2;    /* skip the kind digit */
    if (b[0] == 'X' || b[0] == 'x') b += 2;
    while (*a && *b && *a != '-' && *b != '-') {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return (*a == 0 || *a == '-') && (*b == 0 || *b == '-');
}

/* The base callsign: X1QZ3N-7 is X1QZ3N's device (3.1). */
static bool base_eq(const char *a, const char *b)
{
    while (*a && *b && *a != '-' && *b != '-') {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return (*a == 0 || *a == '-') && (*b == 0 || *b == '-');
}

static uint32_t now_sec(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;   /* 0 = no clock */
}

/* `ts:` (YYYY-MM-DD_hh:mm:ss UTC, section 4.3) as epoch seconds, 0 when it
 * is not one. Local rather than borrowed from the index: this gate must be
 * usable on a board with no storage component at all. */
static uint32_t ts_epoch(const char *s)
{
    int y, mo, d, h, mi, se;
    if (!s || sscanf(s, "%4d-%2d-%2d_%2d:%2d:%2d",
                     &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    /* Days from civil (Howard Hinnant), so no timegm and no timezone. */
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + h * 3600 + mi * 60 + se);
}

/* One allow-list entry, by index. Empty when unset. */
static const char *owner_npub(int i)
{
    static const char *const keys[XAUTH_OWNERS_MAX] = {
        "own1", "own2", "own3", "own4"
    };
    if (i < 0 || i >= XAUTH_OWNERS_MAX) return "";
#ifdef XAUTH_BENCH_NPUB
    /* Bench only: slot 0 is a compiled-in owner, never written to config.
     * Pairs with XOTA_BENCH_PUBHEX; same warning, same reason. */
    if (i == 0) { ESP_LOGE(TAG, "BENCH OWNER in use (XAUTH_BENCH_NPUB)"); return XAUTH_BENCH_NPUB; }
#endif
    return xcfg_get(keys[i], "");
}

bool xauth_is_owner(const char *call)
{
    if (!call || !call[0]) return false;
    for (int i = 0; i < XAUTH_OWNERS_MAX; i++) {
        const char *npub = owner_npub(i);
        if (!npub[0]) continue;
        char derived[NOSTR_CALLSIGN_LEN] = "";
        if (nostr_keys_derive_callsign(npub, derived) != ESP_OK) continue;
        if (key_eq(derived, call)) return true;
    }
    return false;
}

/* The allow-listed key whose callsign is [call], as 32 x-only bytes.
 *
 * This is what makes the gate airtight without any trust-on-first-use: an
 * X3 callsign is DERIVED from its key (section 3), so a forger cannot
 * claim an allow-listed callsign without holding the private half. The
 * TOFU table the station keeps for other purposes is deliberately not
 * consulted here -- it learns from the air, and the air is where the
 * attacker is. */
static bool owner_key(const char *call, uint8_t out[32])
{
    for (int i = 0; i < XAUTH_OWNERS_MAX; i++) {
        const char *npub = owner_npub(i);
        if (!npub[0]) continue;
        char derived[NOSTR_CALLSIGN_LEN] = "";
        if (nostr_keys_derive_callsign(npub, derived) != ESP_OK) continue;
        if (!key_eq(derived, call)) continue;
        char hrp[8] = "";
        uint8_t buf[64];
        size_t n = sizeof buf;
        if (bech32_decode(npub, hrp, buf, &n) != ESP_OK) return false;
        if (n != 32) return false;
        memcpy(out, buf, 32);
        return true;
    }
    return false;
}

void xauth_remember(const char *id, int code)
{
    if (!id || !id[0]) return;
    uint32_t t = now_sec();
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (strcmp(s_seen[i].id, id) == 0) { slot = i; break; }
        if (!s_seen[i].id[0]) { slot = i; break; }
        if (s_seen[i].when < s_seen[slot].when) slot = i;
    }
    snprintf(s_seen[slot].id, sizeof s_seen[slot].id, "%s", id);
    s_seen[slot].when = t ? t : 1;
    s_seen[slot].code = code;
}

static bool seen_before(const char *id, uint32_t t, int *prev_code)
{
    for (int i = 0; i < 8; i++) {
        if (!s_seen[i].id[0] || strcmp(s_seen[i].id, id) != 0) continue;
        if (t && t - s_seen[i].when >= XAUTH_REPLAY_SEC) return false;
        if (prev_code) *prev_code = s_seen[i].code;
        return true;
    }
    return false;
}

xauth_verdict_t xauth_check(const xprs_t *p, const char *self_call,
                            char id_out[8], char from_out[16], int *prev_code)
{
    if (id_out) id_out[0] = 0;
    if (from_out) from_out[0] = 0;
    if (prev_code) *prev_code = 0;

    /* 1. Addressed to us. A broadcast is never an authorisation: a command
     *    nobody was named in is a command anybody may replay at anybody. */
    char dst[16] = "";
    if (!xprs_get_str(p, "d", dst, sizeof dst) || !dst[0]) return XAUTH_SILENT;
    if (!base_eq(dst, self_call)) return XAUTH_SILENT;

    /* 2. Heard directly. 25.4: commands are never carried, and a copy that
     *    reached us through a relay says nothing about who is in range. */
    if (xprs_get(p, "via", NULL) != NULL) return XAUTH_SILENT;

    /* 3. Attributable and signed. */
    char from[16] = "";
    if (!xprs_get_str(p, "f", from, sizeof from) || !from[0]) return XAUTH_SILENT;
    if (from_out) snprintf(from_out, 16, "%s", from);
    if (xprs_get(p, "sig", NULL) == NULL) return XAUTH_SILENT;

    /* 4. On the allow-list, and the signature proves the callsign. A signer
     *    we cannot place gets nothing: not a 403, not a byte. */
    uint8_t pub[32];
    if (!owner_key(from, pub)) {
        ESP_LOGW(TAG, "command from %s: not on the allow list", from);
        return XAUTH_403;
    }
    if (!xprsid_verify(p, pub)) {
        /* "Does not verify" and "could not be verified" are different
         * facts. On a small board a 1.4 MB upload can leave the curve
         * maths with no memory, and answering a good signature with
         * silence -- the answer a forgery gets -- made a station look
         * unreachable when it was merely busy. */
        if (xprssig_last_result() == XPRSSIG_NO_MEM) {
            ESP_LOGW(TAG, "no memory to verify %s right now -- ask again", from);
            return XAUTH_429;
        }
        ESP_LOGW(TAG, "command claiming %s does not verify -- discarded", from);
        return XAUTH_SILENT;
    }

    /* 5. Fresh. A station with no clock cannot judge this and must not
     *    guess: an old command replayed at a clockless node is exactly the
     *    attack the window exists to stop. */
    uint32_t t = now_sec();
    if (!t) return XAUTH_408;
    char ts[24] = "";
    if (!xprs_get_str(p, "ts", ts, sizeof ts)) return XAUTH_408;
    uint32_t when = ts_epoch(ts);
    if (!when) return XAUTH_408;
    if (when > t + 60) return XAUTH_408;              /* from the future */
    if (t - when > XAUTH_WINDOW_SEC) return XAUTH_408;

    /* 6. Idempotent. The identifier is derived, so a repeat is the same
     *    command and gets the same answer without doing the work twice. */
    char id[XPRS_ID_LEN];
    xprs_id(p, id);
    if (id_out) snprintf(id_out, 8, "%s", id);
    if (seen_before(id, t, prev_code)) return XAUTH_REPEAT;

    return XAUTH_OK;
}

xauth_verdict_t xauth_check_http(const char *auth_header, const char *self_call,
                                 const char *body_sha16, char from_out[16])
{
    if (!auth_header || !auth_header[0]) return XAUTH_SILENT;
    xprs_t p;
    int len = (int)strlen(auth_header);
    if (len > XPRS_MAX_WIRE) return XAUTH_SILENT;
    if (!xprs_parse(auth_header, len, &p)) return XAUTH_SILENT;

    char id[8];
    int prev = 0;
    xauth_verdict_t v = xauth_check(&p, self_call, id, from_out, &prev);
    if (v != XAUTH_OK && v != XAUTH_REPEAT) return v;

    /* The body binding. Without it an authorisation lifted from one
     * request would install anything: same signer, same window, different
     * bytes. With it the signature covers what is being asked for. */
    if (body_sha16 && body_sha16[0]) {
        char claimed[20] = "";
        if (!xprs_get_str(&p, "zsha", claimed, sizeof claimed)) return XAUTH_403;
        if (strncasecmp(claimed, body_sha16, 16) != 0) {
            ESP_LOGW(TAG, "auth header does not match this body");
            return XAUTH_403;
        }
    }
    /* An HTTP request is not the air: a repeat here is a retry, and the
     * caller wants it done, not echoed. */
    if (v == XAUTH_REPEAT) return XAUTH_OK;
    xauth_remember(id, 202);
    return XAUTH_OK;
}
