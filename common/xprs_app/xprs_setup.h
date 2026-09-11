/**
 * @file xprs_setup.h
 * @brief The words of XPRS.md 11.10, setting a station up, checked.
 *
 * Pure: no radio, no config, no clock, so the host test exercises exactly
 * what the station runs. xprs_app.c decides who may say these things (an
 * owner, verified, not replayed) and what to do about them; this file only
 * decides whether what was said is well formed.
 *
 * The keys, and where they may travel:
 *
 *   ssid   the network to join          sealed only
 *   pass   its password                 sealed only
 *   nsec   a key to take instead        sealed only
 *   wifi   join | off                   either
 *   nick   what the station is called   either
 *   zone   auto | an offset             either
 *   ap     on | off                     either
 *   key    new                          either
 *
 * 11.10 says pass and nsec only ever travel sealed; ssid travels with its
 * password, so it is sealed here too, and a clear one is refused the same
 * way. A sealed body is `key:value` lines, the first `cmd:set`, a value
 * running to the end of its line (11.4), which is how a network name keeps
 * its spaces.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Setup fields one command may carry. The widest real one is five. */
#define XSETUP_KV_MAX   8
/** The longest value: a WPA2 passphrase or an nsec, 63, and the NUL. */
#define XSETUP_VAL_MAX  66

typedef struct {
    char key[8];
    char val[XSETUP_VAL_MAX];
} xsetup_kv_t;

/** One of the eight keys above? */
bool xsetup_is_key(const char *key);

/** A key that may only arrive inside `x:`. */
bool xsetup_is_secret(const char *key);

/**
 * The lines of a sealed body. Fills @p kv with every `key:value` after the
 * leading `cmd:set` line (a CR before a newline is tolerated).
 *
 * @return how many fields, or -1 when the body is not a `cmd:set`, a line is
 *         not `key:value`, a value is too long, or there are more than
 *         @p max. 11.4: anything that does not begin with `cmd:` is a 400.
 */
int xsetup_lines(const char *plain, xsetup_kv_t *kv, int max);

/**
 * Is @p val an acceptable value for @p key?
 * @return NULL when it is, else the reason, short enough for an `m:`.
 */
const char *xsetup_check(const char *key, const char *val);

#ifdef __cplusplus
}
#endif
