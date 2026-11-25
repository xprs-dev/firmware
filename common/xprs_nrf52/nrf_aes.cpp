/*
 * AES for the LoRa networks (common/xprs_loracrypto/xlc.h) on an nRF52840.
 *
 * Meshtastic's default channel is AES-128-CTR under a published key, and its
 * direct messages are AES-CCM: both need the block cipher in the encrypt
 * direction only. The nRF52840's ECB block does AES-128 in hardware; through
 * the SoftDevice once it is up, directly before. 256-bit keys and the decrypt
 * direction (MeshCore direct messages) are not done here: an nRF52 station
 * relays those, it does not open them, so it says no rather than pretend.
 *
 * Moved here unchanged from the T1000-E's main.cpp when the P1-Pro became the
 * second nRF52 board on Meshtastic's channel (2026-11-25).
 */
#include <Arduino.h>
#include <string.h>

extern "C" {
#include "nrf_sdm.h"
#include "nrf_soc.h"
}

extern "C" bool xlc_aes_encrypt_block(const uint8_t *key, int key_len,
                                      const uint8_t in[16], uint8_t out[16])
{
    if (key_len != 16) return false;
    nrf_ecb_hal_data_t e;
    memcpy(e.key, key, 16);
    memcpy(e.cleartext, in, 16);
    uint8_t sd_on = 0;
    sd_softdevice_is_enabled(&sd_on);
    if (sd_on) {
        if (sd_ecb_block_encrypt(&e) != NRF_SUCCESS) return false;
    } else {
        NRF_ECB->ECBDATAPTR = (uint32_t)&e;
        NRF_ECB->EVENTS_ENDECB = 0;
        NRF_ECB->EVENTS_ERRORECB = 0;
        NRF_ECB->TASKS_STARTECB = 1;
        while (!NRF_ECB->EVENTS_ENDECB && !NRF_ECB->EVENTS_ERRORECB) { }
        if (NRF_ECB->EVENTS_ERRORECB) return false;
    }
    memcpy(out, e.ciphertext, 16);
    return true;
}

extern "C" bool xlc_aes_decrypt_block(const uint8_t *key, int key_len,
                                      const uint8_t in[16], uint8_t out[16])
{
    (void)key; (void)key_len; (void)in; (void)out;
    return false;
}
