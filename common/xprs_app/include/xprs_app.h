/**
 * @file xprs_app.h
 * @brief The XPRS station, minus the board it runs on.
 *
 * Everything a station does -- the bearers, the indexer, the eight-panel
 * dashboard, the HTTP API, the walk-up hotspot -- is the same program on
 * every board. What differs is a screen, a handful of pins, and how a
 * person presses something. This component is the program; the board
 * supplies the difference through one struct and calls xapp_run().
 *
 * It began as models/m5stack-core/firmware/src/main.c and was lifted here
 * unchanged when the T-Deck needed the same station, so that a fix to a
 * panel or a bearer is made once rather than found twice.
 *
 * THE INPUT CONTRACT IS SEMANTIC, NOT PHYSICAL. A board reports what the
 * user MEANT -- next, home, up, down -- not which pin went low. The
 * M5Stack has three buttons and the T-Deck has a trackball; "a short press
 * is OK while the Settings list has focus" is a property of the menu, not
 * of the hardware, so it lives in here.
 */
#ifndef XPRS_APP_H
#define XPRS_APP_H

#include <stdint.h>
#include "esp_err.h"
#include <stdbool.h>
#include "xprslora.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One tick of intent from the board. Polled at 100 Hz; the board is
 *  responsible for debouncing and for firing once per gesture. */
typedef enum {
    XAPP_KEY_NONE = 0,
    XAPP_KEY_NEXT,      /**< next panel, or OK on a focused Settings row */
    XAPP_KEY_PREV,      /**< previous panel (a board with no such control
                             never sends it; the M5Stack does not) */
    XAPP_KEY_HOME,      /**< escape: back to the home panel */
    XAPP_KEY_UP,        /**< selection up */
    XAPP_KEY_DOWN,      /**< selection down, and the rotate tour on home */
} xapp_key_t;

/** What one board is. Every field is required except the input hooks --
 *  a board with no controls at all still runs, driven by the serial
 *  console keys the UI task already reads. */
