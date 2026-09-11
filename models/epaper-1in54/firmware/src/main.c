/*
 * An XPRS station on a Waveshare ESP32-S3-ePaper-1.54.
 *
 * The station is common/xprs_app, the same program the T-Deck, the T-Dongle
 * and the M5Stack run: BLE5, ESP-NOW and the LAN as bearers, the digipeater
 * and the bridge between them, the archive, the HTTP API. What is here is
 * the board: power switches that have to be set before anything else, a
 * 200x200 e-paper panel that must not be refreshed like an LCD, a room
 * thermometer, and a battery.
 *
 * The screen is xprs_ui_paper, one black-and-white page. It shows what the
 * station knows (who is in reach, the newest messages, the network) plus the
 * one thing only this board knows, the room's temperature and humidity,
 * which is pushed from here with xup_set_climate().
 *
 * The archive lives on the microSD card when one is in the slot: the 4 MB
 * of flash leave 832 KB for files, which is less than a single index
 * segment (see storage_mount below).
 *
 * The two buttons turn the page a quarter at a time, for however the cable
 * lets the board stand.
 *
 * NOT YET USED: the PCF85063 RTC, and the ES8311 codec with its microphone
 * and speaker. The pins are in board.h.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "epd.h"
#include "i2c_bsp.h"
#include "shtc3.h"
#include "xprs_app.h"
#include "xprs_ui_paper.h"

#include "board.h"
#include "fw_secrets.h"     /* gitignored; see fw_secrets.h.example   */
#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */

static const char *TAG = "epaper";

static void buttons_start(void);

/* The SHTC3 sits inside the case beside an ESP32-S3 that runs WiFi and BLE
 * all day, and reads warmer than the room. Calibrated on the bench board,
 * 2026-09-11, warm (half an hour up, on USB): the room was 23.0 C by a
 * thermometer and the sensor read 34.4, so 11.4. The legacy build's 6 C left
 * the page at 28.4. The raw value is logged next to the corrected one, so
 * this can be checked again. */
#define TEMP_SELF_HEAT_C 11.4f
#define CLIMATE_EVERY_S  30

/* Saturation vapour pressure over water, hPa (Magnus, Sonntag 1990). */
static float svp_hpa(float t_c)
{
    return 6.112f * expf(17.62f * t_c / (243.12f + t_c));
}

/* Relative humidity is relative to the air's temperature, and the sensor's
 * air is 11 C warmer than the room's: the same water reads about half as
 * humid there. Carry the water across (same vapour pressure) to the room's
 * temperature, or the page would show the room far drier than it is. */
static float room_rh(float rh_sensor, float t_sensor, float t_room)
{
    float rh = rh_sensor * svp_hpa(t_sensor) / svp_hpa(t_room);
    return rh > 100.0f ? 100.0f : rh;
}

/* ── Power ──────────────────────────────────────────────────────────────── */

static void board_power_up(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PWR_PIN_VBAT) | (1ULL << PWR_PIN_EPD) |
                        (1ULL << PWR_PIN_AUDIO) | (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    /* The latch first: on battery the board is only powered while the PWR
     * button is held, until this pin says otherwise. */
    gpio_set_level(PWR_PIN_VBAT, 1);
    gpio_set_level(PWR_PIN_EPD, 0);      /* panel on */
    gpio_set_level(PWR_PIN_AUDIO, 1);    /* codec and amplifier off */
    gpio_set_level(LED_PIN, 1);          /* LED off */
    gpio_config(&out);
    gpio_set_level(PWR_PIN_VBAT, 1);
    gpio_set_level(PWR_PIN_EPD, 0);
    gpio_set_level(PWR_PIN_AUDIO, 1);
    gpio_set_level(LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));      /* let the panel's rail settle */
    ESP_LOGI(TAG, "power: latch on, panel on, audio off");
}

/* ── The screen, and which way up it is ─────────────────────────────────── */

