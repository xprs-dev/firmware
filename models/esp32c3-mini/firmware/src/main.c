/*
 * An XPRS station on an ESP32-C3 mini board.
 *
 * The station is common/xprs_app, the same program every other board runs:
 * BLE5, ESP-NOW and the LAN as bearers, the digipeater and the bridge
 * between them, the HTTP API, and the walk-up hotspot with the chat page.
 * What is here is the board, and there is very little of it: no screen, no
 * battery, no card, one core.
 *
 * WHAT IS DIFFERENT ABOUT THIS CHIP. One core: everything docs/esp32.md
 * pins to core 1 goes wherever it can on this one (XPRS_WORK_CORE,
 * common/xprs_common/include/xprs_core.h). No PSRAM: every buffer is
 * internal RAM. 4 MB of flash: two app slots leave 832 KB for files, so the
 * archive is small and its segments are too (see storage_mount below).
 *
 * NOT USED: the LED on GPIO8 and the BOOT button on GPIO9. On a SuperMini
 * the LED is a plain one, on a DevKitM-1 it is a WS2812 on the same pin,
 * and this board has not been identified closely enough to drive either.
 */

#include <stdbool.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "xprs_app.h"

#include "fw_secrets.h"     /* gitignored; see fw_secrets.h.example   */
#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */

static const char *TAG = "c3mini";

/* The station's files at /idx (xapp_board_t.storage_mount): the log, the
 * statistics, the conversation and the archive, on the 832 KB "storage"
 * partition.
 *
 * The archive gets XAPP_ARCHIVE_832K, 384 KB, in 256-record segments
 * (xprsindex_seg_records sizes them to the budget), so the one being
 * written is at most ~80 KB and eviction gives space back in small steps. */
static esp_err_t storage_mount(uint64_t *archive_bytes)
{
    static wl_handle_t wl = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t mc = {
        .max_files = CONFIG_SDCARD_MAX_FILES,
        .format_if_mount_failed = true,     /* our own partition */
        .allocation_unit_size = 4096,
    };
    *archive_bytes = XAPP_ARCHIVE_832K;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/idx", "storage", &mc, &wl);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "storage did not mount: %s", esp_err_to_name(err));
    else
        xapp_storage_own_flash();
    return err;
}

static const xapp_board_t k_board = {
    .board_id = "esp32c3-mini",
    .banner   = "ESP-NOW + LAN + BLE5, headless, one core",

    .fw_key     = FW_DEFAULT_KEY,
    .fw_owner   = FW_DEFAULT_OWNER,
    .script_key = NULL,         /* falls back to fw_key; no panels shipped */

    .wifi_ssid = WIFI_SSID,
    .wifi_pass = WIFI_PASS,
    .espnow_channel = ESPNOW_FALLBACK_CHANNEL,

    /* No screen: the station links xprs_ui_none and still runs its console
     * task (xprs_app.h). */
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

    /* BLE 5 with extended advertising, like the S3. */
    .ble = true,

    /* The walk-up hotspot with the chat page: with no screen and no
     * keyboard, a phone on this access point is how a person uses the
     * station at all. */
    .hotspot = true,

    .storage_mount = storage_mount,

    /* WiFi and BLE share the one radio, and this board's WiFi link is weak
     * (-84 to -92 dBm on the bench): the long scan window cost web pages
     * seconds each (1.2 to 7.4 s) where the short one gives 0.3 s. */
    .ble_scan_light = true,
};

void app_main(void)
{
    xapp_run(&k_board);
}
