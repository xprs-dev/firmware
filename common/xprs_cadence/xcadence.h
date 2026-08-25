/*
 * xcadence -- how often to ask ONE archiver for news (XPRS.md 36.10.2).
 *
 * A port of the phone's xprs_cadence.dart, and pure for the same reason it
 * is: a poll interval is a battery setting and a load setting, so it has to
 * be justifiable and testable without a radio. No clock, no I/O, no state.
 *
 * Every station used to ask every archiver on the same clock forever: a room
 * nobody had spoken in for three months cost the same metered replay as one
 * with a conversation running. On a super-archiver -- the station everybody
 * pulls the public rooms from -- that fixed clock IS the load.
 *
 * So the interval follows the room. What an archiver returns is the only
 * honest measure of how busy it is, and it is free: the answer is already
 * coming back. Rows -> come sooner. Nothing -> come later.
 *
 * Neither bound is ours to choose:
 *
 *   the CEILING is how long the archiver has been silent. Something that has
 *   said nothing for three months does not need asking every ten minutes.
 *
 *   the FLOOR is what the peer permits. Section 31.2 lets an ordinary
 *   archiver answer a known caller six times an hour, and 36.10.1 says the
 *   ten-minute poll IS that ceiling rather than an arbitrary number. Only a
 *   super-archiver's raised budgets (36.9.4) can serve a fast caller, so only
 *   a super gets one -- a station that polls an ordinary peer faster steals
 *   that peer's whole cross-caller allowance and 429-starves everyone else.
 *
 * And a 429 is not noise. It is the peer saying our cadence is wrong, and it
 * is the one answer that must always slow us down.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What an archiver's last answer was worth. */
typedef enum {
    XC_NEWS = 0,    /**< it served rows we had not seen: it is talking */
    XC_QUIET,       /**< it answered and there was nothing new (200 empty, 404) */
    XC_REFUSED,     /**< 429: the peer is the authority on how often it answers */
} xc_answer_t;

/** How reachable an archiver is, which decides how fast it may be asked. */
typedef enum {
    XC_FAST = 0,    /**< `serve:archive,super` (36.9.4): absorbs a fast caller */
    XC_ORDINARY,    /**< anything else: 31.2's reference budgets apply */
} xc_peer_t;

/* Seconds. The same numbers as the phone, because the two have to be the same
 * caller as far as a responder's budget is concerned. */
#define XC_FAST_FLOOR        15u
#define XC_ORDINARY_FLOOR   600u
#define XC_HIDDEN_FLOOR     600u
#define XC_CEIL_FRESH       600u
#define XC_CEIL_WEEK       3600u
#define XC_CEIL_QUARTER   21600u
#define XC_WEEK_SILENT   (7u * 24u * 3600u)
#define XC_QUARTER_SILENT (90u * 24u * 3600u)
#define XC_REFUSED_MIN       60u

/** The slowest this archiver is worth asking, given how long it has been quiet. */
uint32_t xcadence_ceiling(uint32_t silent_for_s);

/** The fastest it may be asked. @p visible is whether anybody is looking. */
uint32_t xcadence_floor(xc_peer_t peer, bool visible);

/**
 * @brief The next interval, given what this archiver just answered.
 *
 * Halve on news, double on quiet -- it converges fast when a room wakes up
 * and decays gently when it does not. A refusal is deliberately NOT clamped
 * to the floor: being refused means the floor was wrong for this peer.
 */
uint32_t xcadence_next(uint32_t current_s, xc_answer_t answer,
                       xc_peer_t peer, bool visible, uint32_t silent_for_s);

/** What to start an unknown archiver on: polite by default, earns its speed. */
uint32_t xcadence_initial(void);

/**
 * @brief Spread the herd, +/-10%.
 *
 * Many stations pulling one super on the same interval arrive together, which
 * is the load pattern this file exists to avoid. @p rand is any value; only
 * its low bits are used, so a caller can pass a tick count.
 */
uint32_t xcadence_jitter(uint32_t seconds, uint32_t rand);

#ifdef __cplusplus
}
#endif
