/*
 * xprs_diag -- a station's diagnostics, askable over the air.
 *
 * A roof board is usually kilometres away on ESP-NOW or LoRa, and the
 * questions a person would climb up to answer -- is it well, what did it
 * die of, what did it say before it died -- are all already in its RAM:
 * /api/diag, the log, the coredump summary, the health roster. This
 * component puts them behind three signed, owner-gated XPRS commands and
 * a few bytes on a beacon that already exists, so the radio that carries
 * the traffic also carries the post-mortem, without adding any traffic of
 * its own.
 *
 *   cmd:zdiag         one frame: fw, uptime, heap, reset reason, health,
 *                     bearer counters, last crash
 *   cmd:zcore         two frames: the coredump summary -- crashing task and
 *                     backtrace PCs, to addr2line on the ground
 *   cmd:zlog          the log, paged like cmd:history (25.2.1): 202, then
 *                     lines newest first as code:206 frames, then 200/206;
 *                     since:/until: narrow the window, zq: filters, zl:last
 *                     reads the words that survived the last crash
 *
 * Every ask goes through xauth_check (25.4): direct, signed, on the allow
 * list, inside its 300 s window, idempotent. Pages share the history budget
 * (31.2) and are paced per bearer, so a LoRa node cannot be asked into
 * breaching its duty cycle. The z prefix keeps all of it private (8, 34)
 * until the packets have been shown and agreed.
 *
 * Shared by every board: the app (xprs_app) and the dongle both register
 * it with a handful of callbacks and call the pump from a core-1 task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *callsign;                                   /* borrowed */
    /** Sign a wire in place (xprsid_sign wrapper); returns the new length. */
    int  (*sign)(char *wire, int len, int cap);
    /** Air one wire on the named bearer: "espnow" "lora" "ble" "lan". */
    void (*air)(const char *bearer, const char *wire, int len);
    /** rx, tx, cancel, drop, issued, done, fail, peers -- or leave zero. */
    void (*stats)(uint32_t out[8]);
    /** The rotating log files, newest first; NULL when the board keeps no
     *  log on flash -- then this component keeps a short RAM tail itself. */
    const char *log_cur, *log_prev;
    /** Wall clock in epoch seconds, 0 when unsynced. May be NULL. */
    uint32_t (*epoch_now)(void);
    /** The airtime budget (31.2), shared with cmd:history. NULL = unmetered. */
    bool (*budget)(const char *from, uint32_t now_s);
    void (*budget_record)(const char *from, uint32_t now_s);
    /** Hook esp_log ourselves (boards without their own vprintf hook). */
    bool hook_log;
} xdiag_cfg_t;

/** Reads the reset reason, validates the last-words ring, installs the
 *  log hook when asked. Call once, after xcfg_init(), before the bearers. */
void xdiag_init(const xdiag_cfg_t *cfg);

/** Receive-task side: is this a z-command addressed to us? Then copy it
 *  into the one slot. Parse and memcpy only -- the signature is checked
 *  on the pump's task. Returns true when the packet was taken. */
bool xdiag_park(const char *wire, int len, const char *bearer);
/** Same, for a caller that has already parsed the wire: the receive tasks
 *  are small and a second xprs_t on their stack is not free. */
bool xdiag_park_parsed(const xprs_t *p, const char *wire, int len,
                       const char *bearer);

/** Core-1 task, once per tick (250..1500 ms): verifies a parked ask, airs
 *  at most one frame when one is due. Never blocks. */
void xdiag_pump(uint32_t now_ms);

/** Appends " uptime:6h zh:3f/3f[ zc:...]" to a beacon; returns bytes
 *  written, 0 when there was no room. The leading space is included. */
int  xdiag_beacon_fields(char *buf, int cap);

/** Feed from the board's own log hook: one formatted line, any task. */
void xdiag_log_line(const char *line, int n);

/** True when this boot followed a panic or watchdog and the words before
 *  it were recovered from RTC memory. */
bool xdiag_last_words_valid(void);

/** Console test hooks, only in a build with -DXDIAG_TEST_HOOKS:
 *  "cfg zpanic" aborts, "cfg zhang" spins with interrupts off (an
 *  interrupt-watchdog crash, the shape the T-Decks died in). Returns true
 *  when the line was one of these. Always false otherwise. */
bool xdiag_console(const char *line);

#ifdef __cplusplus
}
#endif
