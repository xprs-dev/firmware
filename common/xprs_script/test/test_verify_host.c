/* Does a signed bundle verify, and do the ways of forging one all fail?
 *
 * xsb_verify() is the single decision that separates "run this code" from
 * "refuse it", on a device that will be handed bytecode over the air. It is
 * exercised here with the firmware's OWN signing and hashing code, so what is
 * proven on the desk is what runs on the roof -- not a re-implementation that
 * could agree with itself while disagreeing with the station.
 *
 * The negative cases are the point. Each is something that really arrives:
 * an erased partition, a half-finished push, a bundle built for another
 * board, an approval replayed from an older version, or a payload edited
 * after signing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#include "xs_bundle.h"
#include "xprssig.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* The hook xs_bundle.c calls; mbedTLS supplies this on the target. */
void xsb_sha256(const uint8_t *in, size_t len, uint8_t out[32])
{
    SHA256(in, len, out);
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (!b || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return b;
}

static void hexify(const uint8_t *in, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = h[in[i] >> 4]; out[i*2+1] = h[in[i] & 15]; }
    out[n*2] = 0;
}

/* Sign `buf` as `board` would have to be signed, and stamp it into the
 * header -- i.e. do exactly what the operator does with sign_firmware.dart
 * and mkbundle.py stamp. */
static bool sign_into(uint8_t *buf, size_t len, const char *board,
                      const uint8_t priv[32])
{
    xsb_t b;
    if (!xsb_parse(buf, len, &b)) return false;

    uint8_t sha[32];
    xsb_sha256(buf + XSB_BODY_OFF, b.signed_len, sha);
    char shahex[65]; hexify(sha, 32, shahex);

    char line[200];
    if (xsb_signed_line(line, sizeof line, board, &b, shahex) < 0) return false;

    uint8_t digest[32];
    xsb_sha256((const uint8_t *)line, strlen(line), digest);

    uint8_t sig[XPRSSIG_LEN];
    if (!xprssig_sign(digest, priv, sig)) return false;

    char b85[XPRSSIG_B85_LEN + 1];
    if (xprssig_b85_encode(sig, sizeof sig, b85, sizeof b85) != XPRSSIG_B85_LEN)
        return false;

    memset(buf + 48, 0, XSB_SIG_MAX);
    memcpy(buf + 48, b85, XPRSSIG_B85_LEN);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <bundle>\n", argv[0]); return 2; }

    size_t len = 0;
    uint8_t *orig = slurp(argv[1], &len);
    if (!orig) { printf("FAIL: cannot read %s\n", argv[1]); return 1; }

    /* A fixed scalar, so the test is reproducible. Not a secret: it signs
     * nothing but this temporary file. */
    uint8_t priv[32], pub[32];
    for (int i = 0; i < 32; i++) priv[i] = (uint8_t)(i + 1);
    CHECK(xprssig_public_key(priv, pub), "could not derive the public key");

    uint8_t *buf = malloc(len);
    xsb_t b;

    /* 1. UNSIGNED must not verify. The partition is erased far more often
     *    than it is forged. */
    memcpy(buf, orig, len);
    CHECK(xsb_parse(buf, len, &b), "unsigned bundle did not parse");
    CHECK(!xsb_verify(buf, len, &b, "tdeck", pub), "an UNSIGNED bundle verified");

    /* 2. Properly signed for this board must verify. */
    memcpy(buf, orig, len);
    CHECK(sign_into(buf, len, "tdeck", priv), "signing failed");
    CHECK(xsb_parse(buf, len, &b), "signed bundle did not parse");
    CHECK(xsb_verify(buf, len, &b, "tdeck", pub), "a correctly signed bundle was REFUSED");

    /* 3. Same bytes, different board. A bundle must not travel. */
    CHECK(!xsb_verify(buf, len, &b, "m5stack-core", pub),
          "a bundle signed for tdeck verified on m5stack-core");

    /* 4. Payload edited after signing -- one byte, in the bytecode. */
    {
        uint8_t *t = malloc(len);
        memcpy(t, buf, len);
        t[len - 1] ^= 0x01;
        xsb_t tb;
        CHECK(xsb_parse(t, len, &tb), "tampered bundle did not parse");
        CHECK(!xsb_verify(t, len, &tb, "tdeck", pub),
              "a bundle with an edited payload verified");
        free(t);
    }

    /* 5. Version rewritten in the header, approval replayed. */
    {
        uint8_t *t = malloc(len);
        memcpy(t, buf, len);
        memset(t + 24, 0, XSB_VER_MAX);
        memcpy(t + 24, "9.9.9", 5);
        xsb_t tb;
        CHECK(xsb_parse(t, len, &tb), "version-bumped bundle did not parse");
        CHECK(!xsb_verify(t, len, &tb, "tdeck", pub),
              "an approval was replayed against a different version");
        free(t);
    }

    /* 6. A different publisher's key must not verify. */
    {
        uint8_t other_priv[32], other_pub[32];
        for (int i = 0; i < 32; i++) other_priv[i] = (uint8_t)(200 - i);
        CHECK(xprssig_public_key(other_priv, other_pub), "second key failed");
        CHECK(!xsb_verify(buf, len, &b, "tdeck", other_pub),
              "a bundle verified against the WRONG publisher key");
    }

    /* 7. A signature of the right length but pure garbage. */
    {
        uint8_t *t = malloc(len);
        memcpy(t, buf, len);
        for (int i = 0; i < XPRSSIG_B85_LEN; i++) t[48 + i] = 'A';
        xsb_t tb;
        CHECK(xsb_parse(t, len, &tb), "garbage-sig bundle did not parse");
        CHECK(!xsb_verify(t, len, &tb, "tdeck", pub), "a garbage signature verified");
        free(t);
    }

    free(buf); free(orig);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("PASS: a signed bundle verifies; unsigned, tampered, replayed, "
           "cross-board and wrong-key ones do not\n");
    return 0;
}