/*
 * The USB cable decides how this board can stand, so the page turns: every
 * press of either button rotates it 90 degrees clockwise, and the choice is
 * remembered. It is kept where the old firmware kept it (NVS "display" /
 * "rotation", in degrees), so a board that was set up under the old build
 * comes up the way it was left.
 *
 * The turn happens here, when the picture is packed for the panel, not in
 * the UI: the page is square, so its layout does not change, and xprs_ui_paper
 * goes on drawing one upright page. A screenshot therefore shows the page
 * the way it is read, whichever way the board is standing. What the old
 * firmware also knew is that a turn has to be a FULL refresh: every pixel
 * moves, and a partial update leaves the old picture standing behind the
 * new one.
 */
#define ROT_NVS_NS  "display"
#define ROT_NVS_KEY "rotation"

static SemaphoreHandle_t s_frame_lock;   /* the two frames and the angle */
static uint8_t s_logical[EPD_W / 8 * EPD_H]; /* the page as drawn, upright */
static uint8_t s_img[EPD_W / 8 * EPD_H];     /* the page as the panel gets it */
static bool s_have_frame;
static int s_rot;                            /* quarter turns clockwise, 0..3 */

static void rotation_load(void)
{
    nvs_handle_t h;
    int32_t deg = 0;
    if (nvs_open(ROT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, ROT_NVS_KEY, &deg);
        nvs_close(h);
    }
    s_rot = (deg == 90 || deg == 180 || deg == 270) ? (int)(deg / 90) : 0;
    ESP_LOGI(TAG, "page turned %d degrees", s_rot * 90);
}

static void rotation_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ROT_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, ROT_NVS_KEY, s_rot * 90);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "rotation not saved: %s", esp_err_to_name(err));
}

/* The upright page into the panel's frame, turned s_rot quarters clockwise
 * (the old firmware's mapping). The panel is square, so W and H are one. */
static void pack_turned(void)
{
    const int rb = EPD_W / 8;
    memset(s_img, 0, sizeof s_img);
    for (int y = 0; y < EPD_H; y++) {
        for (int x = 0; x < EPD_W; x++) {
            if (!(s_logical[y * rb + (x >> 3)] & (0x80 >> (x & 7)))) continue;
            int tx, ty;
            switch (s_rot) {
            case 1:  tx = EPD_H - 1 - y; ty = x;             break;
            case 2:  tx = EPD_W - 1 - x; ty = EPD_H - 1 - y; break;
            case 3:  tx = y;             ty = EPD_W - 1 - x; break;
            default: tx = x;             ty = y;             break;
            }
            s_img[ty * rb + (tx >> 3)] |= (uint8_t)(0x80 >> (tx & 7));
        }
    }
}

static esp_err_t display_init(int *w, int *h, void **ctx)
{
    s_frame_lock = xSemaphoreCreateMutex();
    if (!s_frame_lock) return ESP_ERR_NO_MEM;
    rotation_load();
    epd_cfg_t cfg = {
        .host = EPD_SPI_HOST,
        .sck = EPD_PIN_SCK, .mosi = EPD_PIN_MOSI, .cs = EPD_PIN_CS,
        .dc = EPD_PIN_DC, .rst = EPD_PIN_RST, .busy = EPD_PIN_BUSY,
        .width = EPD_W, .height = EPD_H,
    };
    esp_err_t err = epd_init(&cfg);
    if (err != ESP_OK) return err;
    buttons_start();
    *w = EPD_W;
    *h = EPD_H;
    *ctx = NULL;
    return ESP_OK;
}

/* xprs_ui_paper hands over the whole screen, already black and white, only
 * when it has changed (xprs_ui_paper.h). Pack it to one bit per pixel, turn
 * it, and let the panel's task do the slow part. UI task. */
