/*
 * The 1.54" e-paper panel, SSD1681 class. See epd.h for the contract.
 *
 * The command sequences are Waveshare's (EPD_1in54_V2 in their examples,
 * and the vendor's own driver for this board, epaper_driver_bsp.cpp), which
 * common/xprs_epaper_1in54 also copied for the legacy build. Two things are
 * different here, both learned from what that copy did not do:
 *
 *   - every wait on BUSY has a deadline. The legacy loop waited forever, so
 *     a panel without power (GPIO6 not driven low) hung whichever task
 *     asked, silently.
 *   - the RAM addressing (data entry mode, window, cursor) is restated
 *     before every RAM write. A hardware reset, which the partial-mode init
 *     does, puts those registers back to their defaults, and the base-image
 *     write needs the cursor back at the start between its two planes.
 */

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "epd.h"

static const char *TAG = "epd";

/* Waveform tables: 153 bytes of LUT, then the gate/source voltages and VCOM
 * that go with them (the last six bytes, sent by epd_lut()). */
static const uint8_t k_lut_full[159] = {
    0x80, 0x48, 0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x40, 0x48, 0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x80, 0x48, 0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x40, 0x48, 0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0xA,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x8,  0x1,  0x0,  0x8,  0x1,  0x0,  0x2,
    0xA,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0,  0x0,  0x0,
    0x22, 0x17, 0x41, 0x0,  0x32, 0x20
};

static const uint8_t k_lut_partial[159] = {
    0x0,  0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x80, 0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x40, 0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0xF,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x1,  0x1,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0,  0x0,  0x0,
    0x02, 0x17, 0x41, 0xB0, 0x32, 0x28,
};

static epd_cfg_t s_cfg;
static spi_device_handle_t s_spi;
static size_t s_bytes;

static uint8_t *s_next;         /* the newest picture asked for             */
static uint8_t *s_work;         /* the one being sent (DMA-capable)         */
static bool s_have_next;
static bool s_want_full = true; /* the first picture is always full         */
static SemaphoreHandle_t s_lock;  /* a mutex, not a spinlock: 5 KB is copied under it */
static TaskHandle_t s_task;

static uint32_t s_refreshes, s_fulls;

/* ── The wire ──────────────────────────────────────────────────────────── */

static void tx(const uint8_t *buf, size_t len, bool data)
{
    gpio_set_level(s_cfg.dc, data);
    gpio_set_level(s_cfg.cs, 0);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = buf };
    /* Short writes poll; the picture goes by DMA and lets the core go. */
    esp_err_t err = len > 32 ? spi_device_transmit(s_spi, &t)
                             : spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(s_cfg.cs, 1);
    if (err != ESP_OK) ESP_LOGE(TAG, "spi: %s", esp_err_to_name(err));
}

static void cmd(uint8_t c) { tx(&c, 1, false); }

static void data(const uint8_t *d, size_t n)
{
    /* Parameters come from the stack and the LUTs from flash, neither of
     * which DMA can read; the SPI driver copies those into a buffer it can.
     * Only the picture is kept in DMA-capable memory to begin with. */
    tx(d, n, true);
}

static void cmd_data(uint8_t c, const uint8_t *d, size_t n)
{
    cmd(c);
    if (n) data(d, n);
}

#define CMD(c, ...) do { const uint8_t d_[] = { __VA_ARGS__ };        \
                         cmd_data((c), d_, sizeof d_); } while (0)

