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
} xapp_board_t;

/** Run the station. Does not return: it is the body of app_main().
 *  The pointer must stay valid forever (a static is the expected form). */
void xapp_run(const xapp_board_t *board);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_APP_H */
