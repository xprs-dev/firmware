/*
 * An XPRS station on a LilyGO T-Deck.
 *
 * The station is common/xprs_app, the same program the M5Stack runs. What is
 * here is the board: a peripheral rail that has to be switched on, a 320x240
 * ST7789 sharing its SPI bus with a card slot and a radio, a trackball, and a
 * keyboard that is really another ESP32 answering on I2C.
 *
 * The SX1262 on 868 MHz is the board's third bearer, behind the same relay
 * discipline as ESP-NOW and the LAN (xprs_bearer_lora). It shares the SPI
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
#include "gt911.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "st7789.h"
#include "xprs_app.h"
#include "esp_timer.h"
#include "xprs_script.h"   /* phase-0 spike, temporary */

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

#if TDECK_PANEL_SELFTEST
    /* Bench bisect, temporary. Paints the panel red then green through
     * st7789_fill_color(), which is the shortest path from the driver to the
     * glass -- no LVGL, no draw buffer, no flush callback.
     *
     *   colours appear  -> panel, backlight, SPI and CS are all fine, and
     *                      the fault is above this line
     *   nothing appears -> the fault is the panel, its backlight, its power
     *                      or the shared SPI bus, and no amount of looking
     *                      at LVGL will find it
     *
     * Worth having because the UART framedump proved the FRAMEBUFFER is
     * correct, which says nothing at all about what the panel received. */
    ESP_LOGW(TAG, "panel self-test: red, then green, 1 s each");
    st7789_backlight(s_lcd, true);
    st7789_fill_color(s_lcd, 0xF800);
    vTaskDelay(pdMS_TO_TICKS(1000));
    st7789_fill_color(s_lcd, 0x07E0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "panel self-test done");
#endif
    *w = ST7789_WIDTH;
    *h = ST7789_HEIGHT;
    *ctx = s_lcd;
    return ESP_OK;
}

static void display_flush(int x1, int y1, int x2, int y2,
                          const uint16_t *px, void *ctx)
{
    /* The return was discarded here, which meant a panel that never received
     * a byte looked exactly like a panel that did: LVGL renders, the flush
     * callback runs, the framedump over UART is perfect, and the glass stays
     * dark with nothing in the log. Say it once, and then once a minute, so
     * it cannot scroll past unnoticed but also cannot flood. */
    esp_err_t err = st7789_flush((st7789_handle_t)ctx, x1, y1, x2, y2, px);
    if (err != ESP_OK) {
        static int64_t last_us;
        static unsigned suppressed;
        int64_t now = esp_timer_get_time();
        if (now - last_us > 60 * 1000000LL) {
            ESP_LOGE(TAG, "panel flush failed: %s (%u suppressed since)",
                     esp_err_to_name(err), suppressed);
            last_us = now;
            suppressed = 0;
        } else {
            suppressed++;
        }
    }
}

/* ── The trackball ──────────────────────────────────────────────────────── */

/* A trackball is not a button. Rolling it makes and breaks a contact several
 * times per turn, so there is no press to debounce and nothing to hold: what
 * carries the movement is the COUNT OF CHANGES, which is how LilyGO's own
 * code reads it. Levels are polled at 100 Hz alongside everything else in the
 * UI task, so no interrupt service is installed -- which also keeps this out
 * of the way of xprs_sx1262, which installs one of its own.
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
static uint32_t s_dir_last_ms;   /* rate limit for the ball's directions */

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
        /* A flick of the ball breaks the contact many times in a few tens of
         * milliseconds, and the divider alone still let a nudge walk several
         * rows. Nothing may be emitted more often than this, so one flick is
         * one row however fast the ball spins. */
        uint32_t nowms = (uint32_t)(esp_timer_get_time() / 1000);
        if (nowms - s_dir_last_ms < TDECK_TB_MIN_GAP_MS) { s_dir_count[i] = 0; continue; }
        s_dir_last_ms = nowms;
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

/* The GT911 shares the keyboard's bus. Probed right after it, on the same
 * task, and read from LVGL's indev timer -- which also runs on the UI task,
 * so the bus never has two readers. */
