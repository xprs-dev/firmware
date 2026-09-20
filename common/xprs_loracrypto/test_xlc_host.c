/*
 * Host test for xprs_loracrypto: every primitive against somebody else's
 * implementation, because "it agrees with itself" is what a second copy of
 * the same mistake also does.
 *
 *   SHA-512      against OpenSSL
 *   X25519       RFC 7748's vector, and against OpenSSL
 *   Ed25519      RFC 8032's first public key, then both directions against
 *                OpenSSL: it verifies what OpenSSL signed, OpenSSL verifies
 *                what it signed
 *   key exchange the two sides of a MeshCore pair agree
 *   HMAC-SHA256  against OpenSSL
 *   AES          the block the seam is filled with, against OpenSSL
 *
 * Run with test_xlc_host.sh.
 */

#include "xlc.h"

#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d  ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* The AES seam, from OpenSSL, as the Meshtastic harness already does. */
bool xlc_aes_encrypt_block(const uint8_t *key, int key_len,
                           const uint8_t in[16], uint8_t out[16])
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    const EVP_CIPHER *alg = key_len == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb();
    int n = 0;
    EVP_EncryptInit_ex(c, alg, NULL, key, NULL);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_EncryptUpdate(c, out, &n, in, 16);
    EVP_CIPHER_CTX_free(c);
    return n == 16;
}

static void unhex(const char *h, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static int same(const uint8_t *a, const uint8_t *b, int n)
{
    return memcmp(a, b, (size_t)n) == 0;
}

static void test_sha512(void)
{
    const char *msgs[] = { "", "abc", "the quick brown fox jumps over it" };
    for (int i = 0; i < 3; i++) {
        uint8_t ours[64], theirs[64];
        size_t len = strlen(msgs[i]);
        xlc_sha512((const uint8_t *)msgs[i], len, ours);
        SHA512((const unsigned char *)msgs[i], len, theirs);
        CHECK(same(ours, theirs, 64), "sha512 of \"%s\"", msgs[i]);
    }
    /* A message longer than one block, and one that lands exactly on the
     * length field's boundary (112 bytes), which is the case a padding
     * implementation gets wrong. */
    uint8_t big[300], ours[64], theirs[64];
    for (int i = 0; i < 300; i++) big[i] = (uint8_t)(i * 7);
    for (int n = 110; n <= 130; n++) {
        xlc_sha512(big, (size_t)n, ours);
        SHA512(big, (size_t)n, theirs);
        CHECK(same(ours, theirs, 64), "sha512 of %d bytes", n);
    }
    xlc_sha512(big, sizeof big, ours);
    SHA512(big, sizeof big, theirs);
    CHECK(same(ours, theirs, 64), "sha512 of 300 bytes");
}

static void test_x25519(void)
{
    /* RFC 7748 section 6.1. */
    uint8_t apriv[32], apub[32], bpriv[32], bpub[32], ss[32], want[32], got[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", apriv, 32);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, 32);
    xlc_x25519_base(apub, apriv);
    CHECK(same(apub, want, 32), "RFC 7748 Alice's public key");

    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bpriv, 32);
    unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", want, 32);
    xlc_x25519_base(bpub, bpriv);
    CHECK(same(bpub, want, 32), "RFC 7748 Bob's public key");

    unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", want, 32);
    xlc_x25519(ss, apriv, bpub);
    CHECK(same(ss, want, 32), "RFC 7748 shared secret");
    xlc_x25519(got, bpriv, apub);
    CHECK(same(got, want, 32), "and from the other side");

    /* And against OpenSSL, both ways round. */
    for (int i = 0; i < 32; i++) { apriv[i] = (uint8_t)(i + 1); bpriv[i] = (uint8_t)(0x40 + i); }
    apriv[0] &= 248; apriv[31] = (uint8_t)((apriv[31] & 127) | 64);
    bpriv[0] &= 248; bpriv[31] = (uint8_t)((bpriv[31] & 127) | 64);
    xlc_x25519_base(apub, apriv);
    xlc_x25519_base(bpub, bpriv);
    xlc_x25519(ss, apriv, bpub);
    xlc_x25519(got, bpriv, apub);
    CHECK(same(ss, got, 32), "both sides derive one secret");

    EVP_PKEY *ka = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, apriv, 32);
    EVP_PKEY *kb = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, bpub, 32);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(ka, NULL);
    size_t n = 32;
    EVP_PKEY_derive_init(ctx);
    EVP_PKEY_derive_set_peer(ctx, kb);
    EVP_PKEY_derive(ctx, got, &n);
    CHECK(same(ss, got, 32), "the same secret OpenSSL derives");
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(ka);
    EVP_PKEY_free(kb);
}

