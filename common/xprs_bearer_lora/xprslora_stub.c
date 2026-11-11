/*
 * xprslora_stub.c -- the LoRa bearer on a board built without LoRa
 * (CONFIG_XPRS_LORA=n, docs/esp32.md "A board without LoRa need not carry
 * it"). Every public function of xprslora.h, doing nothing: the radio is
 * never active, a mode is never available, a start is ESP_ERR_NOT_SUPPORTED.
 * Generated from the header's prototypes, so it matches them; nothing here
 * references the SX1262, Meshtastic or MeshCore code, so none of it is
 * linked (about 44 KB on the e-paper board).
 */
#include <stddef.h>
#include "xprslora.h"

const char *xprslora_mode_name(xprslora_mode_t mode)
{
    (void)mode; return "none";
}

bool xprslora_mode_parse(const char *word, xprslora_mode_t *out)
{
    return false;
}

bool xprslora_mode_available(xprslora_mode_t mode)
{
    return false;
}

esp_err_t xprslora_set_both(uint32_t freq_hz, uint8_t sf, uint16_t bw_khz, uint8_t sync, uint16_t preamble)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool xprslora_both_channel(uint32_t *freq_hz, uint8_t *sf, uint16_t *bw_khz, uint8_t *sync, uint16_t *preamble)
{
    return false;
}

const char *xprslora_both_why_not(void)
{
    return "this board has no LoRa radio";
}

bool xprslora_class_stats(xprslora_class_stats_t *out)
{
    return false;
}

const xprslora_region_t *xprslora_regions(xprslora_mode_t mode, int *count)
{
    return NULL;
}

xprslora_mode_t xprslora_mode(void)
{
    return (xprslora_mode_t)0;
}

esp_err_t xprslora_set_mode(xprslora_mode_t mode)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t xprslora_survey_start(uint32_t per_mode_s)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t xprslora_detect_start(uint32_t per_mode_s)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool xprslora_survey_active(void)
{
    return false;
}

int xprslora_survey_json(char *buf, size_t cap)
{
    return 0;
}

esp_err_t xprslora_rotate_start(const xprslora_mode_t *modes, int n, uint32_t slice_s)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void xprslora_rotate_stop(void)
{
}

bool xprslora_rotate_cleared(void)
{
    return false;
}

bool xprslora_rotate_state(xprslora_rotate_t *out)
{
    return false;
}

esp_err_t xprslora_start(const char *callsign, const xprslora_cfg_t *cfg)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void xprslora_set_rx_cb(xprslora_rx_cb_t cb)
{
}

bool xprslora_send(const char *wire, int len)
{
    return false;
}

void xprslora_offer(const char *wire, int len)
{
}

void xprslora_digipeat(const char *wire, int len)
{
}

void xprslora_echo(const char *wire, int len)
{
}

uint32_t xprslora_idle_ms(uint32_t now_ms)
{
    return 0;
}

bool xprslora_is_active(void)
{
    return false;
}

void xprslora_set_pace(uint32_t per_packet_ms)
{
}

uint32_t xprslora_owed_ms(void)
{
    return 0;
}

uint32_t xprslora_airtime_ms(int len)
{
    return 0;
}

void xprslora_set_duty(uint32_t budget_ms, uint32_t reserve_ms, uint32_t dwell_ms)
{
}

void xprslora_duty(xb_duty_report_t *out)
{
}

esp_err_t xprslora_set_freq(uint32_t hz)
{
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t xprslora_freq(void)
{
    return 0;
}

esp_err_t xprslora_set_region(const char *name)
{
    return ESP_ERR_NOT_SUPPORTED;
}

const xprslora_region_t *xprslora_region(void)
{
    return NULL;
}

void xprslora_modem(int *sf, uint32_t *bw_hz)
{
}

void xprslora_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled, uint32_t *dupes)
{
}

esp_err_t xprslora_mt_start(const xprslora_mt_hooks_t *hooks, const mt_mesh_cfg_t *cfg, const char *nick)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void xprslora_mt_offer(const char *wire, int len, int origin)
{
}

bool xprslora_mt_stats(mt_mesh_stats_t *out)
{
    return false;
}

int xprslora_mt_node(int i, mt_node_t *out)
{
    return 0;
}

void xprslora_mt_set_nick(const char *nick)
{
}

esp_err_t xprslora_mc_start(const xprslora_mt_hooks_t *hooks, const mc_mesh_cfg_t *cfg, const char *nick)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void xprslora_mc_offer(const char *wire, int len, int origin)
{
}

bool xprslora_mc_stats(mc_mesh_stats_t *out)
{
    return false;
}

int xprslora_mc_node(int i, mc_node_t *out)
{
    return 0;
}

void xprslora_mc_set_nick(const char *nick)
{
}

void xprslora_mc_pause(bool quiet)
{
}
