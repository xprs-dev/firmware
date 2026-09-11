/*
 * Sensirion SHTC3. Datasheet: wake 0x3517, measure T first with clock
 * stretching off 0x7866, sleep 0xB098, ID 0xEFC8; every 16-bit word is
 * followed by a CRC-8 (poly 0x31, init 0xFF).
 *
 * Mind the tick. These boards run FreeRTOS at 100 Hz, so pdMS_TO_TICKS()
 * rounds to tens of milliseconds: 1 ms is zero ticks and 15 ms is one tick,
 * which can be as little as a few milliseconds. The first version waited
 * exactly that and every measurement was a NACK (ESP_FAIL): the sensor was
 * still converting. The wake is a busy-wait; the conversion sleeps three
 * ticks, which is at least 20 ms against a 12.1 ms worst case.
 */

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "shtc3.h"

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static esp_err_t command(i2c_dev_handle_t dev, uint16_t c)
{
    uint8_t b[2] = { (uint8_t)(c >> 8), (uint8_t)c };
    return i2c_write_bytes(dev, -1, b, 2);
}

static esp_err_t wake(i2c_dev_handle_t dev)
{
    esp_err_t err = command(dev, 0x3517);
    esp_rom_delay_us(300);               /* 240 us to wake, at most */
    return err;
}

esp_err_t shtc3_probe(i2c_dev_handle_t dev, uint16_t *id)
{
    esp_err_t err = wake(dev);
    if (err != ESP_OK) return err;
    uint8_t c[2] = { 0xEF, 0xC8 }, r[3];
    err = i2c_write_read(dev, c, 2, r, 3);
    command(dev, 0xB098);
    if (err != ESP_OK) return err;
    if (crc8(r, 2) != r[2]) return ESP_ERR_INVALID_CRC;
    if (id) *id = (uint16_t)((r[0] << 8) | r[1]);
    return ESP_OK;
}

esp_err_t shtc3_measure(i2c_dev_handle_t dev, float *temp_c, float *rh_pct)
{
    esp_err_t err = wake(dev);
    if (err != ESP_OK) return err;
    err = command(dev, 0x7866);
    if (err != ESP_OK) { command(dev, 0xB098); return err; }
    vTaskDelay(pdMS_TO_TICKS(30));       /* 12.1 ms in normal mode */
    uint8_t r[6];
    err = i2c_read_bytes(dev, -1, r, 6);
    command(dev, 0xB098);
    if (err != ESP_OK) return err;
    if (crc8(r, 2) != r[2] || crc8(r + 3, 2) != r[5]) return ESP_ERR_INVALID_CRC;
    uint16_t rt = (uint16_t)((r[0] << 8) | r[1]);
    uint16_t rh = (uint16_t)((r[3] << 8) | r[4]);
    *temp_c = -45.0f + 175.0f * (float)rt / 65536.0f;
    *rh_pct = 100.0f * (float)rh / 65536.0f;
    return ESP_OK;
}