typedef struct {
    const char *board_id;      /**< "m5stack-core" -- reported by /api/status */
    const char *banner;        /**< one line for the boot log, board's own words */

    /** Which key may approve firmware for this board (64 hex, x-only) and
     *  which npub may command it. Defaults only: written into config once
     *  on a board that has none, and the operator's to change afterwards.
     *  NULL or empty means the station installs nothing / obeys nobody,
     *  which is the right answer for a board nobody has configured. */
    const char *fw_key;
    const char *fw_owner;
    /** Publisher key for signed SCRIPT bundles, x-only hex, or NULL.
     *
     * Separate from fw_key on purpose: it lets an operator delegate "may
     * publish panels for this station" without also delegating "may reflash
     * it". Unset falls back to fw_key, and with neither set nothing verifies
     * and no script runs -- a station that has not been told whom to trust
     * runs nobody's code. Seeded into NVS once, like the two above. */
    const char *script_key;

    const char *wifi_ssid;     /**< compiled-in default; config.ini overrides */
    const char *wifi_pass;
    int espnow_channel;        /**< used only when the SSID is empty */

    /** Bring the panel up. Returns its size and an opaque handle that is
     *  handed back to flush(). */
    esp_err_t (*display_init)(int *width, int *height, void **ctx);
    /** Push RGB565 pixels. The signature is xui_flush_fn. */
    void (*flush)(int x1, int y1, int x2, int y2,
                  const uint16_t *px, void *ctx);

    void (*input_init)(void);          /**< optional */
    xapp_key_t (*input_poll)(void);    /**< optional; called every 10 ms */
    /**
     * A physical keyboard, if the board has one. Returns the key as a RAW
     * byte -- lower case left alone, 0x08 backspace, 0x0d enter -- or 0
     * when nothing is pending.
     *
     * Raw, because the same key means two things. Where the console
     * commands are concerned 's' and 'S' are one key and this component
     * folds the case itself; where a person is typing a message they are
     * emphatically not, and a board that folded case first would have
     * thrown away the difference before anybody could ask.
     *
     * Offering this is also what says the board can be TYPED ON, so it is
     * what earns the interactive chat panel in place of the read-only
     * table. A board with buttons and no keyboard leaves it NULL.
     */
    int (*raw_key)(void);

    /**
     * Touch, if the board has a panel over its screen. Fills SCREEN
     * coordinates -- already rotated and flipped to match what the display
     * shows -- and returns true while a finger is down. Polled by the UI.
     *
     * NULL says "no touch", and that is a statement about the BAR too: a
     * board with buttons keeps the button legends; one with a panel gets
     * tap targets instead. Same idiom as raw_key: offering the hook is what
     * earns the behaviour.
     */
    bool (*touch_read)(int *x, int *y);

    /** Keyboard backlight, if the keyboard has one. The app lights it on
     *  every keypress and puts it out after a few idle seconds. NULL: none. */
    void (*kb_backlight)(bool on);

    /** Battery in millivolts, or -1 when the board cannot say. Read by
     *  the UI task every ten seconds; cheap enough for that, no more. */
    int (*battery_mv)(void);

    /** Screen power. false puts the panel to sleep and the backlight out;
     *  true brings both back. NULL: the screen is never blanked. */
    void (*screen_power)(bool on);

    /**
     * Start with the hands-off tour running: home, stats, chat, every 30 s,
     * until somebody presses something.
     *
     * The default (false) suits a board you stand in front of -- it shows
     * the dashboard and waits. It does not suit a board whose only control
     * is the BOOT strap pin, half under the case, which is the T-Dongle:
     * there the tour IS the interface, and a screen that needs a press to
     * show its second page is a screen with one page.
     */
    bool rotate;

    /**
     * The least free heap this board is documented to boot with, in bytes,
     * or 0 to take the shared default (6,000 -- the M5Stack's figure).
     *
     * It is not a budget, it is a tripwire: the station complains below it,
     * so a setting that quietly stopped being applied shows up as a step
     * change rather than as a board that dies next Tuesday. Set it under
     * the boot transient and over the failure, from what THIS board
     * actually measures (docs/esp32.md keeps the table), and raise it when
     * the board genuinely gets roomier.
     */
    int heap_floor;

    /**
     * Does this board run the walk-up hotspot -- its own access point, with
     * the chat page and the API on it -- by default?
     *
     * It is for a board somebody walks up to with a phone and no shared
     * network. A board that lives plugged in beside a router already has
     * one, and the AP costs it about 9 KB of internal heap, measured here:
     * the T-Dongle ran at 12 KB free with a worst case of 96 BYTES with the
     * AP up, and at 21 KB free with it down. On a board with PSRAM that is
     * a rounding error and the hotspot is worth having; on one without it
     * is the difference between a station and a station that is one
     * allocation from failing.
     *
     * Seeded into config once, like the keys above, so it is a DEFAULT and
     * not a verdict: `cfg set ap_on 1` turns it on afterwards and the
     * setting survives the next update.
     */
    bool hotspot;

    /**
     * The board's LoRa radio, or NULL for a board without one. The station
     * brings it up as a third bearer beside ESP-NOW and the LAN: same wire,
     * same relay rules, real range.
     */
    const xprslora_cfg_t *lora;
    /**
     * True on a board whose radio does BLE5 extended advertising, which is the
     * S3/C3 class and not the original ESP32 (docs/esp32.md, "Radio capability
     * per chip"). An XPRS beacon is 112-173 bytes against legacy advertising's
     * 31, so a board without it cannot carry this bearer at all -- leaving the
     * flag false is how that is said, and the M5Stack says it.
     */
    bool ble;
} xapp_board_t;

/** Run the station. Does not return: it is the body of app_main().
 *  The pointer must stay valid forever (a static is the expected form). */
void xapp_run(const xapp_board_t *board);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_APP_H */
