/*
 * What every XPRS station on an nRF52840 needs before it can say anything:
 * a key and the callsign it derives to, a boot ordinal, a config file, a
 * clock, and signing. Shared by models/sensecap-p1-pro and
 * models/sensecap-t1000e (Adafruit nRF52 Arduino core); the radios, the loop
 * and the power policy stay with each board.
 *
 * FLASH ORDER (FIRMWARE.md section 7). nst_init() and every write here go
 * through the core's LittleFS, which deadlocks once the SoftDevice is up: the
 * flash layer then blocks on a SoC event only the station task can pump.
 * So nst_init() runs BEFORE the SoftDevice, and the two later writes (a key
 * import, a config change) take the SoftDevice down, write, and reboot.
 */
#ifndef XPRS_NRF_STATION_H
#define XPRS_NRF_STATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The callsign prefix (XPRS 3): "X3" for a fixed station, "X2" for a
     * movable one. Four characters of the npub follow it. */
    const char *prefix;
    /* A volume that will not take a write (another firmware's files filling
     * it) is formatted, loudly, when this is set. Off, the station runs
     * unsigned and says so. */
    bool format_if_full;
} nst_cfg_t;

void        nst_init(const nst_cfg_t *cfg);

const char *nst_call(void);           /* stable pointer, valid after init */
const char *nst_npub(void);
bool        nst_have_key(void);
uint32_t    nst_boot_epoch(void);
/* The private scalar, for the console's backup command only. */
const uint8_t *nst_priv(void);

/* sig: on our own packet (6.1); unchanged when there is no key. */
int  nst_sign(char *wire, int len, int cap);

/* Adopt another key from an nsec, write it, reboot. Does not return on
 * success. */
void nst_key_import(const char *nsec);

/* key=value lines in /xprs/cfg, read by xcfg_get(). */
void nst_cfg_load(void);
/* The value of [key] in /xprs/cfg, or [def]. The same name the ESP32 boards
 * read their NVS config by, so shared components call it unchanged. */
const char *xcfg_get(const char *key, const char *def);
/* "set <k> <v>" (writes and reboots), "get <k>", "list". */
void nst_cfg_console(char *line);

/* The clock: epoch seconds, or 0 for none. Learned from a signed owner
 * command (nst_clock_learn), or set by a better source (GNSS, a phone):
 * nst_clock_set(). Monotonic forward either way, so a replayed command
 * cannot rewind it and then pass its own freshness check. */
uint32_t nst_now(void);
uint32_t nst_ts_to_epoch(const char *ts);
void     nst_clock_learn(const xprs_t *p);
bool     nst_clock_set(uint32_t epoch, const char *source);
/* The neighbours' hint (XPRS.md 11.10), for a station with no network and
 * no owner in range: OFF unless the owner sets `clock_hint 1`, and only
 * while there is no clock at all. Call it with every packet heard; it
 * learns keys from t:identity packets that verify under the key they carry
 * (and whose callsign derives from it), verifies the ts: of direct
 * results, commands and receipts -- what a station says about now --
 * against those keys, and sets the clock once three agree
 * (xauth_hint_note). */
/* True when the caller should send `t:request d:<ask> q:identity` (XPRS.md
 * 8, 29.1; [ask] 16 bytes) to a station heard directly: the identity it
 * signs in answer is both its key and a reading of now (counted when it
 * arrives within 30 s of the ask). Waiting for traffic that happens to say
 * the time took 5 to 10+ minutes on the bench. Rate-limited here: one
 * author per ten minutes, one ask per 20 s, and only while there is no
 * clock and the owner chose the hint. */
bool     nst_clock_hint(const xprs_t *p, char ask[16]);
/* For a status line: keys learned, samples held (of XAUTH_HINT_N). */
void     nst_hint_status(int *keys, int *samples);
/* ts: under a known clock, epoch:<boots>.<uptime> otherwise (15.7). */
int      nst_time_field(char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_NRF_STATION_H */
