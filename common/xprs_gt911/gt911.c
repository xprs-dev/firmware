/**
 * @file gt911.c
 * @brief GT911 polled driver (see gt911.h).
 */

#include "gt911.h"
#include "esp_log.h"

static const char *TAG = "gt911";

#define REG_STATUS   0x814E   /* bit7: buffer ready; low nibble: touch count */
#define REG_POINT0   0x8150   /* track id, x lo, x hi, y lo, y hi, size lo/hi, res */
#define REG_PRODUCT  0x8140   /* "911" in ASCII */
#define REG_CFG_MAXX 0x8048   /* config: x max lo/hi, y max lo/hi */

/* GT911 registers are 16-bit and big-endian on the wire; the i2c_bsp helpers
 * take an 8-bit register, so the address travels in the write buffer. */
static esp_err_t rd(i2c_dev_handle_t d, uint16_t reg, uint8_t *buf, uint8_t n)
{
    uint8_t a[2] = { reg >> 8, reg & 0xFF };
    return i2c_write_read(d, a, 2, buf, n);
}

static esp_err_t wr8(i2c_dev_handle_t d, uint16_t reg, uint8_t v)
{
    uint8_t a[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_write_bytes(d, -1, a, 3);
}

esp_err_t gt911_init(gt911_t *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    *t = (gt911_t){0};
    static const uint8_t addrs[2] = { 0x5D, 0x14 };
    for (int i = 0; i < 2; i++) {
        i2c_dev_handle_t h = NULL;
        if (i2c_bus_add_device(addrs[i], &h) != ESP_OK) continue;
        uint8_t id[4] = {0};
        if (rd(h, REG_PRODUCT, id, 4) == ESP_OK && id[0] == '9' && id[1] == '1') {
            t->dev = h;
            t->addr = addrs[i];
            uint8_t c[4] = {0};
            if (rd(h, REG_CFG_MAXX, c, 4) == ESP_OK) {
                t->max_x = c[0] | (c[1] << 8);
                t->max_y = c[2] | (c[3] << 8);
            }
            ESP_LOGI(TAG, "GT911 at 0x%02X, id %c%c%c, range %ux%u",
                     addrs[i], id[0], id[1], id[2], t->max_x, t->max_y);
            wr8(h, REG_STATUS, 0);           /* discard anything stale */
            return ESP_OK;
        }
        i2c_bus_remove_device(h);
    }
    ESP_LOGW(TAG, "no GT911 at 0x5D or 0x14 -- this board has no touch");
    return ESP_ERR_NOT_FOUND;
}

bool gt911_read(gt911_t *t, uint16_t *x, uint16_t *y)
{
    if (!t || !t->dev) return false;
    uint8_t st = 0;
    if (rd(t->dev, REG_STATUS, &st, 1) != ESP_OK) return false;
    /* Bit 7 is only set when the controller has something NEW. Between
     * updates a held finger still reports the last point, so the cached
     * state is the answer and nothing else is read. */
    if (st & 0x80) {
        int n = st & 0x0F;
        if (n > 0) {
            uint8_t p[8] = {0};
            if (rd(t->dev, REG_POINT0, p, 8) == ESP_OK) {
                t->x = p[1] | (p[2] << 8);
                t->y = p[3] | (p[4] << 8);
                t->down = true;
            }
        } else {
            t->down = false;
        }
        wr8(t->dev, REG_STATUS, 0);          /* acknowledge, or it stalls */
    }
    if (x) *x = t->x;
    if (y) *y = t->y;
    return t->down;
}
