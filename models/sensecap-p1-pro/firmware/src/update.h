/*
 * Installing firmware on a pole (XPRS.md 25.8), on a chip with no HTTP.
 *
 * The ESP32 boards take an image over HTTP (common/xprs_ota); this board has
 * no WiFi, so the image arrives the way everything else does here -- as XPRS
 * packets on LoRa or BLE, through whatever station is in range -- and the
 * rules are the same ones:
 *
 *   - an owner opens the door with a signed `cmd:update` (xprs_auth: on the
 *     allow-list, fresh, direct, not a repeat) naming the version, size and
 *     sha256 of what is coming;
 *   - the image itself comes unsigned, 160 bytes a packet (`cmd:zfw n:<i>
 *     m:<base85>`), into a staging area of flash; a stranger can waste
 *     airtime here and nothing else, because
 *   - nothing is installed until the whole image hashes to what the owner
 *     named AND the publisher's approval (`cmd:zfwsig`, a signature over
 *     "xprsfw1 <board> <version> <size> <sha256>" by the pinned fwkey)
 *     verifies -- the same line, the same key, the same check as xprs_ota;
 *   - the running image is copied aside before the new one goes in, and a
 *     new image that has not proved itself within three boots is put back.
 *
 * `cmd:zfwq` asks which packets are still missing, so a pusher resends
 * exactly those; `cmd:zdiag` answers with what a person would climb up to
 * read.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

extern "C" {
#include "xprs.h"
#include "xprsbearer.h"
}

typedef struct {
    const char *board;                       /* "sensecap-p1-pro", as in the approval line */
    const char *call;                        /* this station's callsign */
    int (*sign)(char *wire, int len, int cap);  /* sig: on our answers */
    void (*flush)(uint32_t ms);              /* tick the bearers for this long before a reset */
} xfw_cfg_t;

/* Before the SoftDevice: reads the probation file, and either clears it
 * (last boot proved itself), restores the previous image (three boots and
 * nothing proved) or leaves it for this boot to prove. */
void xfw_init(const xfw_cfg_t *cfg, uint32_t boot_epoch);

/* A t:command addressed to us, heard directly on [b]. True when consumed. */
bool xfw_handle(xb_t *b, const xprs_t *p);

/* From the station loop: probation proving, session expiry. */
void xfw_tick(uint32_t now_ms, bool radio_up);

const char *xfw_version(void);
bool        xfw_probation(void);
/* For the status line: chunks received / expected, 0/0 when idle. */
void        xfw_progress(uint32_t *got, uint32_t *of);
