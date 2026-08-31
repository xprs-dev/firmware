/*
 * An XPRS station on a Heltec WiFi LoRa 32 V3.
 *
 * The station is common/xprs_app -- the same program the T-Deck, the
 * M5Stack and the T-Dongle run. What is left in this file is the board: a
 * 128x64 SSD1306 over I2C on the pins in xprs_model_heltec_v3, the SX1262
 * handed to the LoRa bearer as a pin table, the battery divider, and the
 * PRG button, which is the only one there is.
 *
 * WHAT THIS BOARD IS FOR. An ESP32-S3, so BLE5 extended advertising and an
 * XPRS packet fit one advert (docs/esp32.md, "Radio capability per chip");
 * an SX1262 on 868 MHz, so LoRa range; WiFi, so the LAN and ESP-NOW. That is
 * the T-Deck's radio set in a board a third of the price with no keyboard,
 * which makes it the natural digipeater to leave on a shelf: it carries all
 * four bearers and needs nobody to look at it.
 *
 * THE SCREEN IS MONOCHROME AND THE UI IS NOT. xprs_ui_mini draws RGB565 for
 * the T-Dongle's 160x80 strip; the SSD1306 holds one bit per pixel. The
 * flush below is the whole of the bridge: threshold each pixel's luminance,
 * set it in the panel's page buffer, and push the buffer once the last
 * strip of the frame has arrived. No dithering -- at 10 px type on 64 rows
 * there is nothing a dither would improve and everything it would smear.
 */

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "model_init.h"
#include "model_config.h"
#include "ssd1306.h"
#include "xprs_app.h"

#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */
#include "fw_secrets.h"     /* gitignored; see fw_secrets.h.example   */

static const char *TAG = "heltec";

/* ── The screen ─────────────────────────────────────────────────────────── */

static esp_err_t display_init(int *w, int *h, void **ctx)
{
    /* NVS, Vext, the I2C bus and the SSD1306, on the pins in
     * common/xprs_model_heltec_v3. model_init() opens NVS as well, which
     * xapp_run() has already done -- it is idempotent, and doing it here
     * keeps this board's init in the one place every other user of the
     * model component finds it. */
    esp_err_t err = model_init();
    if (err != ESP_OK) return err;

    ssd1306_handle_t oled = model_get_display();
    if (!oled) return ESP_ERR_NOT_FOUND;   /* init logged why */

    *w = SSD1306_WIDTH;
    *h = SSD1306_HEIGHT;
    *ctx = oled;
    return ESP_OK;
}

/* Big-endian RGB565 in (that is what the UI hands every board), one bit
 * out. Luminance is the usual 0.30/0.59/0.11 in integer form; the cut is
 * set low because the UI's "dim" colours -- the grey of a chart border, the
 * dark orange of an inactive tab -- are still meant to be seen, and on a
 * panel with no grey the choice is between seeing them and not. */
#define MONO_CUT 80

static inline bool px_on(uint16_t be)
{
    uint16_t c = (uint16_t)((be >> 8) | (be << 8));   /* back to native */
    unsigned r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    /* scale 5/6-bit channels to 8 bits, weight, keep the top byte */
    unsigned y = (r * 8 * 77 + g * 4 * 151 + b * 8 * 28) >> 8;
    return y >= MONO_CUT;
}

static void display_flush(int x1, int y1, int x2, int y2,
                          const uint16_t *px, void *ctx)
{
    ssd1306_handle_t oled = (ssd1306_handle_t)ctx;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            ssd1306_draw_pixel(oled, (uint16_t)x, (uint16_t)y, px_on(*px++));
    /* LVGL walks the frame in strips from the top; the strip that ends on
     * the last row is the end of the frame, and the panel is written once. */
    if (y2 >= SSD1306_HEIGHT - 1) ssd1306_display(oled);
}

static void screen_power(bool on)
{
    ssd1306_handle_t oled = model_get_display();
    if (oled) ssd1306_set_on(oled, on);
}

/* ── Battery ────────────────────────────────────────────────────────────── */

static int battery_mv(void)
{
    float v = model_get_battery_voltage();
    if (v < 2.5f) return -1;           /* nothing on the divider: USB only */
    return (int)(v * 1000.0f);
}

