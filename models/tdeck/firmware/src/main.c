/*
 * An XPRS station on a LilyGO T-Deck.
 *
 * The station is common/xprs_app, the same program the M5Stack runs. What is
 * here is the board: a peripheral rail that has to be switched on, a 320x240
 * ST7789 sharing its SPI bus with a card slot and a radio, a trackball, and a
 * keyboard that is really another ESP32 answering on I2C.
 *
 * The SX1262 on 868 MHz is the board's third bearer, behind the same relay
 * discipline as ESP-NOW and the LAN (geogram_xprslora). It shares the SPI
 * bus with the panel, which is why its chip select is parked HIGH before
 * anything drives the bus, and why the sx1262 driver holds the bus for
 * exactly its chip-select windows.
 */

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c_bsp.h"
#include "st7789.h"
#include "xprs_app.h"

#include "board.h"
#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */

static const char *TAG = "tdeck";

/* ── The rail, and staying off other people's bus ───────────────────────── */

static void board_power_up(void)
{
    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << TDECK_POWERON) |
                        (1ULL << TDECK_SD_CS) |
                        (1ULL << TDECK_RADIO_CS),
    };
    gpio_config(&out);

    /* The card and the radio share the panel's bus. Park both chip selects
     * HIGH before anything drives the bus, or they answer transactions meant
     * for the screen. */
    gpio_set_level(TDECK_SD_CS, 1);
    gpio_set_level(TDECK_RADIO_CS, 1);

    gpio_set_level(TDECK_POWERON, 1);
    /* The keyboard is an ESP32-C3: it has its own firmware to boot before it
     * will answer on I2C. Nothing else needs the wait, and it costs one
     * half-second once. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "peripheral rail on");
}

/* ── The screen ─────────────────────────────────────────────────────────── */

static st7789_handle_t s_lcd;

static esp_err_t display_init(int *w, int *h, void **ctx)
{
    st7789_config_t cfg = {
        .host = SPI2_HOST,
        .mosi_pin = TDECK_SPI_MOSI,
        .miso_pin = TDECK_SPI_MISO,
        .sclk_pin = TDECK_SPI_SCLK,
        .cs_pin = TDECK_TFT_CS,
        .dc_pin = TDECK_TFT_DC,
        .rst_pin = TDECK_TFT_RST,
        .bl_pin = TDECK_TFT_BL,
        /* 40 MHz: what LilyGO's own TFT_eSPI setup runs this panel at, so
         * the glass is known to take it. Brought up at 20 and raised once
         * the picture was proven -- through the S3's GPIO matrix a bad
         * clock draws confetti, which looks just like bad wiring. */
        .clock_hz = 40 * 1000 * 1000,
    };
    esp_err_t err = st7789_init(&cfg, &s_lcd);
    if (err != ESP_OK) return err;
    *w = ST7789_WIDTH;
    *h = ST7789_HEIGHT;
    *ctx = s_lcd;
    return ESP_OK;
}

static void display_flush(int x1, int y1, int x2, int y2,
                          const uint16_t *px, void *ctx)
{
    st7789_flush((st7789_handle_t)ctx, x1, y1, x2, y2, px);
}

/* ── The trackball ──────────────────────────────────────────────────────── */

/* A trackball is not a button. Rolling it makes and breaks a contact several
 * times per turn, so there is no press to debounce and nothing to hold: what
 * carries the movement is the COUNT OF CHANGES, which is how LilyGO's own
 * code reads it. Levels are polled at 100 Hz alongside everything else in the
 * UI task, so no interrupt service is installed -- which also keeps this out
 * of the way of geogram_sx1262, which installs one of its own.
 *
 * The click is an ordinary button and is debounced like one. */
static const int k_dir_pin[4] = {
    TDECK_TB_UP, TDECK_TB_DOWN, TDECK_TB_LEFT, TDECK_TB_RIGHT
};
static bool s_dir_last[4];
static int  s_dir_count[4];

static void input_init(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << TDECK_TB_UP) | (1ULL << TDECK_TB_DOWN) |
                        (1ULL << TDECK_TB_LEFT) | (1ULL << TDECK_TB_RIGHT) |
                        (1ULL << TDECK_TB_CLICK),
    };
    gpio_config(&io);
    for (int i = 0; i < 4; i++) s_dir_last[i] = gpio_get_level(k_dir_pin[i]);
    ESP_LOGI(TAG, "trackball ready");
}

