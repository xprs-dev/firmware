/* Host test for xwc_file: references as XPRS.md 7.7 writes them.
 *   sh test_xwc_host.sh   (builds and runs this one too) */
#include <stdio.h>
#include <string.h>

#include <openssl/sha.h>

#include "xwc_file.h"

static int fails;
#define CHECK(c, what) do { printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what); if (!(c)) fails++; } while (0)

int main(void)
{
    uint8_t sha[32], want[32];
    char ext[XWCF_EXT_MAX + 1], hex[65];
    const unsigned char data[3] = { 1, 2, 3 };
    SHA256(data, 3, want);
    /* The page's refOf([1,2,3]) (webchat test/blog_test.js). */
    CHECK(xwcf_parse("A5BYxvLAy0ksUzsKTRTvd8wPeKvMztUofYShogEc-4E.png", sha, ext) &&
          memcmp(sha, want, 32) == 0 && strcmp(ext, "png") == 0, "base64url reads to the digest and the type");
    xwcf_hex(want, hex);
    char ref[80];
    snprintf(ref, sizeof ref, "%s.jpg", hex);
    CHECK(xwcf_parse(ref, sha, ext) && memcmp(sha, want, 32) == 0, "64 hex reads to the same digest");
    CHECK(strcmp(hex, "039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81") == 0, "the store's name is the hex");
    CHECK(!xwcf_parse("A5BYxvLAy0ksUzsKTRTvd8wPeKvMztUofYShogEc-4F.png", sha, ext), "stray bits past the digest are refused");
    CHECK(!xwcf_parse("A5BYxvLAy0ksUzsKTRTvd8wPeKvMztUofYShogEc-4E.PNG", sha, ext), "an uppercase type is refused");
    CHECK(!xwcf_parse("A5BYxvLAy0ksUzsKTRTvd8wPeKvMztUofYShogEc-4E.toolongext", sha, ext), "so is a type past 8");
    CHECK(!xwcf_parse("A5BYxvLAy0ksUzsKTRTvd8wPeKvMztUofYShogEc-4E", sha, ext), "and one with no type");
    CHECK(!xwcf_parse("hello.png", sha, ext) && !xwcf_parse("../../idx/wcstate.txt", sha, ext), "and anything else, a path above all");
    CHECK(!xwcf_parse("A5BYxvLAy0ksUzsKTRTvd8wPeKvMztUofYShogEc-4E.png.png", sha, ext), "two dots are not a reference");
    CHECK(strcmp(xwcf_type("png"), "image/png") == 0 && strcmp(xwcf_type("exe"), "application/octet-stream") == 0,
          "types: known ones by name, the rest octets");
    printf(fails ? "xwc_file: %d failed\n" : "xwc_file: all passed\n", fails);
    return fails ? 1 : 0;
}