static gt911_t s_touch;
static bool    s_touch_ok;
#if TDECK_INPUT_TRACE
static bool    s_trace_down;
/* Touch injection. The panel is detected and a finger wakes the screen, yet
 * no tap ever reaches a target -- so the question is whether the loss is in
 * the GT911 read or in the LVGL wiring above it. This drives synthetic
 * presses through the SAME path a finger takes: if the app logs
 * "touch: bar 0" for one of these, everything above the driver is sound and
 * the fault is the controller read; if it logs nothing, the wiring is. */
static int  s_fake_step;
static uint32_t s_fake_at_ms;
static const struct { int x, y; const char *what; } k_fake[] = {
    { 30, 232, "bottom-left bar slot" },
    { 160, 232, "bottom-middle bar slot" },
    { 100, 60,  "a table row" },
};
#endif

/* Keyboard backlight. The stock LilyGO keyboard MCU takes a one-byte
 * command on its I2C address: 0x01 lights the keys, 0x00 puts them out.
 * (Bench-probed; if a future keyboard firmware toggles instead, keep a
 * shadow here.) Same bus as the keys -- UI task only. */
#if TDECK_INPUT_TRACE && TDECK_KB_BL_SWEEP
/* Which byte, if any, drives the keyboard's own backlight? Nobody in this
 * tree knows: no write to 0x55 ever existed. Try the plausible ones with a
 * gap wide enough to see, and say what is being tried, so the answer comes
 * back as "it lit on N". If none of them lights it, the stock keyboard
 * firmware does not expose it and item 4 needs the C3 reflashed. */
static void kb_backlight_sweep(void)
{
    if (!s_kbd) return;
    ESP_LOGW(TAG, "KBBL sweep: watch the KEYS, 1.5 s per step");
    for (uint8_t v = 0; v <= 0x08; v++) {
        ESP_LOGW(TAG, "KBBL one byte 0x%02x", v);
        i2c_write_bytes(s_kbd, -1, &v, 1);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    static const uint8_t pairs[][2] = {
        {0x01, 0xFF}, {0x01, 0x80}, {0x02, 0xFF}, {0x80, 0xFF}, {0xFF, 0xFF},
    };
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        ESP_LOGW(TAG, "KBBL two bytes 0x%02x 0x%02x", pairs[i][0], pairs[i][1]);
        i2c_write_bytes(s_kbd, -1, pairs[i], 2);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    ESP_LOGW(TAG, "KBBL sweep done -- which step lit the keys?");
}
#endif

static void keyboard_backlight(bool on)
{
    if (!s_kbd) return;
    /* LilyGO's own UnitTest.ino: setKeyboardBrightness() writes TWO bytes to
     * 0x55 -- the command LILYGO_KB_BRIGHTNESS_CMD (0x01) and then the level.
     * A single byte, which is what this tried first, is not a command the
     * keyboard MCU knows and it does nothing at all. */
    uint8_t cmd[2] = { TDECK_KB_BRIGHTNESS_CMD, on ? TDECK_KB_BRIGHTNESS : 0x00 };
    esp_err_t err = i2c_write_bytes(s_kbd, -1, cmd, 2);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "keyboard backlight: %s", esp_err_to_name(err));
}

/* Battery: GPIO4 through 100k/100k, so Vbat = pin mV * 2. Calibrated ADC,
 * eight reads averaged. The unit is claimed in app_main, where there is
 * heap for it, not on the 6 KB UI task. */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_adc_cali;
static adc_channel_t             s_adc_ch;

static void battery_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(TDECK_BAT_ADC, &unit, &s_adc_ch) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) return;
    adc_oneshot_chan_cfg_t cc = { .bitwidth = ADC_BITWIDTH_12,
                                  .atten = ADC_ATTEN_DB_12 };
    adc_oneshot_config_channel(s_adc, s_adc_ch, &cc);
    adc_cali_curve_fitting_config_t cf = { .unit_id = unit, .chan = s_adc_ch,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (adc_cali_create_scheme_curve_fitting(&cf, &s_adc_cali) != ESP_OK)
        s_adc_cali = NULL;
    ESP_LOGI(TAG, "battery ADC on GPIO%d%s", TDECK_BAT_ADC,
             s_adc_cali ? "" : " (uncalibrated)");
}

