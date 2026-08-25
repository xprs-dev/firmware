/**
 * @file st7789.c
 * @brief ST7789 TFT LCD driver (ESP-IDF, SPI).
 *
 * Modeled on the M5Stack's ili9342.c, which was modeled on the T-Dongle's
 * st7735.c: polling SPI transactions, RGB565 big-endian, flush-a-window for an
 * LVGL layer above. What differs is the init sequence (ST7789 has its own
 * porch, power and gamma registers and no EXTC unlock) and the fact that the
 * bus may already belong to somebody else.
 */

#include "st7789.h"
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st7789";

/* One SPI transaction is capped by the bus max_transfer_sz; flush in slices
 * that stay under it. 320 px * 48 rows * 2 B = 30720 B per slice. */
#define MAX_SLICE_ROWS 48

struct st7789_dev {
    spi_device_handle_t spi;
    int dc_pin;
    int rst_pin;
    int bl_pin;
};

/* ---- low-level helpers -------------------------------------------------- */

static void lcd_cmd(st7789_handle_t h, uint8_t cmd)
{
    gpio_set_level(h->dc_pin, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(h->spi, &t);
}

static void lcd_data(st7789_handle_t h, const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(h->dc_pin, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(h->spi, &t);
}

static void lcd_cmd_data(st7789_handle_t h, uint8_t cmd,
                         const uint8_t *data, size_t len)
{
    lcd_cmd(h, cmd);
    lcd_data(h, data, len);
}

static void set_window(st7789_handle_t h,
                       uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t col[4] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    uint8_t row[4] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    lcd_cmd_data(h, 0x2A, col, 4);   /* CASET */
    lcd_cmd_data(h, 0x2B, row, 4);   /* RASET */
    lcd_cmd(h, 0x2C);                /* RAMWR */
}

/* ---- init sequence ------------------------------------------------------ */

static void hw_reset(st7789_handle_t h)
{
    if (h->rst_pin < 0) return;
    gpio_set_level(h->rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(h->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void init_sequence(st7789_handle_t h)
{
    lcd_cmd(h, 0x01);                 /* SWRESET -- the only reset a panel
                                       * with no reset line ever gets */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(h, 0x11);                 /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(h, 0x13);                 /* NORON: normal display mode */

    /* Memory access. MV|MX turns the native 240x320 portrait panel into the
     * 320x240 landscape the UI is drawn for; the RGB bit stays clear because
     * the ST7789 is RGB-ordered where the ILI9342C was BGR. */
    { uint8_t d[] = {0x60}; lcd_cmd_data(h, 0x36, d, 1); }

    /* 16-bit colour. */
    { uint8_t d[] = {0x55}; lcd_cmd_data(h, 0x3A, d, 1); }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ── The panel's own numbers, not the ST7789's generic ones ──────────
     *
     * Everything from here to DISPON is the JLX240 panel datasheet as
     * LilyGO ships it (TFT_eSPI Setup210_LilyGo_T_Deck.h -> INIT_SEQUENCE_2),
     * and the difference is not subtle. Generic ST7789 gamma on this glass
     * crushes the midtones, and the midtones are exactly where anti-aliasing
     * lives: the half-lit pixels along a letter's diagonal or a radar circle
     * get snapped to background or foreground, so a correctly anti-aliased
     * frame arrives at the eye looking jagged. The pixels were always right
     * -- a framedump of the same screen is smooth -- the transfer curve
     * carrying them was not.
     *
     * Another ST7789 board may well want its own tables; this is where they
     * would become a config field rather than a constant.
     */

    /* Frame rate / porch. */
    { uint8_t d[] = {0x0C,0x0C,0x00,0x33,0x33}; lcd_cmd_data(h, 0xB2, d, 5); }
    /* Gate control: VGH/VGL. 0x75, not the generic 0x35. */
    { uint8_t d[] = {0x75};                     lcd_cmd_data(h, 0xB7, d, 1); }
    /* VCOM. */
    { uint8_t d[] = {0x1A};                     lcd_cmd_data(h, 0xBB, d, 1); }
    /* LCM control. */
    { uint8_t d[] = {0x2C};                     lcd_cmd_data(h, 0xC0, d, 1); }
    /* VDV/VRH enable, VRH, VDV. */
    { uint8_t d[] = {0x01};                     lcd_cmd_data(h, 0xC2, d, 1); }
    { uint8_t d[] = {0x13};                     lcd_cmd_data(h, 0xC3, d, 1); }
    { uint8_t d[] = {0x20};                     lcd_cmd_data(h, 0xC4, d, 1); }
    /* Frame rate control in normal mode. */
    { uint8_t d[] = {0x0F};                     lcd_cmd_data(h, 0xC6, d, 1); }
    /* Power control. */
    { uint8_t d[] = {0xA4,0xA1};                lcd_cmd_data(h, 0xD0, d, 2); }

    /* Gamma, positive and negative. */
    { uint8_t d[] = {0xD0,0x0D,0x14,0x0D,0x0D,0x09,0x38,
                     0x44,0x4E,0x3A,0x17,0x18,0x2F,0x30};
      lcd_cmd_data(h, 0xE0, d, 14); }
    { uint8_t d[] = {0xD0,0x09,0x0F,0x08,0x07,0x14,0x37,
                     0x44,0x4D,0x38,0x15,0x16,0x2C,0x3E};
      lcd_cmd_data(h, 0xE1, d, 14); }

    lcd_cmd(h, 0x21);                 /* INVON -- see st7789_invert() */

    lcd_cmd(h, 0x29);                 /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---- public API --------------------------------------------------------- */

esp_err_t st7789_init(const st7789_config_t *config, st7789_handle_t *out)
{
    if (!config || !out) return ESP_ERR_INVALID_ARG;

    st7789_handle_t h = calloc(1, sizeof(struct st7789_dev));
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
        .max_transfer_sz = ST7789_WIDTH * MAX_SLICE_ROWS * 2 + 16,
    };
    /* INVALID_STATE means somebody already set this bus up -- on a board that
     * shares one bus between the panel, the card and the radio that is the
     * normal case, not an error. */
    esp_err_t ret = spi_bus_initialize(config->host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        free(h);
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = config->clock_hz > 0 ? config->clock_hz
                                               : 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = config->cs_pin,
        .queue_size = 7,
    };
    ret = spi_bus_add_device(config->host, &devcfg, &h->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        free(h);
        return ret;
    }

    hw_reset(h);
    init_sequence(h);

    if (h->bl_pin >= 0) gpio_set_level(h->bl_pin, 1);   /* active-high */

    ESP_LOGI(TAG, "ST7789 initialised (%dx%d @ %d MHz)",
             ST7789_WIDTH, ST7789_HEIGHT, devcfg.clock_speed_hz / 1000000);
    *out = h;
    return ESP_OK;
}

esp_err_t st7789_deinit(st7789_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->bl_pin >= 0) gpio_set_level(handle->bl_pin, 0);
    spi_bus_remove_device(handle->spi);
    free(handle);
    return ESP_OK;
}

esp_err_t st7789_flush(st7789_handle_t handle,
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

esp_err_t st7789_fill_color(st7789_handle_t handle, uint16_t color)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    uint16_t c = (color >> 8) | (color << 8);
    uint16_t *line = malloc(ST7789_WIDTH * 2);
    if (!line) return ESP_ERR_NO_MEM;
    for (int i = 0; i < ST7789_WIDTH; i++) line[i] = c;

    set_window(handle, 0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);
    gpio_set_level(handle->dc_pin, 1);
    for (int y = 0; y < ST7789_HEIGHT; y++) {
        spi_transaction_t t = {
            .length = ST7789_WIDTH * 16,
            .tx_buffer = line,
        };
        spi_device_polling_transmit(handle->spi, &t);
    }
    free(line);
    return ESP_OK;
}

void st7789_sleep(st7789_handle_t handle, bool sleep)
{
    if (!handle) return;
    if (sleep) {
        lcd_cmd(handle, 0x28);                    /* DISPOFF */
        lcd_cmd(handle, 0x10);                    /* SLPIN   */
        vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        lcd_cmd(handle, 0x11);                    /* SLPOUT  */
        vTaskDelay(pdMS_TO_TICKS(120));           /* datasheet: 120 ms before anything */
        lcd_cmd(handle, 0x29);                    /* DISPON  */
    }
}

void st7789_backlight(st7789_handle_t handle, bool on)
{
    if (!handle || handle->bl_pin < 0) return;
    gpio_set_level(handle->bl_pin, on ? 1 : 0);   /* active-high */
}

void st7789_invert(st7789_handle_t handle, bool on)
{
    if (!handle) return;
    lcd_cmd(handle, on ? 0x21 : 0x20);
}