static void display_flush(int x1, int y1, int x2, int y2,
                          const uint16_t *px, void *ctx)
{
    (void)ctx;
    if (x1 != 0 || y1 != 0 || x2 != EPD_W - 1 || y2 != EPD_H - 1) {
        ESP_LOGW(TAG, "flush of a window (%d,%d)-(%d,%d): this panel takes "
                      "whole frames only", x1, y1, x2, y2);
        return;
    }
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    memset(s_logical, 0, sizeof s_logical);
    for (int y = 0; y < EPD_H; y++) {
        uint8_t *row = s_logical + y * (EPD_W / 8);
        for (int x = 0; x < EPD_W; x++, px++)
            if (*px) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
    s_have_frame = true;
    pack_turned();
    epd_show(s_img);
    xSemaphoreGive(s_frame_lock);
}

/* A quarter turn clockwise. Buttons task. */
static void rotate_cw(void)
{
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    s_rot = (s_rot + 1) % 4;
    if (s_have_frame) {
        pack_turned();
        epd_request_full();
        epd_show(s_img);
    }
    xSemaphoreGive(s_frame_lock);
    rotation_save();            /* a flash write: outside the lock, never in an ISR */
    ESP_LOGI(TAG, "page turned %d degrees (stack %u free)", s_rot * 90,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

/* ── The buttons ────────────────────────────────────────────────────────── */

/*
 * BOOT and PWR, both active low, polled every 20 ms on a task of their own.
 * Polled, not interrupt-driven, because all a press does is a flash write and
 * a refresh, neither of which may happen in an ISR anyway.
 *
 * They are NOT handed to xprs_app as keys. Any key stops the hands-off tour,
 * and the tour is how the page gets both its halves (see .rotate below).
 *
 * A press counts on release, after at least 40 ms down. And a button that is
 * already down when the task starts is ignored until it has been let go: on
 * battery the board is switched on by HOLDING PWR, and the release at the end
 * of that is not a request to turn the page.
 */
#define BTN_POLL_MS    20
#define BTN_MIN_MS     40

static void buttons_task(void *arg)
{
    (void)arg;
    static const int pin[2] = { BTN_PIN_BOOT, BTN_PIN_PWR };
    static const char *const name[2] = { "BOOT", "PWR" };
    bool armed[2] = { false, false };
    int held_ms[2] = { 0, 0 };
    for (;;) {
        for (int i = 0; i < 2; i++) {
            bool down = gpio_get_level(pin[i]) == 0;
            if (!armed[i]) {
                if (!down) armed[i] = true;
                continue;
            }
            if (down) {
                held_ms[i] += BTN_POLL_MS;
            } else {
                if (held_ms[i] >= BTN_MIN_MS) {
                    ESP_LOGI(TAG, "%s pressed", name[i]);
                    rotate_cw();
                }
                held_ms[i] = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

static void buttons_start(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BTN_PIN_BOOT) | (1ULL << BTN_PIN_PWR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    if (xTaskCreatePinnedToCore(buttons_task, "buttons", 3072, NULL, 2, NULL, 1)
        != pdPASS)
        ESP_LOGE(TAG, "buttons task did not start: the page cannot be turned");
}

/* ── The room ───────────────────────────────────────────────────────────── */

static i2c_dev_handle_t s_shtc3;

/* The latest good reading, for the station's report (climate_report). */
static portMUX_TYPE s_clim_mux = portMUX_INITIALIZER_UNLOCKED;
static float    s_last_t, s_last_rh;
static int64_t  s_last_us;          /* 0: no good reading yet */

static void climate_task(void *arg)
{
    (void)arg;
    int n = 0;
    for (;;) {
        float t, rh;
        esp_err_t err = shtc3_measure(s_shtc3, &t, &rh);
        if (err == ESP_OK) {
            float room = t - TEMP_SELF_HEAT_C;
            float rrh = room_rh(rh, t, room);
            xup_set_climate(true, room, rrh);
            portENTER_CRITICAL(&s_clim_mux);
            s_last_t = room;
            s_last_rh = rrh;
            s_last_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_clim_mux);
            if (n++ % 10 == 0)          /* every five minutes is plenty */
                ESP_LOGI(TAG, "climate: %.1f C %.0f%% RH (sensor %.1f C "
                              "%.0f%%) (stack %u free)",
                         (double)room, (double)rrh, (double)t, (double)rh,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            xup_set_climate(false, NAN, NAN);
            ESP_LOGW(TAG, "climate: read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(CLIMATE_EVERY_S * 1000));
    }
}

/*
 * The room, on the air (xapp_board_t.report, section 15.3): the indoor keys,
 * because this sensor is in a room, and a reading that says so is not
 * mistaken for the air outside by a neighbour who uses it.
 *
 * Whole degrees and whole percent. The number of decimals is the precision
 * claimed (section 4, "Trailing zeros are significant"), and a reading
 * corrected by 11 C of the board's own heat is good to a degree, not to a
 * tenth. Nothing at all when the last good reading is more than two periods
 * old: a stale number sent as if current is worse than none.
 *
 * Called on the station's storage task: it only copies what the climate
 * task measured.
 */
static int climate_report(char *out, int cap)
{
    portENTER_CRITICAL(&s_clim_mux);
    float t = s_last_t, rh = s_last_rh;
    int64_t at = s_last_us;
    portEXIT_CRITICAL(&s_clim_mux);
    if (!at || esp_timer_get_time() - at > 2LL * CLIMATE_EVERY_S * 1000000)
        return 0;
    return snprintf(out, (size_t)cap, "intemp:%ldC inhum:%ld%%",
                    lroundf(t), lroundf(rh));
}

static void climate_start(void)
{
    i2c_bus_config_t bus = {
        .sda_pin = I2C_PIN_SDA,
        .scl_pin = I2C_PIN_SCL,
        .port = I2C_NUM_0,
        .freq_hz = 100000,
    };
    uint16_t id = 0;
    if (i2c_bus_init(&bus) != ESP_OK ||
        i2c_bus_add_device(I2C_ADDR_SHTC3, &s_shtc3) != ESP_OK ||
        shtc3_probe(s_shtc3, &id) != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 did not answer at 0x%02X, no climate",
                 I2C_ADDR_SHTC3);
        return;
    }
    ESP_LOGI(TAG, "SHTC3 at 0x%02X, id 0x%04X", I2C_ADDR_SHTC3, id);
    /* Core 1 and a small stack: it sleeps for thirty seconds at a time and
     * the bus is its own (nothing else here talks I2C yet). */
    if (xTaskCreatePinnedToCore(climate_task, "climate", 3072, NULL, 2, NULL, 1)
        != pdPASS)
        ESP_LOGE(TAG, "climate task did not start");
}

/* ── The battery ────────────────────────────────────────────────────────── */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_adc_ch;

static void battery_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BAT_ADC_GPIO, &unit, &s_adc_ch) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { s_adc = NULL; return; }
    adc_oneshot_chan_cfg_t cc = { .bitwidth = ADC_BITWIDTH_12,
                                  .atten = ADC_ATTEN_DB_12 };
    adc_oneshot_config_channel(s_adc, s_adc_ch, &cc);
    adc_cali_curve_fitting_config_t cf = { .unit_id = unit, .chan = s_adc_ch,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = NULL;
    ESP_LOGI(TAG, "battery ADC on GPIO%d%s", BAT_ADC_GPIO,
             s_cali ? "" : " (uncalibrated)");
}

static int battery_mv(void)
{
    if (!s_adc) return -1;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, s_adc_ch, &raw) != ESP_OK) continue;
        if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
        else mv = raw * 3300 / 4095;
        sum += mv;
        n++;
    }
    return n ? (sum / n) * BAT_DIVIDER : -1;
}

/* ── Storage ────────────────────────────────────────────────────────────── */

/* The station's files at /idx (xapp_board_t.storage_mount). Called once, on
 * the storage task.
 *
 * The card, when there is one: the archive gets the same 10 MB every other
 * board keeps. It is NEVER formatted from here: a card that does not mount
 * may be somebody's photographs, and a station that wipes it to make room
 * for packets has done something nobody asked for. It is reported and the
 * board falls back to its flash.
 *
 * The flash otherwise: the 832 KB "storage" partition holds the log, the
 * statistics and the conversation, and no archive, because one index
 * segment is 1.3 MB and the segment being written can never be evicted. */
static esp_err_t storage_mount(uint64_t *archive_bytes)
{
    esp_vfs_fat_sdmmc_mount_config_t sd_mc = {
        .format_if_mount_failed = false,
        .max_files = CONFIG_SDCARD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0 = SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/idx", &host, &slot, &sd_mc, &card);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "storage: microSD at /idx, %llu MB; the archive is there",
                 (unsigned long long)card->csd.capacity *
                 card->csd.sector_size / (1024u * 1024u));
        *archive_bytes = 10u * 1024u * 1024u;
        return ESP_OK;
    }
    ESP_LOGW(TAG, "storage: no usable microSD (%s); using the flash, "
                  "without an archive", esp_err_to_name(err));

    static wl_handle_t wl = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t mc = {
        .max_files = CONFIG_SDCARD_MAX_FILES,
        .format_if_mount_failed = true,     /* our own partition, never a card */
        .allocation_unit_size = 4096,
    };
    *archive_bytes = 0;
    return esp_vfs_fat_spiflash_mount_rw_wl("/idx", "storage", &mc, &wl);
}

/* ── This board ─────────────────────────────────────────────────────────── */

static const xapp_board_t k_board = {
    .board_id = EPAPER_BOARD_ID,
    .banner   = "ESP-NOW + LAN + BLE5, 200x200 e-paper",

    .fw_key     = FW_DEFAULT_KEY,
    .fw_owner   = FW_DEFAULT_OWNER,
    .script_key = NULL,         /* falls back to fw_key; no panels shipped */

    .wifi_ssid = WIFI_SSID,
    .wifi_pass = WIFI_PASS,
    .espnow_channel = ESPNOW_FALLBACK_CHANNEL,

    .display_init = display_init,
    .flush        = display_flush,
    /* An e-paper panel holds its picture with the power off and costs
     * nothing to leave showing, so it is never blanked for idleness. */
    .screen_power = NULL,

    /* The two buttons turn the page (buttons_task above) and are kept away
     * from the station: a key would stop the tour below. */
    .input_init = NULL,
    .input_poll = NULL,
    .raw_key    = NULL,
    .touch_read = NULL,
    .kb_backlight = NULL,

    .battery_mv = battery_mv,
    .lora = NULL,
    .storage_mount = storage_mount,

    /* The room's temperature and humidity as a t:observation, every
     * minute; `cfg set report_s <seconds>` changes it. */
    .report   = climate_report,
    .report_s = 60,

    /* An S3: BLE5 extended advertising, so a phone can reach it with no
     * access point. */
    .ble = true,

    /* The page needs the home panel (who is in reach) and the chat panel
     * (the messages), and the app fills in only the panel it is on. The
     * tour visits both every 90 s; see xprs_ui_paper.c. */
    .rotate = true,

    /* The walk-up hotspot: this board goes where there may be no LAN, and
     * then its own access point, with the chat page on it, is the only way
     * a phone can reach it. Up whenever there is no LAN; with a LAN it is
     * up too, and the power policy takes it down on battery when nobody is
     * on it (xprs_power). A default only: `cfg set ap_on 0` turns it off. */
    .hotspot = true,
};

void app_main(void)
{
    board_power_up();
    battery_init();
    climate_start();
    xapp_run(&k_board);
}
