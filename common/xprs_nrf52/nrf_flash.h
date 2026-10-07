/*
 * Raw flash pages on an nRF52840, whether or not the SoftDevice is up.
 *
 * With the SoftDevice running, flash is written THROUGH it: sd_flash_write
 * and sd_flash_page_erase are asynchronous and report through a SoC event,
 * which tinynimble's pump drains and hands to tn_soc_event(). This module
 * owns that event (one owner: the firmware update and the mail store both
 * write flash) and waits on it by pumping SoC events only. Before the
 * SoftDevice is up it drives the NVMC directly.
 *
 * Call from the station task only, never from a receive callback: an erase
 * takes ~85 ms and the wait pumps SoC events, not the BLE queue
 * (docs/ble5-gatt.md, tn_soc_pump).
 *
 * A word may be written twice between erases (nRF52840 nWRITE = 2), and a
 * write can only clear bits: that is what lets a record's status byte be
 * cleared in place after it was written.
 */
#pragma once
#include <stdint.h>

#define NRF_FLASH_PAGE 4096u

bool nrf_flash_erase(uint32_t addr);
bool nrf_flash_write(uint32_t addr, const uint32_t *src, uint32_t words);
