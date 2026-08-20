/*
 * An XPRS station on an M5Stack Core, whose whole job is to be somebody else.
 *
 * Testing a bearer with one device proves nothing: a station that hears its own
 * broadcast has learned that the loopback works. This is the second voice — it
 * beacons on ESP-NOW, repeats what it hears, and prints every packet with the
 * signal it arrived at, so the two ends can be compared.
 *
 * WHAT THIS BOARD CANNOT DO. It is an original ESP32 (D0WDQ6): no BLE5 extended
 * advertising, so it can never join the mesh plane the T-Dongle runs on
 * (docs/esp32.md, "Radio capability per chip"). It has ESP-NOW and WiFi, which
 * is the entire point of it being here.
 *
 * The station itself lives in common/xprs_app and is shared with every other
 * board. What is left here is what makes this board this board: an ILI9342C on
 * the VSPI pins, and three buttons.
 */

#include "driver/gpio.h"
#include "esp_log.h"

#include "ili9342.h"
#include "xprs_app.h"

#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */

static const char *TAG = "m5board";

/* ── The screen ─────────────────────────────────────────────────────────── */

static ili9342_handle_t s_lcd;

static esp_err_t display_init(int *w, int *h, void **ctx)
{
    /* M5Stack Core LCD on VSPI-ish pins, backlight active-high. */
    ili9342_config_t cfg = {
        .mosi_pin = 23, .miso_pin = 19, .sclk_pin = 18,
        .cs_pin = 14, .dc_pin = 27, .rst_pin = 33, .bl_pin = 32,
    };
    esp_err_t err = ili9342_init(&cfg, &s_lcd);
    if (err != ESP_OK) return err;
    *w = ILI9342_WIDTH;
    *h = ILI9342_HEIGHT;
    *ctx = s_lcd;
    return ESP_OK;
}

static void display_flush(int x1, int y1, int x2, int y2,
                          const uint16_t *px, void *ctx)
{
    ili9342_flush((ili9342_handle_t)ctx, x1, y1, x2, y2, px);
}

/* ── The buttons ────────────────────────────────────────────────────────── */

#define BTN_A GPIO_NUM_39   /* short: next panel / OK.  held: home */
#define BTN_B GPIO_NUM_38   /* up   */
#define BTN_C GPIO_NUM_37   /* down */

/* Poll one active-low button; 3 agreeing samples = state, fire on the edge.
 * GPIO 37-39 are input-only with the board's own pull-ups. */
typedef struct { gpio_num_t pin; int low_count; bool armed; } btn_t;

static btn_t s_b = { BTN_B, 0, true };
static btn_t s_c = { BTN_C, 0, true };

static bool btn_pressed(btn_t *b)
{
    if (gpio_get_level(b->pin) == 0) {
        if (b->low_count < 3) b->low_count++;
        if (b->low_count == 3 && b->armed) {
            b->armed = false;
            return true;
        }
    } else {
        b->low_count = 0;
        b->armed = true;
    }
    return false;
}

static void input_init(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     /* 37-39 have no internal pulls */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << BTN_A) | (1ULL << BTN_B) | (1ULL << BTN_C),
    };
    gpio_config(&io);
}

/* Called every 10 ms. A is the only one that distinguishes a hold, so it is
 * timed here rather than debounced: 700 ms is ESC, anything shorter is the
 * ordinary press, and both fire on release so the two can never both go. */
static xapp_key_t input_poll(void)
{
    static int a_held_ms;
    static bool a_long_fired;

    if (gpio_get_level(BTN_A) == 0) {
        a_held_ms += 10;
        if (a_held_ms >= 700 && !a_long_fired) {
            a_long_fired = true;
            ESP_LOGI(TAG, "button A long");
            return XAPP_KEY_HOME;
        }
    } else {
        bool fire = a_held_ms >= 30 && !a_long_fired;
        a_held_ms = 0;
        a_long_fired = false;
        if (fire) {
            ESP_LOGI(TAG, "button A");
            return XAPP_KEY_NEXT;
        }
    }

    if (btn_pressed(&s_b)) return XAPP_KEY_UP;
    if (btn_pressed(&s_c)) return XAPP_KEY_DOWN;
    return XAPP_KEY_NONE;
}

/* ── This board ─────────────────────────────────────────────────────────── */

static const xapp_board_t k_m5stack = {
    .board_id = "m5stack-core",
    .banner = "ESP-NOW + LAN, no BLE5 on this chip",
    .wifi_ssid = WIFI_SSID,
    .wifi_pass = WIFI_PASS,
    .espnow_channel = ESPNOW_FALLBACK_CHANNEL,
    .display_init = display_init,
    .flush = display_flush,
    .input_init = input_init,
    .input_poll = input_poll,
};

void app_main(void)
{
    xapp_run(&k_m5stack);
}