static int battery_mv(void)
{
    if (!s_adc) return -1;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, s_adc_ch, &raw) != ESP_OK) continue;
        if (s_adc_cali) adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);
        else mv = raw * 3300 / 4095;
        sum += mv; n++;
    }
    return n ? (sum / n) * 2 : -1;
}

static void screen_power(bool on)
{
    if (!s_lcd) return;
    if (on) { st7789_sleep(s_lcd, false); st7789_backlight(s_lcd, true); }
    else    { st7789_backlight(s_lcd, false); st7789_sleep(s_lcd, true); }
}

static bool touch_read(int *x, int *y)
{
#if TDECK_INPUT_TRACE
    /* One synthetic tap every 8 s for the first half-minute: 120 ms down,
     * then up, so LVGL sees a real press/release pair and can raise CLICKED. */
    {
        uint32_t nowms = (uint32_t)(esp_timer_get_time() / 1000);
        static uint32_t first_poll_ms;
        if (!first_poll_ms) first_poll_ms = nowms;
        nowms -= first_poll_ms;      /* from the first poll, not from boot */
        if (s_fake_step < (int)(sizeof k_fake / sizeof k_fake[0])) {
            uint32_t due = 4000 + (uint32_t)s_fake_step * 4000;
            if (nowms >= due && nowms < due + 120) {
                if (nowms - s_fake_at_ms > 1000) {
                    s_fake_at_ms = nowms;
                    ESP_LOGW(TAG, "FAKE tap %d at (%d,%d) -- %s",
                             s_fake_step, k_fake[s_fake_step].x,
                             k_fake[s_fake_step].y, k_fake[s_fake_step].what);
                }
                *x = k_fake[s_fake_step].x;
                *y = k_fake[s_fake_step].y;
                return true;
            }
            if (nowms >= due + 120) s_fake_step++;
        }
    }
#endif
    uint16_t rx, ry;
    if (!s_touch_ok || !gt911_read(&s_touch, &rx, &ry)) {
#if TDECK_INPUT_TRACE
        s_trace_down = false;
#endif
        return false;
    }
    int px = rx, py = ry;
#if TDECK_TOUCH_SWAP_XY
    { int t = px; px = py; py = t; }
#endif
#if TDECK_TOUCH_FLIP_X
    px = (ST7789_WIDTH - 1) - px;
#endif
#if TDECK_TOUCH_FLIP_Y
    py = (ST7789_HEIGHT - 1) - py;
#endif
    if (px < 0) px = 0;
    if (px >= ST7789_WIDTH)  px = ST7789_WIDTH - 1;
    if (py < 0) py = 0;
    if (py >= ST7789_HEIGHT) py = ST7789_HEIGHT - 1;
    *x = px; *y = py;
#if TDECK_INPUT_TRACE
    if (!s_trace_down) {
        s_trace_down = true;
        ESP_LOGW(TAG, "TOUCH raw(%u,%u) -> screen(%d,%d)  [swap=%d flipx=%d flipy=%d]",
                 rx, ry, px, py,
                 TDECK_TOUCH_SWAP_XY, TDECK_TOUCH_FLIP_X, TDECK_TOUCH_FLIP_Y);
    }
#endif
    return true;
}

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

    s_touch_ok = gt911_init(&s_touch) == ESP_OK;
}