/* Called every 10 ms. */
static xapp_key_t input_poll(void)
{
    static int click_held_ms;
    static bool click_long_fired;

    /* The click first: a hold is ESC, and while it is held the ball is
     * almost certainly being nudged too. */
    if (gpio_get_level(TDECK_TB_CLICK) == 0) {
        click_held_ms += 10;
        if (click_held_ms >= 700 && !click_long_fired) {
            click_long_fired = true;
            ESP_LOGI(TAG, "trackball click long");
            return XAPP_KEY_HOME;
        }
    } else {
        bool fire = click_held_ms >= 30 && !click_long_fired;
        click_held_ms = 0;
        click_long_fired = false;
        if (fire) {
            ESP_LOGI(TAG, "trackball click");
            return XAPP_KEY_NEXT;
        }
    }

    for (int i = 0; i < 4; i++) {
        bool now = gpio_get_level(k_dir_pin[i]);
        if (now == s_dir_last[i]) continue;
        s_dir_last[i] = now;
        if (++s_dir_count[i] < TDECK_TB_DIVIDER) continue;
        s_dir_count[i] = 0;
        switch (i) {
        case 0: return XAPP_KEY_UP;
        case 1: return XAPP_KEY_DOWN;
        case 2: return XAPP_KEY_PREV;
        default: return XAPP_KEY_NEXT;
        }
    }
    return XAPP_KEY_NONE;
}

/* ── The keyboard ───────────────────────────────────────────────────────── */

/* It answers with one byte: the key, or zero for nothing pending. Enter is
 * 0x0d and backspace 0x08; letters arrive lower case unless shift is down.
 *
 * The byte is passed on RAW. Where the station's console commands are
 * concerned 's' and 'S' are one key and xprs_app folds the case itself;
 * where somebody is writing a message they are two different letters, and
 * folding here would have thrown that away before anybody could ask.
 * Offering this at all is what tells xprs_app the board can be typed on,
 * and so earns it the interactive chat panel. */
static i2c_dev_handle_t s_kbd;

static void keyboard_init(void)
{
    i2c_bus_config_t bus = {
        .sda_pin = TDECK_I2C_SDA,
        .scl_pin = TDECK_I2C_SCL,
        .port = I2C_NUM_0,
        .freq_hz = 100000,
    };
    if (i2c_bus_init(&bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed — no keyboard");
        return;
    }
    if (i2c_bus_add_device(TDECK_KBD_ADDR, &s_kbd) != ESP_OK) {
        ESP_LOGW(TAG, "keyboard not on the bus at 0x%02X", TDECK_KBD_ADDR);
        s_kbd = NULL;
        return;
    }
    ESP_LOGI(TAG, "keyboard at 0x%02X", TDECK_KBD_ADDR);
}

static int keyboard_key(void)
{
    if (!s_kbd) return 0;
    uint8_t c = 0;
    if (i2c_read_bytes(s_kbd, -1, &c, 1) != ESP_OK || c == 0) return 0;
    return c;
}

/* ── This board ─────────────────────────────────────────────────────────── */

/* The radio: LilyGO wires DIO2 as the RF switch and a TCXO on DIO3, and a
 * build without both is deaf while looking perfectly healthy. */
static const xprslora_cfg_t k_tdeck_lora = {
    .sck_pin = TDECK_SPI_SCLK,
    .mosi_pin = TDECK_SPI_MOSI,
    .miso_pin = TDECK_SPI_MISO,
    .cs_pin = TDECK_RADIO_CS,
    .rst_pin = TDECK_RADIO_RST,
    .busy_pin = TDECK_RADIO_BUSY,
    .dio1_pin = TDECK_RADIO_DIO1,
    .freq_hz = 868000000u,
    .tx_power_dbm = 14,
    .use_tcxo = true,
    .use_dio2_rf_switch = true,
};

static const xapp_board_t k_tdeck = {
    .board_id = TDECK_BOARD_ID,
    .banner = "ESP-NOW + LAN + LoRa 868",
    .wifi_ssid = WIFI_SSID,
    .wifi_pass = WIFI_PASS,
    .espnow_channel = ESPNOW_FALLBACK_CHANNEL,
    .display_init = display_init,
    .flush = display_flush,
    .input_init = input_init,
    .input_poll = input_poll,
    .raw_key = keyboard_key,
    .lora = &k_tdeck_lora,
};

void app_main(void)
{
    board_power_up();
    keyboard_init();
    xapp_run(&k_tdeck);
}
