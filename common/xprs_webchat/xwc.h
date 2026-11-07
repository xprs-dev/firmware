/*
 * xwc -- the engine behind the web chat's station API (spec/API-HTTP.md
 * /api/session, /api/admin, /api/rooms, and the rules /api/xprs/history and
 * /api/xprs/send apply), XPRS.md 10.10, 10.10.1 and 10.10.2.
 *
 * The web chat page is one page for every platform; a board, a phone or a
 * server is the engine behind the same API. This is that engine for the
 * ESP32 boards, the C counterpart of the Dart core's WebChatApi, and the
 * webchat repo's conformance suite (contract/conformance.py) is what says
 * the two agree.
 *
 * Plain C, no IDF: time, storage, the peer-key table and the transmitter
 * come in through xwc_cb_t, so test_xwc_host.sh runs the whole thing on a
 * laptop. xwc_http.c is the httpd glue.
 *
 * The station is the group of its own rooms: `d:` is its callsign, its key
 * signs the administrator's acts, moderators sign theirs in the browser. A
 * hosted room is a closed group whose key this station holds; its traffic is
 * filed here and never aired. Every act is a signed `t:moderate`, kept in a
 * file and replayed (10.4) into rosters; the answers the API needs are then
 * table lookups.
 */
#ifndef XWC_H
#define XWC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XWC_CALL_LEN     12
#define XWC_ID_LEN       7
#define XWC_TOKEN_LEN    33      /* 32 hex + NUL */
#define XWC_NONCE_LEN    17      /* 16 hex + NUL */
/* On a board without PSRAM (the C3, the T-Dongle) the store is a third the
 * size: at 8 rooms it held 21 KB, and the C3's hotspot and the T-Dongle's
 * screen with Bluetooth on no longer fit (2026-11-07). */
#if defined(ESP_PLATFORM) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && !(defined(CONFIG_SPIRAM) && CONFIG_SPIRAM)
#define XWC_ROOMS        3
#else
#define XWC_ROOMS        8
#endif
#define XWC_ROOM_NAME    33
#define XWC_ROOM_ABOUT   81

/* What the host supplies. Every pointer must outlive xwc. */
typedef struct {
    uint32_t (*now)(void);                              /* epoch seconds, 0 if unknown */
    uint32_t (*uptime)(void);                           /* seconds since boot: nonces and sessions
                                                           age by this, not by a clock that may
                                                           jump when NTP sets it */
    const uint8_t *(*peer_key)(const char *call);       /* x-only key, or NULL */
    void (*random_bytes)(uint8_t *buf, size_t n);
    /* Small strings by key. kv_get returns NULL when absent. */
    const char *(*kv_get)(const char *key);
    bool (*kv_set)(const char *key, const char *val);
    /* Put a wire on the air (the station's own acts, and what the rules
     * let through) -- the API's ordinary send path. */
    bool (*air)(const char *wire, int len);
    /* File a wire without airing it (a hosted room's traffic and acts). */
    bool (*file)(const char *wire, int len);
    /* Where the acts are kept, one wire per line. NULL: memory only. */
    const char *acts_path;
} xwc_cb_t;

/** Start. [station_priv] is the station's secp256k1 scalar (32 bytes). */
void xwc_init(const xwc_cb_t *cb, const char *station_call,
              const uint8_t *station_priv);
/** Replay the acts file. Call once after xwc_init. Returns acts replayed. */
int xwc_load(void);

/* ---- sessions (/api/session) ------------------------------------------- */

/** A one-time nonce for the asker. */
void xwc_nonce(char out[XWC_NONCE_LEN]);
/** `t:session f:<call> ts: m:<nonce>` signed by the key this station holds
 *  for the callsign. Returns the HTTP status; 200 fills [token] and [call]. */
int xwc_prove(const char *wire, int len, char token[XWC_TOKEN_LEN],
              char call[XWC_CALL_LEN], char *why, size_t why_cap);
/** The callsign a session proves, or NULL. */
const char *xwc_session_call(const char *token);

/* ---- the administrator and settings (/api/admin) ------------------------ */

/** GET /api/admin: the state for [token], as JSON. */
int xwc_admin_json(char *buf, size_t cap, const char *token, bool local);
/** POST /api/admin with [body]; the HTTP status, and the answer in [buf]. */
int xwc_admin_post(const char *body, bool local, char *buf, size_t cap);

/** The settings as the rules read them. */
bool        xwc_local_private(void);
const char *xwc_who_may_post(void);    /* everyone | local | nobody */
const char *xwc_who_may_alert(void);   /* moderators | everyone */
const char *xwc_admin_call(void);      /* or NULL */

/* ---- hosted rooms (/api/rooms) ------------------------------------------- */

/** The hosted room [v] names (an X5 callsign this station holds), or NULL. */
const char *xwc_room_of(const char *v);
/** The public key of a hosted room, for the host's verifier. */
const uint8_t *xwc_room_pub(const char *call);
bool xwc_may_read_room(const char *room, const char *token);
int  xwc_rooms_json(char *buf, size_t cap, const char *token);
int  xwc_rooms_post(const char *body, char *buf, size_t cap);
bool xwc_has_rooms(void);

/* ---- the rules on what visitors read and send -------------------------- */

/** True when a history row must be left out: removed by a moderator, by an
 *  author sent away (safety traffic excepted), or Local kept from outside. */
bool xwc_hide_row(const char *group, const char *id, const char *from,
                  const char *type, bool scope_local, bool local_peer);
/** `"moderation":{...}` for [group] (the station, or a hosted room). */
int  xwc_moderation_json(char *buf, size_t cap, const char *group);

/** The gate on POST /api/xprs/send, before the ordinary send path.
 *   0   let the ordinary path air it
 *   1   handled here (filed, or aired as an act); [id] is set
 *  <0   refuse: the negated HTTP status, [why] says why */
int xwc_gate(const char *wire, int len, bool local_peer,
             char id[XWC_ID_LEN], char *why, size_t why_cap);

/** The fragment /api/services adds: `"station":..,"page":{..}` (no braces). */
int xwc_services_json(char *buf, size_t cap, const char *tabs_csv);

/* ---- for the host test ----------------------------------------------- */
void xwc_reset(void);
const char *xwc_role_of(const char *group, const char *call);  /* none|invited|member|mod|admin */

#ifdef __cplusplus
}
#endif
#endif /* XWC_H */
