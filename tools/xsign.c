/* xsign -- sign one line with an XPRS key, for DEVELOPMENT ONLY.
 *
 * Production signing is tools/sign_firmware.dart, which takes an nsec file and
 * whose header states the rule this file must not undermine: THE KEY DOES NOT
 * GO INTO CI. This exists because bringing a script bundle up on a bench needs
 * a signature in a shell loop, and because the resulting signature must be
 * produced by the SAME C code the station verifies with -- if the two ever
 * disagree, every station refuses every bundle.
 *
 * It takes the private scalar as hex on the command line, which is exactly the
 * handling a real key must never get. Use a throwaway key for the bench.
 *
 * Build: gcc -DXPRSSIG_HOST_TEST -I../common/geogram_xprssig \
 *            -o xsign xsign.c ../common/geogram_xprssig/xprssig.c -lcrypto
 * Usage: xsign <32-byte-priv-hex> "<line to sign>"
 *        xsign --pub <32-byte-priv-hex>
 */

#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>

#include "xprssig.h"

static int unhex(const char *hex, unsigned char *out, int n)
{
    if ((int)strlen(hex) != n * 2) return 0;
    for (int i = 0; i < n; i++) {
        unsigned b;
        if (sscanf(hex + i * 2, "%2x", &b) != 1) return 0;
        out[i] = (unsigned char)b;
    }
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char priv[XPRSSIG_KEY_LEN], pub[XPRSSIG_KEY_LEN];

    if (argc == 3 && strcmp(argv[1], "--pub") == 0) {
        if (!unhex(argv[2], priv, XPRSSIG_KEY_LEN)) { fprintf(stderr, "bad key\n"); return 2; }
        if (!xprssig_public_key(priv, pub)) { fprintf(stderr, "not a valid key\n"); return 1; }
        for (int i = 0; i < XPRSSIG_KEY_LEN; i++) printf("%02x", pub[i]);
        printf("\n");
        return 0;
    }

    if (argc != 3) {
        fprintf(stderr, "usage: %s <priv-hex> \"<line>\"\n       %s --pub <priv-hex>\n",
                argv[0], argv[0]);
        return 2;
    }
    if (!unhex(argv[1], priv, XPRSSIG_KEY_LEN)) { fprintf(stderr, "bad key\n"); return 2; }

    unsigned char digest[32];
    SHA256((const unsigned char *)argv[2], strlen(argv[2]), digest);

    unsigned char sig[XPRSSIG_LEN];
    if (!xprssig_sign(digest, priv, sig)) { fprintf(stderr, "sign failed\n"); return 1; }

    char b85[XPRSSIG_B85_LEN + 1];
    if (xprssig_b85_encode(sig, sizeof sig, b85, sizeof b85) != XPRSSIG_B85_LEN) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    printf("%s\n", b85);
    return 0;
}
