/* Does the DEVICE's bundle parser agree with the HOST's packer?
 *
 * Two independent implementations -- Python in tools/mkbundle.py, C in
 * xs_bundle.c -- and they have to agree about a container that carries
 * executable code and the signature over it. If they ever disagree, either
 * every station refuses every bundle, or worse, one of them measures the
 * signed range differently from the other and something unsigned gets run.
 *
 * So it is checked on the desk, the same way xprs_ota checks its signing tool
 * against its device verifier. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#include "xs_bundle.h"

/* The hook xs_bundle.c calls. This test is about STRUCTURE, not signatures --
 * test_verify_host.c covers those -- but the parser and the verifier live in
 * one translation unit, so the symbol has to exist. */
void xsb_sha256(const uint8_t *in, size_t len, uint8_t out[32])
{
    SHA256(in, len, out);
}

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (!b || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <bundle> <expected-sha>\n", argv[0]); return 2; }

    size_t len = 0;
    uint8_t *buf = slurp(argv[1], &len);
    if (!buf) { printf("FAIL: cannot read %s\n", argv[1]); return 1; }

    xsb_t b;
    CHECK(xsb_parse(buf, len, &b), "a good bundle did not parse");

    CHECK(strcmp(b.id, "spike") == 0, "bundle id differs");
    CHECK(strcmp(b.version, "0.1.0") == 0, "version differs");
    CHECK(b.nmod == 2, "module count differs");
    CHECK(strcmp(b.mod[0].name, "one") == 0, "module 0 name differs");
    CHECK(strcmp(b.mod[1].name, "two") == 0, "module 1 name differs");
    CHECK(b.signed_len == len - XSB_BODY_OFF, "signed range is not the rest of the file");

    /* The tick floor is enforced by the parser, not trusted from the file:
     * the bundle asks for 10 ms and must come back clamped. */
    CHECK(b.mod[1].tick_ms == 100, "a 10 ms tick was not clamped to the 100 ms floor");
    CHECK(b.mod[0].tick_ms == 0, "module 0 should want no tick");

    for (int i = 0; i < b.nmod; i++) {
        CHECK(b.mod[i].off + b.mod[i].len <= len, "a payload runs past the end");
        CHECK(b.mod[i].len > 0, "a payload is empty");
    }

    CHECK(xsb_wants_type(&b, "t:message"), "declared type not matched");
    CHECK(xsb_wants_type(&b, "t:report"), "declared type not matched");
    CHECK(!xsb_wants_type(&b, "t:command"), "undeclared type matched");
    CHECK(!xsb_wants_type(&b, ""), "empty type matched");

    /* The line the signature is taken over must be byte-identical to the one
     * the packer printed. This is the drift that would strand a fleet. */
    char line[200];
    int n = xsb_signed_line(line, sizeof line, "tdeck", &b, argv[2]);
    CHECK(n > 0, "signed line did not build");
    char want[200];
    snprintf(want, sizeof want, "xprsscr1 tdeck spike 0.1.0 %u %s",
             (unsigned)b.signed_len, argv[2]);
    if (strcmp(line, want) != 0) {
        printf("FAIL: signed line differs\n  device: %s\n  packer: %s\n", line, want);
        fails++;
    }

    /* Now the ways a bundle must be REFUSED. Each is a real thing that
     * arrives off a half-finished push or a corrupt partition. */
    xsb_t junk;
    uint8_t *copy = malloc(len);

    /* Byte 1, not byte 0: the magic is "XSCB" and setting [0]='X' changes
     * nothing -- which this test got wrong first time and duly reported. */
    memcpy(copy, buf, len); copy[1] = 'Z';
    CHECK(!xsb_parse(copy, len, &junk), "bad magic was accepted");

    memcpy(copy, buf, len); copy[4] = 99;
    CHECK(!xsb_parse(copy, len, &junk), "unknown format version was accepted");

    memcpy(copy, buf, len); copy[6] = 0;
    CHECK(!xsb_parse(copy, len, &junk), "zero modules was accepted");

    memcpy(copy, buf, len); copy[6] = XSB_MODS_MAX + 1;
    CHECK(!xsb_parse(copy, len, &junk), "too many modules was accepted");

    CHECK(!xsb_parse(buf, len - 1, &junk), "a truncated bundle was accepted");
    CHECK(!xsb_parse(buf, XSB_BODY_OFF - 1, &junk), "a stub was accepted");

    memcpy(copy, buf, len);
    uint32_t huge = (uint32_t)len + 4096;
    memcpy(copy + XSB_BODY_OFF + 24, &huge, 4);
    CHECK(!xsb_parse(copy, len, &junk), "an out-of-range payload offset was accepted");

    /* A payload overlapping the header, which would let a module claim the
     * signature bytes as its own bytecode. */
    memcpy(copy, buf, len);
    uint32_t low = 8;
    memcpy(copy + XSB_BODY_OFF + 24, &low, 4);
    CHECK(!xsb_parse(copy, len, &junk), "a payload inside the header was accepted");

    free(copy);
    free(buf);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("PASS: mkbundle.py and xs_bundle.c agree, and malformed bundles are refused\n");
    return 0;
}
