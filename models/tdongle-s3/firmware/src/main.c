/*
 * An XPRS station on a LilyGO T-Dongle-S3.
 *
 * The station is common/xprs_app -- the same program the T-Deck and the
 * M5Stack run. What is left in this file is the board: a 160x80 ST7735 on
 * the pins in xprs_model_tdongle_s3, and the BOOT strap pin, which is the
 * only button there is.
 *
 * WHAT THIS BOARD IS FOR. An ESP32-S3, so it has BLE5 extended advertising
 * and can carry an XPRS packet on the air (docs/esp32.md, "Radio capability
 * per chip"); a USB stick, so it lives plugged into something beside a
 * router. That is the two-sided job it does and the reason it exists in the
 * fleet: it BRIDGES -- what it hears on Bluetooth it repeats on the LAN and
 * on ESP-NOW, and what it hears on the LAN it repeats on Bluetooth -- and it
 * DIGIPEATS on each of those, repeating what it heard on the medium it heard
 * it on, appending itself to `via:` within the hop budget (XPRS 13.1). None
 * of that is written here: every bearer does it, and xprs_app wires them
 * together the same way on every board.
 *
 * WHAT IT NO LONGER DOES. This file used to be five thousand lines and its
 * own station: its own index, gossip, cadence, catch-up, updater, HTTP API,
 * digipeat rules and identity, none of which the other boards could use and
 * all of which had to be fixed twice. Beside them sat an APRS-IS iGate, a
 * BLE street mesh with a GATT service, and a Reticulum TCP uplink -- three
 * jobs that had nothing to do with being a bridge and that between them did
 * not fit: the notes that went with them recorded a board about 12 KB short
 * of holding everything it started, failing silently at whatever it started
 * last. They are gone from here. `git log` has them if they are wanted back,
 * and the place for them is a shared component, not this file.
 */

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "model_init.h"
#include "st7735.h"
#include "xprs_app.h"

#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */
#include "fw_secrets.h"     /* gitignored; see fw_secrets.h.example   */

static const char *TAG = "tdongle";

/* ── The screen ─────────────────────────────────────────────────────────── */

static esp_err_t display_init(int *w, int *h, void **ctx)
{
    /* NVS and the ST7735, on the pins in common/xprs_model_tdongle_s3.
     * model_init() opens NVS as well, which xapp_run() has already done --
     * it is idempotent, and doing it here keeps this board's init in the
     * one place every other user of the model component finds it. */
    esp_err_t err = model_init();
    if (err != ESP_OK) return err;

    st7735_handle_t lcd = model_get_lcd();
    if (!lcd) return ESP_ERR_NOT_FOUND;   /* init logged why */

    *w = ST7735_WIDTH;
    *h = ST7735_HEIGHT;
    *ctx = lcd;
    return ESP_OK;
}

static void display_flush(int x1, int y1, int x2, int y2,
                          const uint16_t *px, void *ctx)
{
    st7735_flush((st7735_handle_t)ctx, x1, y1, x2, y2, px);
}

static void screen_power(bool on)
{
    st7735_handle_t lcd = model_get_lcd();
    if (lcd) st7735_backlight(lcd, on);
}

/* ── The one button ──────────────────────────────────────────────────────
 *
 * There is no BTN_* pin in the model component because the board has no
 * button of its own: what a finger can reach is the BOOT strap pin, GPIO0,
 * active low, and in most mountings it is half under the case. That is why
 * this board asks xprs_app for the hands-off tour at boot (see .rotate
 * below) -- the screen has to be worth looking at without being touched.
 *
 * Three gestures, and the two long ones are one movement:
 *
 *   tap                   NEXT -- step to the next panel. The first tap
 *                         also stops the tour, so a tap is how you hold a
 *                         panel still to read it.
 *   hold (0.7 s)          HOME -- back to the radar.
 *   keep holding (2 s)    HOME, then DOWN, which on the home panel is what
 *                         starts the tour. So "hold until it moves" is
 *                         resume, and there is no gesture to remember.
 */
