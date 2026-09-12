/*
 * Opening a sealed body, `x:` -- see xprsseal.h.
 *
 * AES comes from mbedtls on the device and OpenSSL in the host harness, and
 * the rest (base64url, the padding check) is shared, so the harness tests the
 * code the station runs rather than a second copy of it.
 */

#include "xprsseal.h"
#include "xprssig.h"

#include <string.h>
#include <stdbool.h>

#ifdef XPRSSIG_HOST_TEST
#include <openssl/evp.h>

static bool xs_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                           const uint8_t *in, size_t len, uint8_t *out)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n1 = 0, n2 = 0;
    bool ok = c &&
              EVP_DecryptInit_ex(c, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
              EVP_CIPHER_CTX_set_padding(c, 0) == 1 &&
              EVP_DecryptUpdate(c, out, &n1, in, (int)len) == 1 &&
              EVP_DecryptFinal_ex(c, out + n1, &n2) == 1 &&
              (size_t)(n1 + n2) == len;
    EVP_CIPHER_CTX_free(c);
    return ok;
}
#else
#include "mbedtls/aes.h"

static bool xs_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                           const uint8_t *in, size_t len, uint8_t *out)
{
    mbedtls_aes_context aes;
    uint8_t ivc[16];
    memcpy(ivc, iv, 16);            /* mbedtls advances the IV in place */
    mbedtls_aes_init(&aes);
    bool ok = mbedtls_aes_setkey_dec(&aes, key, 256) == 0 &&
              mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, len, ivc,
                                    in, out) == 0;
    mbedtls_aes_free(&aes);
    return ok;
}
#endif

/* base64url, no padding: 4 characters to 3 bytes, a tail of 2 or 3
 * characters to 1 or 2. A padded value, or one with a character outside the
 * alphabet, is not what 6.2 says `x:` is, and is refused. */
static int b64u_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int b64u_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    if (len % 4 == 1) return -1;
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64u_val(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

int xprsseal_open(const uint8_t my_priv[32], const uint8_t peer_x[32],
                  const char *x, size_t xlen, uint8_t *out, size_t cap)
{
    uint8_t blob[XPRSSEAL_X_MAX * 3 / 4];
    uint8_t key[32];
    int rc = -1;
    size_t ctlen = 0;

    if (!my_priv || !peer_x || !x || !out || xlen > XPRSSEAL_X_MAX) return -1;
    /* Every byte of x is read here, before anything is written to out: a
     * caller may pass the same buffer as both (xprsseal.h). */
    int n = b64u_decode(x, xlen, blob, sizeof blob);
    /* An IV and at least one block, and whole blocks after it. */
    if (n < 32 || (n - 16) % 16 != 0) goto done;
    ctlen = (size_t)n - 16;
    if (cap < ctlen) goto done;

    if (!xprssig_ecdh_x(my_priv, peer_x, key)) goto done;
    if (!xs_cbc_decrypt(key, blob, blob + 16, ctlen, out)) goto done;

    /* PKCS#7, strictly: the last byte says how many bytes of padding there
     * are, 1 to 16, and every one of them says the same. A body sealed to a
     * different key decrypts to noise, and this is where it is caught. */
    uint8_t pad = out[ctlen - 1];
    if (pad < 1 || pad > 16) goto done;
    for (size_t i = ctlen - pad; i < ctlen; i++)
        if (out[i] != pad) goto done;
    size_t plen = ctlen - pad;
    if (plen >= cap) goto done;     /* room for the NUL */
    memset(out + plen, 0, ctlen - plen);
    out[plen] = 0;
    rc = (int)plen;
done:
    memset(key, 0, sizeof key);
    memset(blob, 0, sizeof blob);
    if (rc < 0 && ctlen) memset(out, 0, cap < ctlen ? cap : ctlen);
    return rc;
}