/* ── The one button ──────────────────────────────────────────────────────
 *
 * The board has PRG on GPIO0 (the strap pin, active low) and RST, and RST
 * is a reset line, not an input. So this is the T-Dongle's one-button
 * grammar, unchanged, and like the T-Dongle the board asks for the tour at
 * boot (see .rotate below) so the screen is worth looking at untouched:
 *
 *   tap                   NEXT -- step to the next view. The first tap
 *                         also stops the tour.
 *   hold (0.7 s)          HOME -- back to the first view.
 *   keep holding (2 s)    HOME, then DOWN, which on the home view starts
 *                         the tour again.
 */
#define BTN_GPIO        BTN_PIN_BOOT
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
    ESP_LOGI(TAG, "PRG button ready on GPIO%d", (int)BTN_GPIO);
}

/* Called every 10 ms by the UI task. */
static xapp_key_t input_poll(void)
{
    static int  held_ms;
    static bool home_fired, tour_fired;

    if (gpio_get_level(BTN_GPIO) == BTN_ACTIVE_LEVEL) {
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

/* ── The radio ──────────────────────────────────────────────────────────── */

/* Heltec wires DIO2 as the RF switch and a TCXO on DIO3, the same way
 * LilyGO does on the T-Deck, and a build without both is deaf while looking
 * perfectly healthy. 868 MHz and 14 dBm are the fleet's figures; SF7/125k
 * and the rest are fixed inside the bearer so the two ends of a bench test
 * agree by construction. Vext must be on before this radio answers, and
 * model_init() -- called from display_init(), which xapp_run() reaches
 * before it starts the bearers -- is what turns it on. */
static const xprslora_cfg_t k_lora = {
    .sck_pin  = LORA_PIN_SCK,
    .mosi_pin = LORA_PIN_MOSI,
    .miso_pin = LORA_PIN_MISO,
    .cs_pin   = LORA_PIN_NSS,
    .rst_pin  = LORA_PIN_RST,
    .busy_pin = LORA_PIN_BUSY,
    .dio1_pin = LORA_PIN_DIO1,
    .freq_hz  = 0,  /* the region preset's channel; see the T-Deck's note */
    .tx_power_dbm = 14,
    .use_tcxo = true,
    .use_dio2_rf_switch = true,
};

/* ── The board ──────────────────────────────────────────────────────────── */

static const xapp_board_t k_board = {
    .board_id = "heltec-v3",
    .banner   = "LoRa + BLE5 digipeater, 128x64 OLED, no PSRAM",

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
     * panel, so the bottom bar keeps its legends. A LiPo may or may not be
     * on the connector; battery_mv says -1 when it is not. */
    .raw_key     = NULL,
    .touch_read  = NULL,
    .kb_backlight = NULL,
    .battery_mv  = battery_mv,

    .lora = &k_lora,

    /* An S3: extended advertising, so this bearer is real here. */
    .ble = true,

    /* One button; the tour is the interface. */
    .rotate = true,

    /* No walk-up hotspot by default: no PSRAM, and the T-Dongle measured
     * the AP at about 9 KB of internal heap on the same chip. Seeded as a
     * default, so `cfg set ap_on 1` turns it on for anyone who wants it. */
    .hotspot = false,

    /* Measured 2026-08-30, first evening on the air: the end of boot sat
     * at 6,440 free with the bearers just started on a bench of four
     * stations digipeating one another, and steady state on a quieter
     * bench is 14-15 KB with a min-ever of 9.4 KB. Under the storm it
     * touched 172 bytes and lost signature checks to OUT OF MEMORY, which
     * is the failure the floor exists to name. 4,000 is under the boot
     * transient and over that, the T-Dongle's number for the same chip. */
    .heap_floor = 4000,
};

void app_main(void)
{
    /* NOTE: the console is UART0 behind the CP2102 and IDF leaves stdin on
     * it unread, so getchar() returns EOF and this board answers none of
     * the console keys ('S' for a UART framedump, '1'..'8' for a panel) or
     * `cfg set ...` lines. Installing the UART driver and pointing the VFS
     * at it was tried on 2026-08-31 and the station stopped reaching WiFi,
     * so it is left alone: GET /api/screen is the screenshot door here, and
     * it needs no console at all. */
    xapp_run(&k_board);
}
