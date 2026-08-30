/**
 * @file bech32.h
 * @brief Bech32 encoding for NOSTR npub/nsec
 *
 * Implementation based on BIP-173 specification.
 */

#ifndef XPRS_BECH32_H
#define XPRS_BECH32_H

#include <stdint.h>
#include <stddef.h>
#if defined(ESP_PLATFORM)
#include "esp_err.h"
#else
/* bech32.c is pure C and wanted only for its return-code spelling. On a
 * target without the IDF (the SenseCAP P1-Pro) the three codes it uses are
 * given the IDF's values, so a caller reads them the same way everywhere. */
typedef int esp_err_t;
#define ESP_OK               0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM       0x101
#define ESP_ERR_INVALID_ARG  0x102
#define ESP_ERR_INVALID_CRC  0x109
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode data as bech32
 *
 * @param hrp Human-readable part (e.g., "npub", "nsec")
 * @param data Raw data bytes
 * @param data_len Length of data
 * @param output Output buffer for bech32 string
 * @param output_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t bech32_encode(const char *hrp, const uint8_t *data, size_t data_len,
                        char *output, size_t output_len);

/**
 * @brief Decode bech32 string
 *
 * @param input Bech32-encoded string
 * @param hrp Output buffer for human-readable part (at least 8 bytes)
 * @param data Output buffer for decoded data
 * @param data_len Input: size of data buffer, Output: actual data length
 * @return ESP_OK on success
 */
esp_err_t bech32_decode(const char *input, char *hrp, uint8_t *data, size_t *data_len);

#ifdef __cplusplus
}
#endif

#endif // XPRS_BECH32_H
