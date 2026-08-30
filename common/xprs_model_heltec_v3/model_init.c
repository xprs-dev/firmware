#include <stdio.h>
#include "model_init.h"
#include "model_config.h"
#include "i2c_bsp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "model_init";

// Device handles
static ssd1306_handle_t s_display = NULL;
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_adc_cali = NULL;
static adc_channel_t s_adc_ch;

// ============================================================================
// Vext power control
// ============================================================================

void model_vext_on(void)
{
    gpio_set_level(VEXT_PIN, VEXT_ON_LEVEL);
}

void model_vext_off(void)
{
    gpio_set_level(VEXT_PIN, VEXT_OFF_LEVEL);
}

// ============================================================================
// LED control (LEDC PWM)
// ============================================================================

void model_led_on(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void model_led_off(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void model_led_set_brightness(uint8_t brightness)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ============================================================================
// Battery voltage
// ============================================================================

float model_get_battery_voltage(void)
{
    if (!s_adc) return 0.0f;

    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, s_adc_ch, &raw) != ESP_OK) return 0.0f;
    if (s_adc_cali) {
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) != ESP_OK) return 0.0f;
    } else {
        mv = raw * 3100 / 4095;   /* 12 dB attenuation, uncalibrated */
    }
    /* Divider 390k / 100k -> the pin sees the cell over 4.9. */
    return (float)mv * BATTERY_ADC_SCALE / 1000.0f;
}

// ============================================================================
// Accessors
// ============================================================================

ssd1306_handle_t model_get_display(void)
{
    return s_display;
}

// ============================================================================
// Init / Deinit
// ============================================================================

esp_err_t model_init(void)
{
    ESP_LOGI(TAG, "Initializing %s (%s)", MODEL_NAME, MODEL_VARIANT);
    ESP_LOGI(TAG, "ESP32-S3 LX7 @ 240MHz, 512KB SRAM, 8MB Flash");

    esp_err_t ret;

    // 1. Initialize NVS (required for WiFi)
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "NVS initialized");

    // 2. Initialize Vext power control and turn ON (powers OLED + LoRa)
    gpio_config_t vext_conf = {
        .pin_bit_mask = (1ULL << VEXT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vext_conf);
    model_vext_on();
    vTaskDelay(pdMS_TO_TICKS(100));  // Wait for power stabilization
    ESP_LOGI(TAG, "Vext power ON (GPIO%d)", VEXT_PIN);

    // 3. Initialize I2C bus
    i2c_bus_config_t i2c_config = {
        .sda_pin = I2C_PIN_SDA,
        .scl_pin = I2C_PIN_SCL,
        .port = I2C_MASTER_PORT,
        .freq_hz = I2C_MASTER_FREQ_HZ,
    };
    ret = i2c_bus_init(&i2c_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d, %luHz)",
             I2C_PIN_SDA, I2C_PIN_SCL, (unsigned long)I2C_MASTER_FREQ_HZ);

    // 4. Initialize SSD1306 OLED display
    ssd1306_config_t oled_config = {
        .sda_pin = I2C_PIN_SDA,
        .scl_pin = I2C_PIN_SCL,
        .rst_pin = OLED_PIN_RST,
        .i2c_addr = I2C_ADDR_OLED,
    };
    ret = ssd1306_create(&oled_config, &s_display);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 create failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ssd1306_init(s_display);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "OLED display initialized (128x64, I2C 0x%02X)", I2C_ADDR_OLED);

    // 5. The SX1262 is NOT brought up here. xprs_bearer_lora creates and
    //    owns it from the pins in model_config.h; a driver here as well was
    //    two owners of SPI2 and one CS pin.

    // 6. Initialize LED (LEDC PWM)
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num = LED_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);
    ESP_LOGI(TAG, "LED initialized on GPIO%d (PWM)", LED_PIN);

    // 7. Battery ADC, oneshot driver with curve-fitting calibration where
    //    the chip has it (the T-Deck's pattern).
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BATTERY_ADC_PIN, &unit, &s_adc_ch) == ESP_OK) {
        adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
        if (adc_oneshot_new_unit(&uc, &s_adc) == ESP_OK) {
            adc_oneshot_chan_cfg_t cc = { .bitwidth = ADC_BITWIDTH_12,
                                          .atten = ADC_ATTEN_DB_12 };
            adc_oneshot_config_channel(s_adc, s_adc_ch, &cc);
            adc_cali_curve_fitting_config_t cf = { .unit_id = unit, .chan = s_adc_ch,
                .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
            if (adc_cali_create_scheme_curve_fitting(&cf, &s_adc_cali) != ESP_OK)
                s_adc_cali = NULL;
            ESP_LOGI(TAG, "Battery ADC on GPIO%d%s", BATTERY_ADC_PIN,
                     s_adc_cali ? "" : " (uncalibrated)");
        }
    }
    if (!s_adc) ESP_LOGW(TAG, "Battery ADC not available");

    ESP_LOGI(TAG, "Board initialization complete");
    return ESP_OK;
}

esp_err_t model_deinit(void)
{
    if (s_adc_cali) { adc_cali_delete_scheme_curve_fitting(s_adc_cali); s_adc_cali = NULL; }
    if (s_adc) { adc_oneshot_del_unit(s_adc); s_adc = NULL; }
    if (s_display) {
        ssd1306_delete(s_display);
        s_display = NULL;
    }
    i2c_bus_deinit();
    model_vext_off();
    ESP_LOGI(TAG, "Board deinitialization complete");
    return ESP_OK;
}
