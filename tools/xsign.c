/* xsign -- sign one line with an XPRS key, with the SAME C code the station
 * verifies with: if the two ever disagree, every station refuses every
 * bundle. tools/scripts/publish_folder.py runs it to sign a firmware
 * release's folder descriptions (docs/plan-firmware-folders.md), on the
 * release workflow and on a bench alike.
 *
 * It takes the private scalar as hex on the command line. In the release
 * workflow the scalar comes from a repository secret and never leaves the
 * runner; on a bench use a throwaway key.
 *
 * Build: gcc -DXPRSSIG_HOST_TEST -I../common/xprs_sig \
 *            -o xsign xsign.c ../common/xprs_sig/xprssig.c -lcrypto
 * Usage: xsign <32-byte-priv-hex> "<line to sign>"      prints the sig (base85)
 *        xsign --pub <32-byte-priv-hex>                   prints the x-only public key
 *        xsign --verify <32-byte-pub-hex> "<line>" <sig>  exit 0 when it verifies
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

    if (argc == 5 && strcmp(argv[1], "--verify") == 0) {
        if (!unhex(argv[2], pub, XPRSSIG_KEY_LEN)) { fprintf(stderr, "bad key\n"); return 2; }
        unsigned char digest[32], sig[XPRSSIG_LEN];
        SHA256((const unsigned char *)argv[3], strlen(argv[3]), digest);
        if (xprssig_b85_decode(argv[4], strlen(argv[4]), sig, sizeof sig) != XPRSSIG_LEN) {
            fprintf(stderr, "bad signature encoding\n");
            return 2;
        }
        if (!xprssig_verify(digest, sig, pub)) { printf("BAD\n"); return 1; }
        printf("OK\n");
        return 0;
    }

    if (argc != 3) {
        fprintf(stderr, "usage: %s <priv-hex> \"<line>\"\n       %s --pub <priv-hex>\n"
                        "       %s --verify <pub-hex> \"<line>\" <sig>\n",
                argv[0], argv[0], argv[0]);
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