static bool wait_busy(uint32_t timeout_ms, const char *what)
{
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(s_cfg.busy) == 1) {
        if (esp_timer_get_time() > until) {
            ESP_LOGE(TAG, "busy for over %lu ms during %s: is the panel "
                          "powered?", (unsigned long)timeout_ms, what);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

static void hw_reset(void)
{
    gpio_set_level(s_cfg.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(s_cfg.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_cfg.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* Data entry X+ Y-, the window over the whole panel, the cursor at its
 * first byte. The cursor is where the next RAM write starts. */
static void addressing(void)
{
    int xe = s_cfg.width / 8 - 1, ys = s_cfg.height - 1;
    CMD(0x11, 0x01);
    CMD(0x44, 0x00, (uint8_t)xe);
    CMD(0x45, (uint8_t)ys, (uint8_t)(ys >> 8), 0x00, 0x00);
    CMD(0x4E, 0x00);
    CMD(0x4F, (uint8_t)ys, (uint8_t)(ys >> 8));
}

static void lut(const uint8_t *l)
{
    cmd_data(0x32, l, 153);
    wait_busy(1000, "lut");
    CMD(0x3F, l[153]);
    CMD(0x03, l[154]);
    CMD(0x04, l[155], l[156], l[157]);
    CMD(0x2C, l[158]);
}

static bool init_full(void)
{
    hw_reset();
    if (!wait_busy(2000, "reset")) return false;
    cmd(0x12);                                  /* software reset */
    if (!wait_busy(2000, "software reset")) return false;
    int g = s_cfg.height - 1;
    CMD(0x01, (uint8_t)g, (uint8_t)(g >> 8), 0x01);   /* driver output */
    addressing();
    CMD(0x3C, 0x01);                            /* border waveform */
    CMD(0x18, 0x80);                            /* internal temperature sensor */
    CMD(0x22, 0xB1);                            /* load temperature + waveform */
    cmd(0x20);
    if (!wait_busy(2000, "waveform load")) return false;
    lut(k_lut_full);
    return true;
}

static bool init_partial(void)
{
    hw_reset();
    if (!wait_busy(2000, "reset")) return false;
    lut(k_lut_partial);
    CMD(0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00);
    CMD(0x3C, 0x80);
    CMD(0x22, 0xC0);
    cmd(0x20);
    return wait_busy(2000, "partial init");
}

static void write_ram(uint8_t plane)
{
    addressing();
    cmd(plane);
    data(s_work, s_bytes);
}

/* ── The task ──────────────────────────────────────────────────────────── */

static void refresh_full(void)
{
    int64_t t0 = esp_timer_get_time();
    if (!init_full()) return;
    /* Both planes: 0x24 is the picture, 0x26 the "previous" picture the
     * partial waveform compares against. Starting them equal is what makes
     * the partials that follow draw only what changed. */
    write_ram(0x24);
    write_ram(0x26);
    CMD(0x22, 0xC7);
    cmd(0x20);
    bool ok = wait_busy(5000, "full refresh");
    init_partial();
    s_fulls++;
    s_refreshes++;
    ESP_LOGI(TAG, "full refresh %s in %lld ms (stack %u free)",
             ok ? "done" : "TIMED OUT", (esp_timer_get_time() - t0) / 1000,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void refresh_partial(void)
{
    int64_t t0 = esp_timer_get_time();
    write_ram(0x24);
    CMD(0x22, 0xCF);
    cmd(0x20);
    bool ok = wait_busy(3000, "partial refresh");
    s_refreshes++;
    ESP_LOGD(TAG, "partial refresh %s in %lld ms", ok ? "done" : "TIMED OUT",
             (esp_timer_get_time() - t0) / 1000);
    if (!ok) s_want_full = true;    /* start clean next time */
}

static void epd_task(void *arg)
{
    (void)arg;
    uint32_t partials = 0;
    int64_t last_full_us = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool have, full;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        have = s_have_next;
        if (have) memcpy(s_work, s_next, s_bytes);
        s_have_next = false;
        full = s_want_full;
        s_want_full = false;
        xSemaphoreGive(s_lock);
        if (!have) continue;

        int64_t now = esp_timer_get_time();
        if (partials >= EPD_FULL_EVERY ||
            now - last_full_us > (int64_t)EPD_FULL_AFTER_S * 1000000)
            full = true;
        if (full) {
            refresh_full();
            partials = 0;
            last_full_us = esp_timer_get_time();
        } else {
            refresh_partial();
            partials++;
        }
    }
}

/* ── Public ────────────────────────────────────────────────────────────── */

esp_err_t epd_init(const epd_cfg_t *cfg)
{
    if (!cfg || cfg->width % 8) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_bytes = (size_t)cfg->width / 8 * cfg->height;

    s_next = heap_caps_malloc(s_bytes, MALLOC_CAP_8BIT);
    s_work = heap_caps_malloc(s_bytes, MALLOC_CAP_DMA);
    s_lock = xSemaphoreCreateMutex();
    if (!s_next || !s_work || !s_lock) return ESP_ERR_NO_MEM;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << cfg->cs) | (1ULL << cfg->dc) | (1ULL << cfg->rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(cfg->cs, 1);
    gpio_set_level(cfg->rst, 1);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << cfg->busy,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    spi_bus_config_t bus = {
        .mosi_io_num = cfg->mosi,
        .miso_io_num = -1,
        .sclk_io_num = cfg->sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)s_bytes,
    };
    esp_err_t err = spi_bus_initialize(cfg->host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t dev = {
        /* 20 MHz: the legacy build ran 40, and the whole picture is 5 KB,
         * so the difference is a millisecond against a 300 ms refresh. */
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,     /* CS by hand: a command and its data share it */
        .queue_size = 1,
    };
    err = spi_bus_add_device(cfg->host, &dev, &s_spi);
    if (err != ESP_OK) return err;

    /* Core 1, like everything else here that blocks for milliseconds at a
     * time (docs/esp32.md, "The two processors"). */
    if (xTaskCreatePinnedToCore(epd_task, "epd", 3072, NULL, 3, &s_task, 1)
        != pdPASS) {
        ESP_LOGE(TAG, "panel task did not start");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "panel %dx%d ready", cfg->width, cfg->height);
    return ESP_OK;
}

void epd_show(const uint8_t *img)
{
    if (!s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_next, img, s_bytes);
    s_have_next = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void epd_request_full(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_want_full = true;
    xSemaphoreGive(s_lock);
}

uint32_t epd_refresh_count(uint32_t *full)
{
    if (full) *full = s_fulls;
    return s_refreshes;
}
