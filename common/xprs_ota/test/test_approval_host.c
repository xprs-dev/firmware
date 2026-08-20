/* Does the DEVICE's verifier accept what the signing tool produced? The two
 * implementations are independent -- Dart on one side, C on the other -- and
 * this is the only assertion that matters for an OTA nobody can undo. */
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include "xprssig.h"

static int unhex(const char *h, unsigned char *o, int n) {
    for (int i = 0; i < n; i++) if (sscanf(h + 2*i, "%2hhx", &o[i]) != 1) return 0;
    return 1;
}
int main(void) {
    const char *line = "xprsfw1 m5stack-core 0.1.0 1457376 512ded47037e0a59497fa3a813f28131a49b7e81f555d4a18e8667ff95e9fbcb";
    const char *sig85 = "6zEaG$_%E0Kmz_ndmf*EU.%3MJ=RcWddaioTVb1xao7Bub_gnOO4Ce7@5KT?";
    const char *pubhex = "c956c4d599c8810147b72687871218dee4b9a49e8e1adb39db4e58a062b9e4b6";
    unsigned char digest[32], sig[XPRSSIG_LEN], pub[32];
    SHA256((const unsigned char *)line, strlen(line), digest);
    if (xprssig_b85_decode(sig85, XPRSSIG_B85_LEN, sig, sizeof sig) != XPRSSIG_LEN) {
        printf("FAIL: base85 did not decode\n"); return 1;
    }
    if (!unhex(pubhex, pub, 32)) { printf("FAIL: bad pubkey hex\n"); return 1; }
    if (!xprssig_verify(digest, sig, pub)) { printf("FAIL: signature rejected\n"); return 1; }
    printf("ok: the C verifier accepts the Dart approval\n");

    /* And it must REFUSE a tampered line -- a different version, same bytes. */
    char bad[200]; snprintf(bad, sizeof bad, "%s", line);
    char *v = strstr(bad, "0.1.0"); if (v) v[4] = '1';
    SHA256((const unsigned char *)bad, strlen(bad), digest);
    if (xprssig_verify(digest, sig, pub)) { printf("FAIL: accepted a forged version\n"); return 1; }
    printf("ok: a changed version is refused\n");
    return 0;
}