#define BTN_GPIO        GPIO_NUM_0
#define BTN_DEBOUNCE_MS 30
#define BTN_HOME_MS     700
#define BTN_TOUR_MS     2000

static void input_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    ESP_LOGI(TAG, "BOOT button ready on GPIO%d", (int)BTN_GPIO);
}

/* Called every 10 ms by the UI task. */
static xapp_key_t input_poll(void)
{
    static int  held_ms;
    static bool home_fired, tour_fired;

    if (gpio_get_level(BTN_GPIO) == 0) {
        held_ms += 10;
        if (held_ms >= BTN_TOUR_MS && !tour_fired) {
            tour_fired = true;
            ESP_LOGI(TAG, "button: hold -> tour");
            return XAPP_KEY_DOWN;
        }
        if (held_ms >= BTN_HOME_MS && !home_fired) {
            home_fired = true;
            ESP_LOGI(TAG, "button: hold -> home");
            return XAPP_KEY_HOME;
        }
        return XAPP_KEY_NONE;
    }

    bool tap = held_ms >= BTN_DEBOUNCE_MS && !home_fired;
    held_ms = 0;
    home_fired = tour_fired = false;
    if (tap) {
        ESP_LOGI(TAG, "button: tap");
        return XAPP_KEY_NEXT;
    }
    return XAPP_KEY_NONE;
}

/* ── The board ──────────────────────────────────────────────────────────── */

static const xapp_board_t k_board = {
    .board_id = "tdongle-s3",
    .banner   = "BLE5 bridge and digipeater, 160x80, no PSRAM",

    .fw_key     = FW_DEFAULT_KEY,
    .fw_owner   = FW_DEFAULT_OWNER,
    .script_key = NULL,        /* falls back to fw_key; no panels shipped */

    .wifi_ssid = WIFI_SSID,
    .wifi_pass = WIFI_PASS,
    .espnow_channel = ESPNOW_FALLBACK_CHANNEL,

    .display_init = display_init,
    .flush        = display_flush,
    .screen_power = screen_power,

    .input_init = input_init,
    .input_poll = input_poll,

    /* No keyboard, so the chat panel stays the read-only table; no touch
     * panel, so the bottom bar keeps its legends; no battery, so the screen
     * is never blanked for idleness (xprs_app only blanks on a discharging
     * cell); no LoRa radio. */
    .raw_key     = NULL,
    .touch_read  = NULL,
    .kb_backlight = NULL,
    .battery_mv  = NULL,
    .lora        = NULL,

    /* An S3: extended advertising, so this bearer is real here. It is the
     * whole reason this board is in the fleet. */
    .ble = true,

    /* The button is barely reachable; the tour is the interface. */
    .rotate = true,

    /* NO walk-up hotspot, and this is the one field on this board that was
     * decided by measurement rather than by what the hardware is. The AP is
     * for a board somebody approaches with a phone and no shared network;
     * this one lives plugged in beside a router, which already has one. It
     * costs about 9 KB of internal heap: with the AP up this station ran at
     * 12 KB free and touched 96 BYTES at its worst moment, and with it down
     * at 21 KB free. Seeded as a default, so `cfg set ap_on 1` still turns
     * it on for anyone who wants it and knows what it costs. */
    .hotspot = false,

    /* This board's own tripwire, not the fleet default. Measured on the
     * firmware this one replaces (docs/esp32.md): the end of boot sat around
     * 5 KB with the bearers just started and settled near 14 KB once
     * association finished, so 4,000 is below the transient and above the
     * failure. It has no PSRAM, so this number is the whole of its room and
     * the one that catches a setting that stopped being applied. */
    .heap_floor = 4000,
};

void app_main(void)
{
    xapp_run(&k_board);
}
