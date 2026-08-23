/**
 * @file xprs_health.h
 * @brief One definition of "this station is working", and something that
 *        says so out loud.
 *
 * Nothing on this chip fails loudly. The station has now been taken off
 * the air four separate times by something that returned an error nobody
 * read, and in every case it kept running and kept looking healthy:
 *
 *   - xTaskCreate() could not get an 8 KB stack, so relay_task never ran:
 *     no beacons, no service announcements, no clock.
 *   - httpd_start() could not get 5,120 bytes in one piece, so the dongle
 *     served nothing on the LAN while gossiping happily over ESP-NOW and
 *     BLE. From the air it looked perfect.
 *   - nimble_port_freertos_init() could not get 5,120 either, so the BLE
 *     host never started, so relay_task -- which waits on on_sync() --
 *     stayed parked. It carries the firmware self-test and the answer to
 *     an over-the-air cmd:update.
 *   - A whole block of sdkconfig went missing in a repo move. No test
 *     covers a Kconfig value.
 *
 * The evidence for the third one had been printing in the heartbeat for a
 * long time -- `relay=0` -- and nothing read it. A number in a log line is
 * not an alarm. So this keeps a small register of what the board is
 * supposed to have, and shouts the names of whatever is missing, at the
 * end of boot AND periodically afterwards, because a subsystem that dies
 * at hour six deserves the same noise as one that never started.
 *
 * It also owns the verdict the OTA self-test consumes (XPRS.md 25.8), so
 * "healthy enough to keep this firmware" and "healthy enough to stop
 * complaining" cannot drift apart: they are the same function.
 *
 * Deliberately allocation-free. A health check that needs the heap is
 * useless on the day the heap is the problem -- names are borrowed, not
 * copied, so pass string literals.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Plenty for both boards; a station with more parts than this wants a
 *  different design, not a bigger array. */
#define XH_MAX 12

/**
 * Declare that this build is supposed to have [name], BEFORE starting it.
 *
 * Registering up front is the point: a subsystem that is never started at
 * all is exactly the failure this exists to catch, and one that only
 * appears in the register when it succeeds can never be reported missing.
 *
 * [required] false records something worth printing but not worth calling
 * the station broken over (an SD card that is simply absent).
 * [name] must outlive the call -- use a literal.
 */
void xh_expect(const char *name, bool required);

/** Report [name] up (true) or down (false). Safe to call repeatedly, and
 *  safe to call from a poll: this is how a subsystem that dies later is
 *  noticed. Unknown names are ignored rather than fatal. */
void xh_set(const char *name, bool up);

/** The roster as two words: bit i is the i-th xh_expect() in registration
 *  order, so a board's boot log is the decoder. For the air, where a
 *  sixty-byte health line does not fit. */
void xh_masks(uint16_t *up, uint16_t *required);

/**
 * Log what is missing. One ESP_LOGE per absent required subsystem, naming
 * it, then a single summary line either way.
 *
 * Call at the end of boot and from the heartbeat. It only shouts when the
 * picture CHANGES or when [force] is true, so a station that is quietly
 * fine does not fill its own log, and one that breaks says so once,
 * loudly, at the moment it breaks.
 */
void xh_report(bool force);

/** True when every required subsystem is up. This is the OTA self-test's
 *  verdict as well as the log's -- see the file comment. */
bool xh_all_ok(void);

/**
 * Complain when free heap has fallen below what this board is documented
 * to run at.
 *
 * docs/esp32.md carries measured heap tables, and the day one of them
 * silently stopped being true -- an sdkconfig block lost in a repo move --
 * the board ran for months on the margin it used to have and then died
 * from 2,912 bytes of new static RAM. A table nobody re-reads is not a
 * check. Pass the board's documented floor; this turns the table into one.
 *
 * Call it at the end of boot AND from the heartbeat. The periodic call is
 * not decoration: a board can start every subsystem cleanly and then run
 * out, and when it does the parts do not vanish, they stop WORKING -- the
 * HTTP handle stays valid while every handler's response malloc fails,
 * which is a station that looks present and answers nothing. Free heap is
 * the honest signal for that. Edge-triggered, so it speaks when the answer
 * changes rather than every beat.
 */
void xh_heap_floor(unsigned expected_free);

#ifdef __cplusplus
}
#endif