static int keyboard_key(void)
{
    if (!s_kbd) return 0;
    uint8_t c = 0;
    if (i2c_read_bytes(s_kbd, -1, &c, 1) != ESP_OK || c == 0) return 0;
#if TDECK_INPUT_TRACE
    ESP_LOGW(TAG, "KBD 0x%02x '%c'", c, (c >= 0x20 && c < 0x7f) ? c : '.');
#endif
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

static xapp_board_t k_tdeck = {
    .board_id = TDECK_BOARD_ID,
    .banner = "ESP-NOW + LAN + LoRa 868 + BLE5",
    .wifi_ssid = WIFI_SSID,
    .wifi_pass = WIFI_PASS,
    .espnow_channel = ESPNOW_FALLBACK_CHANNEL,
    .display_init = display_init,
    .flush = display_flush,
    .input_init = input_init,
    .input_poll = input_poll,
    .raw_key = keyboard_key,
    .touch_read = touch_read,
    .kb_backlight = keyboard_backlight,
    .battery_mv = battery_mv,
    .screen_power = screen_power,
    .lora = &k_tdeck_lora,
    /* This is an S3: CONFIG_SOC_BLE_50_SUPPORTED, so extended advertising is
     * available and an XPRS packet fits one AD. It is the only bearer here a
     * phone can reach with no access point and no pairing. */
    .ble = true,
    /* BENCH KEY, temporary. The private half is
     * 0101..01 and is in the shell history of whoever set this up, which is
     * exactly the handling a real key must never get. Production seeds this
     * from the gitignored src/fw_secrets.h, like fw_key does. */
    .script_key = "1b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f",
};


#if TDECK_INPUT_PROBE
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* GT911 registers are 16-bit, big-endian on the wire. The i2c_bsp helpers
 * take an 8-bit reg, so address bytes go in the write buffer instead. */
static esp_err_t gt_read(i2c_dev_handle_t d, uint16_t reg, uint8_t *buf, uint8_t n)
{
    uint8_t a[2] = { reg >> 8, reg & 0xFF };
    return i2c_write_read(d, a, 2, buf, n);
}
static esp_err_t gt_write8(i2c_dev_handle_t d, uint16_t reg, uint8_t v)
{
    uint8_t a[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_write_bytes(d, -1, a, 3);
}

static void input_probe(void)
{
    static const char *P = "probe";
    ESP_LOGW(P, "==== INPUT PROBE: press keys, touch corners, watch ====");

    /* GT911: which address, what product id. */
    i2c_dev_handle_t gt = NULL;
    uint8_t addrs[2] = { 0x5D, 0x14 };
    for (int i = 0; i < 2 && !gt; i++) {
        i2c_dev_handle_t h;
        if (i2c_bus_add_device(addrs[i], &h) != ESP_OK) continue;
        uint8_t id[4] = {0};
        if (gt_read(h, 0x8140, id, 4) == ESP_OK && id[0] == '9' && id[1] == '1') {
            ESP_LOGW(P, "GT911 at 0x%02X, id '%c%c%c%c'", addrs[i],
                     id[0], id[1], id[2], id[3]);
            gt = h;
        } else {
            ESP_LOGW(P, "no GT911 at 0x%02X (id %02x %02x %02x %02x)",
                     addrs[i], id[0], id[1], id[2], id[3]);
            i2c_bus_remove_device(h);
        }
    }
    if (gt) {
        uint8_t cfg[4] = {0};
        if (gt_read(gt, 0x8048, cfg, 4) == ESP_OK)   /* x/y output max */
            ESP_LOGW(P, "GT911 configured max x=%u y=%u",
                     cfg[0] | (cfg[1] << 8), cfg[2] | (cfg[3] << 8));
    }

    /* Battery ADC. */
    adc_oneshot_unit_handle_t adc = NULL;
    adc_cali_handle_t cali = NULL;
    adc_unit_t unit; adc_channel_t ch;
    if (adc_oneshot_io_to_channel(TDECK_BAT_ADC, &unit, &ch) == ESP_OK) {
        adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
        adc_oneshot_new_unit(&uc, &adc);
        adc_oneshot_chan_cfg_t cc = { .bitwidth = ADC_BITWIDTH_12,
                                      .atten = ADC_ATTEN_DB_12 };
        adc_oneshot_config_channel(adc, ch, &cc);
        adc_cali_curve_fitting_config_t cf = { .unit_id = unit, .chan = ch,
            .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        if (adc_cali_create_scheme_curve_fitting(&cf, &cali) != ESP_OK) cali = NULL;
        ESP_LOGW(P, "battery ADC on GPIO%d unit %d ch %d cali=%s",
                 TDECK_BAT_ADC, unit, ch, cali ? "yes" : "NO");
    } else {
        ESP_LOGW(P, "GPIO%d is not ADC capable", TDECK_BAT_ADC);
    }

    /* Keyboard backlight: try host writes, 0x01 then 0x00, twice. Watch the
     * keys. Says SET if 0x01 lights and 0x00 darkens; TOGGLE if each write
     * flips it; NONE if nothing happens. */
    ESP_LOGW(P, "kbd backlight: writing 0x01 (watch the keys)");
    uint8_t one = 1, zero = 0;
    esp_err_t w = i2c_write_bytes(s_kbd, -1, &one, 1);
    ESP_LOGW(P, "  write 0x01 -> %s", esp_err_to_name(w));
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(P, "kbd backlight: writing 0x00");
    w = i2c_write_bytes(s_kbd, -1, &zero, 1);
    ESP_LOGW(P, "  write 0x00 -> %s", esp_err_to_name(w));
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(P, "kbd backlight: writing 0x01 again");
    i2c_write_bytes(s_kbd, -1, &one, 1);
    vTaskDelay(pdMS_TO_TICKS(3000));
    i2c_write_bytes(s_kbd, -1, &zero, 1);
    ESP_LOGW(P, "kbd backlight: done; now press keys and touch corners");

    /* Loop: keyboard bytes, touch points, battery every 2 s. */
    int tick = 0;
    bool was_down = false;
    for (;;) {
        uint8_t c = 0;
        if (s_kbd && i2c_read_bytes(s_kbd, -1, &c, 1) == ESP_OK && c)
            ESP_LOGW(P, "kbd 0x%02x '%c'", c, (c >= 0x20 && c < 0x7f) ? c : '.');

        if (gt) {
            uint8_t st = 0;
            if (gt_read(gt, 0x814E, &st, 1) == ESP_OK) {
                if (st & 0x80) {
                    int n = st & 0x0F;
                    if (n > 0) {
                        uint8_t p[8] = {0};
                        gt_read(gt, 0x8150, p, 8);
                        int x = p[1] | (p[2] << 8), y = p[3] | (p[4] << 8);  /* p[0] is the track id */
                        ESP_LOGW(P, "touch n=%d raw x=%d y=%d", n, x, y);
                        was_down = true;
                    } else if (was_down) {
                        ESP_LOGW(P, "touch up");
                        was_down = false;
                    }
                    gt_write8(gt, 0x814E, 0);
                }
            }
        }

        if (adc && (++tick % 200) == 0) {
            int raw = 0, mv = 0;
            adc_oneshot_read(adc, ch, &raw);
            if (cali) adc_cali_raw_to_voltage(cali, raw, &mv);
            ESP_LOGW(P, "battery raw=%d pin=%d mV -> Vbat=%d mV", raw, mv, mv * 2);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif /* TDECK_INPUT_PROBE */

void app_main(void)
{
    board_power_up();
    keyboard_init();
    battery_init();
#if TDECK_INPUT_TRACE && TDECK_KB_BL_SWEEP
    kb_backlight_sweep();
#endif
#if TDECK_INPUT_PROBE
    input_probe();          /* never returns */
#endif

    /* PHASE-0 SPIKE, temporary. Claimed here, before xapp_run(), because this
     * is where the heap is still one large block -- docs/esp32.md: "prefer
     * claiming a large stack early over hoping it fits later". The measurement
     * sequence is queued now and drained by the script task as it comes up;
     * the non-terminating case waits for the station to finish booting before
     * it starts, so reachability is measured against a running station. */
    if (xs_start() == ESP_OK) xs_spike(120);

    /* No GT911 answered: say so in the descriptor, so the bar keeps the
     * button legends instead of naming taps nobody can make. */
    if (!s_touch_ok) k_tdeck.touch_read = NULL;
    xapp_run(&k_tdeck);
}
