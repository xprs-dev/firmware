/* xlc_aes_idf.c -- xlc_aes_encrypt_block() on the ESP32, over mbedtls,
 * which the IDF backs with the AES peripheral (CONFIG_MBEDTLS_HARDWARE_AES).
 * Moved here from xprs_meshtastic/mt_aes_idf.c: both networks want it. */

#include "xlc.h"

#include "mbedtls/aes.h"

bool xlc_aes_encrypt_block(const uint8_t *key, int key_len,
                           const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = mbedtls_aes_setkey_enc(&ctx, key, (unsigned)key_len * 8u) == 0 &&
              mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out) == 0;
    mbedtls_aes_free(&ctx);
    return ok;
}

bool xlc_aes_decrypt_block(const uint8_t *key, int key_len,
                           const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = mbedtls_aes_setkey_dec(&ctx, key, (unsigned)key_len * 8u) == 0 &&
              mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in, out) == 0;
    mbedtls_aes_free(&ctx);
    return ok;
}
