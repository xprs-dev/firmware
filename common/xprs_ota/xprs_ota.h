/**
 * @file xprs_ota.h
 * @brief Installing firmware without a ladder (XPRS.md 25.8).
 *
 * A station on a roof updates over the air or stays on the version it was
 * carried up with. This is that path, and its whole design is one
 * sentence: **the station accepts an image, never a source.**
 *
 * What that means concretely:
 *
 *   - The bytes are authenticated by a signature made with the key whose
 *     holder may publish firmware for this station, and that signature is
 *     checked BEFORE a single byte reaches flash. A poisoned mirror, a
 *     lying DNS answer or a stranger's web server can waste this
 *     station's airtime and cannot change what it runs.
 *   - So the transport needs no trust of its own. Plain HTTP is
 *     deliberate: TLS would only prove which server answered, and it
 *     wants 20-40 KB of peak heap that the dongle does not have.
 *   - What is signed is not the bare image digest. The same key signs
 *     this station's packets, and a signature over 32 anonymous bytes
 *     says nothing about which 32 bytes were meant. The approval covers
 *     a line no packet can produce (a packet always begins `t:`):
 *
 *         xprsfw1 <board> <version> <size> <sha256 as 64 lowercase hex>
 *
 *     which binds board, version, size and content together -- so a
 *     dongle build cannot install on an m5stack, and last version's
 *     approval cannot be replayed onto the next one.
 *   - The old firmware stays in its slot until the new one has proved it
 *     works. If the new image cannot come up, or cannot say it is well
 *     within two minutes, the bootloader puts the old one back and the
 *     old one reports the failure. Nobody goes up the ladder.
 *
 * NOT DEFENDED, on purpose: anybody holding the board and a USB cable can
 * write anything to it. That is the owner's right, and it is why the
 * pinned key and the allow-list are re-writable with that cable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Result codes, chosen to be the XPRS `code:` the station answers with. */
typedef enum {
    XOTA_ACCEPTED   = 202,  /**< work started; the real answer comes later */
    XOTA_REFUSED    = 403,  /**< bad signature, wrong board, or no key pinned */
    XOTA_BUSY       = 429,  /**< already installing, or a verify is pending */
    XOTA_FAILED     = 500,  /**< transfer died, or the bytes did not match */
    XOTA_UPTODATE   = 200   /**< already running that version */
} xota_code_t;

typedef struct {
    /** Board id as it appears in the signed line, e.g. "m5stack-core". */
    const char *board;
    /** This station's callsign, for the answers it airs. */
    const char *callsign;
    /**
     * Air one wire on the bearer named. Supplied by the board because
     * only it knows its radios. NULL disables the over-the-air answers.
     */
    void (*air)(const char *bearer, const char *wire, int len);
    /** Called before flash work starts / after it ends: quiesce storage. */
    void (*quiesce)(bool quiet);
} xota_cfg_t;

/**
 * Start the updater. Claims NO task and NO stack.
 *
 * The first cut created its own 8 KB worker "beside the other big stacks",
 * per the usual advice -- and on the m5stack, which lives at about 8 KB
 * free with a full archive, that 8 KB was the whole margin: the LAN bearer
 * started failing sendto with ENOMEM and the index could not open its
 * files. The advice is right for a task the station always needs; it is
 * wrong for one that runs for three minutes a month.
 *
 * So the install runs on the caller's storage task -- which already has a
 * big stack, already lives on core 1, and already owns the flash the
 * install is about to pause. Call xota_poll() from it.
 */
esp_err_t xota_start(const xota_cfg_t *cfg);

/**
 * Do any pending install work. Call from the station's storage task (the
 * one that may block for minutes); returns immediately when idle.
 */
void xota_poll(void);

/**
 * Ask for an update from the configured source (or [url] when non-NULL,
 * which only an authorised commander may supply). [version] may be NULL
 * for "whatever the channel offers". [reply_to]/[bearer]/[cmd_id] are
 * remembered so the outcome can be aired later -- across the reboot.
 *
 * Returns immediately: this is the 202. The 200 or 500 comes minutes
 * later, from whichever firmware is running by then.
 */
xota_code_t xota_request(const char *version, const char *url,
                         const char *reply_to, const char *bearer,
                         const char *cmd_id);

/**
 * Install an image the caller already holds (the HTTP push door). [sig] is
 * the 60-character base85 signature over the xprsfw1 line for these bytes.
 * Feed it in chunks; a NULL [data] with [len] 0 finishes.
 */
esp_err_t xota_push_begin(const char *version, size_t size, const char *sig);
esp_err_t xota_push_write(const void *data, size_t len);
esp_err_t xota_push_finish(void);   /**< verifies, then reboots on success */
void      xota_push_abort(void);

/** Progress for the screen and for /api/diag: -1 idle, else 0..100. */
int  xota_progress(void);
/** True while an install is under way (so other work can stand aside). */
bool xota_busy(void);

/**
 * Call once the station is demonstrably healthy. Marks the running image
 * valid and cancels the rollback; also airs the parked `code:200` for the
 * command that asked for this version. Safe to call repeatedly.
 */
void xota_mark_healthy(void);

/**
 * The other half of the self-test: this image did NOT come up healthy.
 * When it is still on probation the bootloader is asked to put the
 * previous one back, immediately, rather than waiting for a watchdog.
 * Does nothing when the running image is already valid -- a station that
 * was merely having a bad afternoon must not reboot itself in a loop.
 */
void xota_mark_unhealthy(void);

/**
 * Call early in boot. If the previous boot was an install that the
 * bootloader rolled back, this airs the parked `code:500 fw:<running>`
 * -- the failed update reporting its own failure, with nobody on the
 * roof. Returns true when it found and cleared such a record.
 */
bool xota_report_rollback(void);

/** The running firmware version, from the image header. Never NULL. */
const char *xota_version(void);

#ifdef __cplusplus
}
#endif