static void test_ed25519(void)
{
    uint8_t seed[32], sk[64], pub[32], want[32], sig[64];

    /* RFC 8032 section 7.1, the first test's key. */
    unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
    unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", want, 32);
    xlc_ed25519_from_seed(seed, sk, pub);
    CHECK(same(pub, want, 32), "RFC 8032 public key from the seed");

    const char *msg = "an advert is believable only because its signature is";
    size_t len = strlen(msg);

    /* Ours signs, OpenSSL verifies. */
    xlc_ed25519_sign(sig, (const uint8_t *)msg, len, sk, pub);
    CHECK(xlc_ed25519_verify(sig, (const uint8_t *)msg, len, pub), "our own signature");
    EVP_PKEY *vk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(md, NULL, NULL, NULL, vk);
    CHECK(EVP_DigestVerify(md, sig, 64, (const unsigned char *)msg, len) == 1,
          "OpenSSL verifies what we signed");
    EVP_MD_CTX_free(md);

    /* OpenSSL signs, ours verifies. */
    EVP_PKEY *sgk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    uint8_t theirs[64];
    size_t sl = 64;
    md = EVP_MD_CTX_new();
    EVP_DigestSignInit(md, NULL, NULL, NULL, sgk);
    EVP_DigestSign(md, theirs, &sl, (const unsigned char *)msg, len);
    EVP_MD_CTX_free(md);
    CHECK(sl == 64 && same(sig, theirs, 64), "byte-identical to OpenSSL's signature");
    CHECK(xlc_ed25519_verify(theirs, (const uint8_t *)msg, len, pub),
          "we verify what OpenSSL signed");

    /* A signature over other words, or under another key, is refused. */
    CHECK(!xlc_ed25519_verify(sig, (const uint8_t *)msg, len - 1, pub),
          "a shortened message does not verify");
    uint8_t bad[64];
    memcpy(bad, sig, 64);
    bad[10] ^= 0x01;
    CHECK(!xlc_ed25519_verify(bad, (const uint8_t *)msg, len, pub),
          "a flipped bit does not verify");
    uint8_t other[32], opub[32], osk[64];
    memset(other, 9, sizeof other);
    xlc_ed25519_from_seed(other, osk, opub);
    CHECK(!xlc_ed25519_verify(sig, (const uint8_t *)msg, len, opub),
          "somebody else's key does not verify it");

    /* The empty message, RFC 8032's own first case. */
    xlc_ed25519_sign(sig, (const uint8_t *)"", 0, sk, pub);
    CHECK(xlc_ed25519_verify(sig, (const uint8_t *)"", 0, pub), "the empty message");

    EVP_PKEY_free(vk);
    EVP_PKEY_free(sgk);
}

static void test_key_exchange(void)
{
    /* MeshCore's ed25519_key_exchange: two identities, one secret. */
    uint8_t sa[32], sb[32], ska[64], skb[64], pa[32], pb[32], k1[32], k2[32];
    memset(sa, 3, sizeof sa);
    memset(sb, 7, sizeof sb);
    xlc_ed25519_from_seed(sa, ska, pa);
    xlc_ed25519_from_seed(sb, skb, pb);
    CHECK(xlc_ed25519_key_exchange(k1, pb, ska), "our side");
    CHECK(xlc_ed25519_key_exchange(k2, pa, skb), "their side");
    CHECK(same(k1, k2, 32), "one shared secret");

    uint8_t zero[32];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(k1, zero, 32) != 0, "and it is not zero");
}

static void test_hmac(void)
{
    const char *key = "8b3387e9c5cdea6ac9e5edbaa115cd72";
    uint8_t msg[64], ours[32];
    unsigned char theirs[32];
    unsigned int tn = 32;
    for (int i = 0; i < 64; i++) msg[i] = (uint8_t)(i * 3 + 1);
    for (int len = 0; len <= 64; len += 16) {
        xlc_hmac_sha256((const uint8_t *)key, strlen(key), msg, (size_t)len, ours);
        HMAC(EVP_sha256(), key, (int)strlen(key), msg, (size_t)len, theirs, &tn);
        CHECK(tn == 32 && same(ours, theirs, 32), "hmac over %d bytes", len);
    }
    /* A key longer than the block, which is the case RFC 2104 hashes first. */
    uint8_t big[100];
    memset(big, 0xA5, sizeof big);
    xlc_hmac_sha256(big, sizeof big, msg, sizeof msg, ours);
    HMAC(EVP_sha256(), big, (int)sizeof big, msg, sizeof msg, theirs, &tn);
    CHECK(same(ours, theirs, 32), "a key longer than the block");
}

static void test_seed_from_call(void)
{
    uint8_t a[32], b[32], c[32];
    xlc_seed_from_call("XPRS/mc/ed25519", "X1QZ3N", 6, a);
    xlc_seed_from_call("XPRS/mc/ed25519", "x1qz3n-2", 8, b);
    CHECK(same(a, b, 32), "the device suffix and the case are the same person");
    xlc_seed_from_call("XPRS/mt/x25519", "X1QZ3N", 6, c);
    CHECK(!same(a, c, 32), "another network's identity is another identity");
}

int main(void)
{
    test_sha512();
    test_x25519();
    test_ed25519();
    test_key_exchange();
    test_hmac();
    test_seed_from_call();
    if (g_fail) {
        printf("xprs_loracrypto: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("xprs_loracrypto: all checks passed\n");
    return 0;
}
