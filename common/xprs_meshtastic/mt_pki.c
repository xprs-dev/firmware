/* mt_pki.c -- Meshtastic's direct-message encryption, and the keys XPRS
 * nodes wear on Meshtastic. See mt.h.
 *
 * Meshtastic 2.5 and later refuse to send a DM on the channel key and
 * reject one on receipt ("Rejecting legacy DM"): a DM is X25519 between the
 * two nodes' keys, SHA-256 of the shared secret as the AES-256 key, and
 * AES-CCM with an 8-byte tag and a 13-byte nonce (L = 2). What goes on the
 * air is the ciphertext, the tag, and a random 32-bit extra nonce.
 *
 * Written from RFC 3610 over xprs_loracrypto's AES hook, not taken from any
 * library, so it builds on the ESP32 and the nRF52 alike and is checked on
 * the host against Python's `cryptography` (test_mt_host.c). X25519 and the
 * callsign seed live in xprs_loracrypto now (xlc.h), because MeshCore wants
 * the same curve and the same rule.
 *
 * THE KEYS ARE NOT SECRET, by decision (docs/meshtastic.md): an XPRS
 * callsign's key pair is derived from the callsign, so every bridge that
 * relays for it presents the same key and can open a DM to it. That is the
 * privacy the public channel has -- none -- and the bridge turns the DM into
 * a clear XPRS message anyway. What it buys is that a Meshtastic user can
 * DM any XPRS user through any bridge.
 */

#include "mt.h"

#include <string.h>

#include "xlc.h"
#include "xprs.h"

/* ── AES-CCM (RFC 3610), M = 8, L = 2, 13-byte nonce ──────────────────── */

#define CCM_M 8
#define CCM_L 2

static bool ccm_tag(const uint8_t *key, int key_len, const uint8_t nonce[13],
                    const uint8_t *plain, int len, uint8_t tag[16])
{
    uint8_t b[16], x[16];
    /* B0: flags, nonce, length. No associated data. */
    b[0] = (uint8_t)((((CCM_M - 2) / 2) << 3) | (CCM_L - 1));
    memcpy(&b[1], nonce, 13);
    b[14] = (uint8_t)(len >> 8);
    b[15] = (uint8_t)len;
    if (!xlc_aes_encrypt_block(key, key_len, b, x)) return false;
    for (int off = 0; off < len; off += 16) {
        int n = len - off < 16 ? len - off : 16;
        for (int i = 0; i < n; i++) x[i] ^= plain[off + i];
        if (!xlc_aes_encrypt_block(key, key_len, x, x)) return false;
    }
    memcpy(tag, x, 16);
    return true;
}

/* CTR over [buf] with A_i = flags | nonce | i, i from 1; A_0 masks the tag. */
static bool ccm_ctr(const uint8_t *key, int key_len, const uint8_t nonce[13],
                    uint8_t *buf, int len, uint8_t tag[16])
{
    uint8_t a[16], s[16];
    a[0] = CCM_L - 1;
    memcpy(&a[1], nonce, 13);
    a[14] = a[15] = 0;
    if (!xlc_aes_encrypt_block(key, key_len, a, s)) return false;
    for (int i = 0; i < CCM_M; i++) tag[i] ^= s[i];
    uint16_t ctr = 1;
    for (int off = 0; off < len; off += 16, ctr++) {
        a[14] = (uint8_t)(ctr >> 8);
        a[15] = (uint8_t)ctr;
        if (!xlc_aes_encrypt_block(key, key_len, a, s)) return false;
        int n = len - off < 16 ? len - off : 16;
        for (int i = 0; i < n; i++) buf[off + i] ^= s[i];
    }
    return true;
}

bool mt_ccm_encrypt(const uint8_t *key, int key_len, const uint8_t nonce[13],
                    const uint8_t *plain, int len, uint8_t *out, uint8_t tag8[8])
{
    uint8_t tag[16];
    if (!ccm_tag(key, key_len, nonce, plain, len, tag)) return false;
    if (out != plain) memmove(out, plain, (size_t)len);
    if (!ccm_ctr(key, key_len, nonce, out, len, tag)) return false;
    memcpy(tag8, tag, CCM_M);
    return true;
}

bool mt_ccm_decrypt(const uint8_t *key, int key_len, const uint8_t nonce[13],
                    const uint8_t *crypt, int len, const uint8_t tag8[8],
                    uint8_t *out)
{
    uint8_t mask[16] = { 0 };
    if (out != crypt) memmove(out, crypt, (size_t)len);
    if (!ccm_ctr(key, key_len, nonce, out, len, mask)) return false;
    uint8_t tag[16];
    if (!ccm_tag(key, key_len, nonce, out, len, tag)) return false;
    uint8_t diff = 0;
    for (int i = 0; i < CCM_M; i++) diff |= (uint8_t)(tag[i] ^ mask[i] ^ tag8[i]);
    return diff == 0;
}

/* ── Meshtastic's DM layer ─────────────────────────────────────────────── */

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The firmware's nonce: packet id as a u64, from at byte 8, and the extra
 * nonce written over bytes 4..7 (the id's high half, which is 0). */
static void pki_nonce(uint8_t n[16], uint32_t from, uint32_t id, uint32_t extra)
{
    memset(n, 0, 16);
    wr32le(n, id);
    wr32le(n + 4, extra);
    wr32le(n + 8, from);
}

static bool pki_key(const uint8_t my_priv[32], const uint8_t their_pub[32],
                    uint8_t key[32])
{
    uint8_t shared[32];
    xlc_x25519(shared, my_priv, their_pub);
    uint8_t zero = 0;
    for (int i = 0; i < 32; i++) zero |= shared[i];
    if (!zero) return false;             /* a weak point, not a key */
    xprs_sha256(shared, 32, key);
    return true;
}

int mt_pki_encrypt(const uint8_t my_priv[32], const uint8_t their_pub[32],
                   uint32_t from, uint32_t id, uint32_t extra,
                   const uint8_t *plain, int len, uint8_t *out, int cap)
{
    if (len < 0 || len + MT_PKI_OVERHEAD > cap) return -1;
    uint8_t key[32], nonce[16];
    if (!pki_key(my_priv, their_pub, key)) return -1;
    pki_nonce(nonce, from, id, extra);
    if (!mt_ccm_encrypt(key, 32, nonce, plain, len, out, out + len)) return -1;
    wr32le(out + len + 8, extra);
    return len + MT_PKI_OVERHEAD;
}

int mt_pki_decrypt(const uint8_t my_priv[32], const uint8_t their_pub[32],
                   uint32_t from, uint32_t id, const uint8_t *in, int len,
                   uint8_t *out)
{
    if (len <= MT_PKI_OVERHEAD) return -1;
    int n = len - MT_PKI_OVERHEAD;
    uint32_t extra = (uint32_t)in[n + 8] | ((uint32_t)in[n + 9] << 8) |
                     ((uint32_t)in[n + 10] << 16) | ((uint32_t)in[n + 11] << 24);
    uint8_t key[32], nonce[16], tag[8];
    if (!pki_key(my_priv, their_pub, key)) return -1;
    pki_nonce(nonce, from, id, extra);
    memcpy(tag, in + n, 8);
    if (!mt_ccm_decrypt(key, 32, nonce, in, n, tag, out)) return -1;
    return n;
}

void mt_node_keys(const char *call, int len, uint8_t priv[32], uint8_t pub[32])
{
    xlc_seed_from_call("XPRS/mt/x25519", call, len, priv);
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);
    if (pub) xlc_x25519_base(pub, priv);
}
