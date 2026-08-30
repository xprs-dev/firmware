/**
 * @file xprs_auth.h
 * @brief May this packet make this station DO something? (XPRS.md 25.4)
 *
 * One gate, three doors. A signed `t:command` is the authorisation object
 * whether it arrived over ESP-NOW, over the LAN bearer, or as an HTTP
 * header on the local network -- so there is one allow-list, one replay
 * ring, one freshness window and one code path to get right.
 *
 * The rules are the specification's, not this file's inventions:
 *
 *   - unsigned, or a signature that does not verify, is DISCARDED AND
 *     NEVER ANSWERED. Answering a forgery tells the forger the station
 *     is listening and gives them a free packet to replay.
 *   - a verified signer who is not on the allow-list gets `code:403` out
 *     loud, because silence and refusal look identical to the asker and
 *     mean opposite things (31.2).
 *   - a command expires. 300 seconds from `ts:`, and a station with no
 *     clock cannot check that, so it refuses with `code:408` rather than
 *     acting on a timestamp it cannot judge.
 *   - a command is never carried. A packet wearing `via:` reached us
 *     through somebody else and is not evidence of a peer in range.
 *   - repeating a command does not repeat the action: the section-5
 *     identifier is remembered and the same answer is re-aired.
 *
 * WHAT THIS IS NOT. Authentication is not authorisation. This proves the
 * callsign holds its key -- for an X3 callsign, which DERIVES from the
 * key, that is airtight and needs no trust-on-first-use. Whether that
 * callsign may reflash this station is the allow-list, which the owner
 * writes and this file only reads.
 *
 * The allow-list is re-writable with a cable, deliberately: a lost key is
 * a ladder, never a brick. See docs/device.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How stale a command may be before it is refused (25.4). */
#define XAUTH_WINDOW_SEC   300
/** How long an answered command id is remembered, so a repeat is idempotent. */
#define XAUTH_REPLAY_SEC   600
/** Allow-list entries (config keys own1..own4). */
#define XAUTH_OWNERS_MAX   4

typedef enum {
    XAUTH_OK = 0,     /**< act on it */
    XAUTH_SILENT,     /**< discard, answer nothing at all */
    XAUTH_403,        /**< verified, but not permitted here */
    XAUTH_408,        /**< expired, or this station has no clock */
    XAUTH_429,        /**< busy: the station is already doing this, or had
                       *   no memory to check the signature -- ask again */
    XAUTH_REPEAT      /**< seen before: re-air [prev_code], do not act */
} xauth_verdict_t;

/**
 * Decide whether [p] may act on this station.
 *
 * @param p          the parsed packet (a t:command or t:request)
 * @param self_call  this station's callsign
 * @param id_out     7 bytes: the section-5 identifier, for the `r:` of the answer
 * @param from_out   16 bytes: the sender's callsign, for the `d:` of the answer
 * @param prev_code  set when the verdict is XAUTH_REPEAT: the code answered before
 */
xauth_verdict_t xauth_check(const xprs_t *p, const char *self_call,
                            char id_out[8], char from_out[16], int *prev_code);

/**
 * Remember what was answered for [id], so a repeat re-airs it rather than
 * acting twice. Call once, with the code actually aired.
 */
void xauth_remember(const char *id, int code);

/**
 * Is [call] on the allow-list at all? For the HTTP door, which has a
 * callsign in a header rather than a packet in the air.
 */
bool xauth_is_owner(const char *call);

/**
 * The allow-listed key behind [call], as 32 x-only bytes. For a board that
 * has to learn something else from an owner before it can take a command
 * -- the clockless P1-Pro takes the time from an owner's signed packet.
 */
bool xauth_owner_key_of(const char *call, uint8_t out[32]);

/**
 * The HTTP door. [auth_header] is a complete signed XPRS command wire, and
 * [body_sha16] is the first 16 hex characters of sha256(request body) which
 * the wire must carry in `zsha:` -- so an authorisation captured from one
 * request cannot be replayed onto a different body.
 * NULL [body_sha16] skips that binding (for bodiless requests).
 */
xauth_verdict_t xauth_check_http(const char *auth_header, const char *self_call,
                                 const char *body_sha16, char from_out[16]);

#ifdef __cplusplus
}
#endif
