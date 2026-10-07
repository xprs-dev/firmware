/* Raw flash pages on an nRF52840 (nrf_flash.h). */
#include <Arduino.h>

#include "nrf_flash.h"

extern "C" {
#include "tinynimble.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
}

static volatile int s_flash_evt;

extern "C" void tn_soc_event(uint32_t evt)
{
    if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) s_flash_evt = 1;
    else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) s_flash_evt = -1;
}

static bool sd_on(void)
{
    uint8_t on = 0;
    sd_softdevice_is_enabled(&on);
    return on != 0;
}

static bool flash_wait(void)
{
    /* SoC events only: the flash-done event we are waiting for arrives here,
     * and draining the BLE queue instead (tn_gatt_pump) would reenter gatt_rx
     * from inside the chunk we are still writing -- the hang that a fast image
     * push produced (docs/ble5-gatt.md, tn_soc_pump). */
    for (uint32_t t0 = millis(); millis() - t0 < 3000; ) {
        tn_soc_pump();
        if (s_flash_evt) return s_flash_evt > 0;
        delay(1);
    }
    return false;
}

bool nrf_flash_erase(uint32_t addr)
{
    if (!sd_on()) {
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
        while (!NRF_NVMC->READY) { }
        NRF_NVMC->ERASEPAGE = addr;
        while (!NRF_NVMC->READY) { }
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
        return true;
    }
    s_flash_evt = 0;
    uint32_t err;
    while ((err = sd_flash_page_erase(addr / NRF_FLASH_PAGE)) == NRF_ERROR_BUSY) { tn_soc_pump(); delay(1); }
    return err == NRF_SUCCESS && flash_wait();
}

bool nrf_flash_write(uint32_t addr, const uint32_t *src, uint32_t n)
{
    if (!sd_on()) {
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
        for (uint32_t i = 0; i < n; i++) {
            ((volatile uint32_t *)addr)[i] = src[i];
            while (!NRF_NVMC->READY) { }
        }
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
        return true;
    }
    s_flash_evt = 0;
    uint32_t err;
    while ((err = sd_flash_write((uint32_t *)addr, src, n)) == NRF_ERROR_BUSY) { tn_soc_pump(); delay(1); }
    return err == NRF_SUCCESS && flash_wait();
}
