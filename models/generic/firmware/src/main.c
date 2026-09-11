/*
 * An XPRS station on a plain ESP32 devkit.
 *
 * For anyone with an ESP32 board in a drawer: the ESP32-DevKitC, a DOIT
 * DevKit V1, any WROOM-32 board with 4 MB of flash. The station is
 * common/xprs_app, the same program every other board runs: ESP-NOW and the
 * LAN as bearers, the digipeater and the bridge between them, the HTTP API,
 * and the walk-up hotspot with the chat page. What is here is the board, and
 * on purpose it is nothing: no pin is driven or read, so the image cannot
 * short or confuse whatever a particular board has wired to them.
 *
 * WHAT THIS CHIP CANNOT DO. The original ESP32 has Bluetooth 4.2, whose
 * 31-byte advertising cannot carry an XPRS beacon (docs/esp32.md, "Radio
 * capability per chip"), so there is no BLE bearer; the M5Stack Core, the
 * same chip, runs without one too. No PSRAM: every buffer is internal RAM.
 * 4 MB of flash: two app slots leave 832 KB for files, which is less than
 * one archive segment (see storage_mount below).
 */

#include <stdbool.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "xprs_app.h"

#include "fw_secrets.h"     /* gitignored; see fw_secrets.h.example   */
#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */

static const char *TAG = "generic";

/* The station's files at /idx (xapp_board_t.storage_mount): the log, the
 * statistics and the conversation, on the 832 KB "storage" partition, and
 * NO archive. An index segment is 4096 records of 320 bytes, 1.3 MB, and
 * the segment being written can never be evicted, so an archive here would
 * fill the volume and take the log down with it. */
static esp_err_t storage_mount(uint64_t *archive_bytes)
{
    static wl_handle_t wl = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t mc = {
        .max_files = CONFIG_SDCARD_MAX_FILES,
        .format_if_mount_failed = true,     /* our own partition */
        .allocation_unit_size = 4096,
    };
    *archive_bytes = 0;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/idx", "storage", &mc, &wl);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "storage did not mount: %s", esp_err_to_name(err));
    return err;
}

static const xapp_board_t k_board = {
    .board_id = "generic",
    .banner   = "ESP-NOW + LAN, headless, no BLE5 on this chip",

    .fw_key     = FW_DEFAULT_KEY,
    .fw_owner   = FW_DEFAULT_OWNER,
    .script_key = NULL,         /* falls back to fw_key; no panels shipped */

    .wifi_ssid = WIFI_SSID,
    .wifi_pass = WIFI_PASS,
    .espnow_channel = ESPNOW_FALLBACK_CHANNEL,

    /* No screen: the station links xprs_ui_none and still runs its console
     * task (xprs_app.h), so `cfg set ssid ...` works over the USB cable. */
    .display_init = NULL,
    .flush        = NULL,
    .screen_power = NULL,

    .input_init = NULL,
    .input_poll = NULL,
    .raw_key    = NULL,
    .touch_read = NULL,
    .kb_backlight = NULL,
    .battery_mv = NULL,
    .lora = NULL,

    .ble = false,

    /* The walk-up hotspot with the chat page: with no screen and no
     * keyboard, a phone on this access point is how a person uses the
     * station at all, and with no network configured it is the first thing
     * a freshly flashed board offers. */
    .hotspot = true,

    .storage_mount = storage_mount,
};

void app_main(void)
{
    xapp_run(&k_board);
}
