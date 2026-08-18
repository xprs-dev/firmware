/**
 * @file ili9342.c
 * @brief ILI9342C TFT LCD driver for the M5Stack Core (ESP-IDF, SPI).
 *
 * Modeled on the T-Dongle's st7735.c: polling SPI transactions, RGB565
 * big-endian, flush-a-window API for an LVGL layer above. The panel is
 * natively 320x240 landscape, so no rotation gymnastics are needed --
 * MADCTL sets BGR order and nothing else.
 */

#include "ili9342.h"
#include <string.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ili9342";

/* One SPI transaction is capped by the bus max_transfer_sz; flush in slices
 * that stay under it. 320 px * 48 rows * 2 B = 30720 B per slice. */
#define MAX_SLICE_ROWS 48

struct ili9342_dev {
    spi_device_handle_t spi;
    int dc_pin;
    int rst_pin;
    int bl_pin;
};

/* ---- low-level helpers -------------------------------------------------- */

static void lcd_cmd(ili9342_handle_t h, uint8_t cmd)
{
    gpio_set_level(h->dc_pin, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(h->spi, &t);
}

static void lcd_data(ili9342_handle_t h, const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(h->dc_pin, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(h->spi, &t);
}

static void lcd_cmd_data(ili9342_handle_t h, uint8_t cmd,
                         const uint8_t *data, size_t len)
{
    lcd_cmd(h, cmd);
    lcd_data(h, data, len);
}

static void set_window(ili9342_handle_t h,
                       uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t col[4] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    uint8_t row[4] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    lcd_cmd_data(h, 0x2A, col, 4);   /* CASET */
    lcd_cmd_data(h, 0x2B, row, 4);   /* RASET */
    lcd_cmd(h, 0x2C);                /* RAMWR */
}

/* ---- init sequence ------------------------------------------------------ */

static void hw_reset(ili9342_handle_t h)
{
    if (h->rst_pin < 0) return;
    gpio_set_level(h->rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(h->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void init_sequence(ili9342_handle_t h)
{
    lcd_cmd(h, 0x01);                 /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Set EXTC: unlock the extended command set. */
    { uint8_t d[] = {0xFF, 0x93, 0x42}; lcd_cmd_data(h, 0xC8, d, 3); }

    /* Power controls. */
    { uint8_t d[] = {0x12, 0x12}; lcd_cmd_data(h, 0xC0, d, 2); }
    { uint8_t d[] = {0x03};       lcd_cmd_data(h, 0xC1, d, 1); }
    { uint8_t d[] = {0xF2};       lcd_cmd_data(h, 0xC5, d, 1); }   /* VCOM */

    /* Interface control. */
    { uint8_t d[] = {0xE0};             lcd_cmd_data(h, 0xB0, d, 1); }
    { uint8_t d[] = {0x01, 0x00, 0x00}; lcd_cmd_data(h, 0xF6, d, 3); }

    /* Gamma. */
    { uint8_t d[] = {0x00,0x0C,0x11,0x04,0x11,0x08,0x37,0x89,
                     0x4C,0x06,0x0C,0x0A,0x2E,0x34,0x0F};
      lcd_cmd_data(h, 0xE0, d, 15); }
    { uint8_t d[] = {0x00,0x0B,0x11,0x05,0x13,0x09,0x33,0x67,
                     0x48,0x07,0x0E,0x0B,0x2E,0x33,0x0F};
      lcd_cmd_data(h, 0xE1, d, 15); }

    /* Memory access: BGR order; the panel is natively landscape 320x240. */
    { uint8_t d[] = {0x08}; lcd_cmd_data(h, 0x36, d, 1); }

    /* 16-bit colour. */
    { uint8_t d[] = {0x55}; lcd_cmd_data(h, 0x3A, d, 1); }

    /* Current M5Stack Core panels want inversion on for true colours. */
    lcd_cmd(h, 0x21);                 /* INVON */

    lcd_cmd(h, 0x11);                 /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(h, 0x29);                 /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---- public API --------------------------------------------------------- */

esp_err_t ili9342_init(const ili9342_config_t *config, ili9342_handle_t *out)
{
    if (!config || !out) return ESP_ERR_INVALID_ARG;

    ili9342_handle_t h = calloc(1, sizeof(struct ili9342_dev));
    if (!h) return ESP_ERR_NO_MEM;
    h->dc_pin  = config->dc_pin;
    h->rst_pin = config->rst_pin;
    h->bl_pin  = config->bl_pin;

    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    io.pin_bit_mask = (1ULL << config->dc_pin);
    if (config->rst_pin >= 0) io.pin_bit_mask |= (1ULL << config->rst_pin);
    if (config->bl_pin >= 0)  io.pin_bit_mask |= (1ULL << config->bl_pin);
    gpio_config(&io);

    spi_bus_config_t buscfg = {
        .mosi_io_num = config->mosi_pin,
        .miso_io_num = config->miso_pin,
        .sclk_io_num = config->sclk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ILI9342_WIDTH * MAX_SLICE_ROWS * 2 + 16,
    };
    /* SPI3 (VSPI): 18/23/19 are its IOMUX pins on the original ESP32, which is
     * what permits 40 MHz -- on SPI2 these pins route through the GPIO matrix
     * and the driver refuses the clock (ESP_ERR_NOT_SUPPORTED). */
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        free(h);
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = config->cs_pin,
        .queue_size = 7,
    };
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &h->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        free(h);
        return ret;
    }

    hw_reset(h);
    init_sequence(h);

    if (h->bl_pin >= 0) gpio_set_level(h->bl_pin, 1);   /* active-high */

    ESP_LOGI(TAG, "ILI9342C initialised (%dx%d)", ILI9342_WIDTH, ILI9342_HEIGHT);
    *out = h;
    return ESP_OK;
}

esp_err_t ili9342_deinit(ili9342_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->bl_pin >= 0) gpio_set_level(handle->bl_pin, 0);
    spi_bus_remove_device(handle->spi);
    free(handle);
    return ESP_OK;
}

esp_err_t ili9342_flush(ili9342_handle_t handle,
                        uint16_t x1, uint16_t y1,
                        uint16_t x2, uint16_t y2,
                        const uint16_t *data)
{
    if (!handle || !data) return ESP_ERR_INVALID_ARG;

    set_window(handle, x1, y1, x2, y2);

    const uint16_t width = x2 - x1 + 1;
    uint32_t rows_left = y2 - y1 + 1;
    const uint16_t *px = data;
    gpio_set_level(handle->dc_pin, 1);
    while (rows_left) {
        uint32_t rows = rows_left > MAX_SLICE_ROWS ? MAX_SLICE_ROWS : rows_left;
        spi_transaction_t t = {
            .length = (size_t)width * rows * 16,
            .tx_buffer = px,
        };
        esp_err_t ret = spi_device_polling_transmit(handle->spi, &t);
        if (ret != ESP_OK) return ret;
        px += (size_t)width * rows;
        rows_left -= rows;
    }
    return ESP_OK;
}

esp_err_t ili9342_fill_color(ili9342_handle_t handle, uint16_t color)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    uint16_t c = (color >> 8) | (color << 8);
    uint16_t *line = malloc(ILI9342_WIDTH * 2);
    if (!line) return ESP_ERR_NO_MEM;
    for (int i = 0; i < ILI9342_WIDTH; i++) line[i] = c;

    set_window(handle, 0, 0, ILI9342_WIDTH - 1, ILI9342_HEIGHT - 1);
    gpio_set_level(handle->dc_pin, 1);
    for (int y = 0; y < ILI9342_HEIGHT; y++) {
        spi_transaction_t t = {
            .length = ILI9342_WIDTH * 16,
            .tx_buffer = line,
        };
        spi_device_polling_transmit(handle->spi, &t);
    }
    free(line);
    return ESP_OK;
}

void ili9342_backlight(ili9342_handle_t handle, bool on)
{
    if (!handle || handle->bl_pin < 0) return;
    gpio_set_level(handle->bl_pin, on ? 1 : 0);   /* active-high */
}

void ili9342_invert(ili9342_handle_t handle, bool on)
{
    if (!handle) return;
    lcd_cmd(handle, on ? 0x21 : 0x20);
}
