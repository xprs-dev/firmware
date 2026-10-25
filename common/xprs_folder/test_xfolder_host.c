/* Host test for xprs_folder.c: the listing, the descriptions, the image
 * names and the version order. */
#include "xprs_folder.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* sha256("tdeck") in both forms, from the Dart side. */
#define TDECK_HEX  "f3d8bfd872491924567a14e89019848a4f5773295025f89dc28037165284b586"

static int n_lines;
static xfolder_entry_t last;
static bool collect(const xfolder_entry_t *e, void *ctx)
{
    (void)ctx;
    n_lines++;
    last = *e;
    return true;
}

static void test_refs(void)
{
    uint8_t a[32], b[32];
    CHECK(xfolder_ref_sha(TDECK_HEX ".bin", 68, a), "hex ref");
    /* base64url of the same 32 bytes */
    const char *b64 = "89i_2HJJGSRWehTokBmEik9XcylQJfidwoA3FlKEtYY.bin";
    CHECK(xfolder_ref_sha(b64, (int)strlen(b64), b), "b64u ref");
    CHECK(memcmp(a, b, 32) == 0, "the two forms are one reference");
    CHECK(!xfolder_ref_sha("not-a-ref", 9, a), "junk refused");
    CHECK(xfolder_qty_bytes("812", 3) == 812, "bare");
    CHECK(xfolder_qty_bytes("240kB", 5) == 240 * 1024, "kB");
    CHECK(xfolder_qty_bytes("2MB", 3) == 2 * 1024 * 1024, "MB");
    CHECK(xfolder_qty_bytes("x", 1) == 0, "junk qty");
}

static void test_listing(void)
{
    const char *l =
        "XFL1\n"
        "89i_2HJJGSRWehTokBmEik9XcylQJfidwoA3FlKEtYY.bin 1310720 esp32c3-mini-0.2.1.bin\n"
        "garbage\n"
        TDECK_HEX ".bin 1460192 tdeck-0.5.1.bin\r\n";
    n_lines = 0;
    int n = xfolder_listing_each(l, strlen(l), collect, NULL);
    CHECK(n == 2 && n_lines == 2, "lines %d", n);
    CHECK(strcmp(last.name, "tdeck-0.5.1.bin") == 0, "name '%s'", last.name);
    CHECK(last.size == 1460192, "size %u", (unsigned)last.size);
    CHECK(xfolder_listing_each("XFL2\n", 5, collect, NULL) == -1, "wrong magic");
}

static void test_descriptions(void)
{
    const char *fw = "t:file f:X1PUB1 ts:2026-10-25_12:00:00 file:qA7dTf2mWx9bK4pZcV0yLuJ3gRhN8sE5iDoQ6vXaB1M.xfl kind:folder count:3 size:2771724 m:XPRS firmware stable";
    xprs_t p;
    uint8_t sha[32];
    CHECK(xprs_parse(fw, (int)strlen(fw), &p), "parse folder");
    CHECK(xfolder_desc_folder(&p, sha), "a folder description");
    xfolder_entry_t e;
    CHECK(!xfolder_desc_member(&p, &e), "a folder is not a member");

    const char *mw = "t:file f:X1PUB1 ts:2026-10-25_12:00:00 file:" TDECK_HEX ".bin size:1460192 name:tdeck-0.5.1.bin";
    CHECK(xprs_parse(mw, (int)strlen(mw), &p), "parse member");
    CHECK(xfolder_desc_member(&p, &e), "a member description");
    CHECK(e.size == 1460192 && strcmp(e.name, "tdeck-0.5.1.bin") == 0, "member fields");
    CHECK(!xfolder_desc_folder(&p, sha), "a member is not a folder");

    const char *inl = "t:file f:X1PUB1 ts:2026-10-25_12:00:00 file:" TDECK_HEX ".png size:240 name:a.png b:AAAA";
    CHECK(xprs_parse(inl, (int)strlen(inl), &p), "parse inline");
    CHECK(!xfolder_desc_member(&p, &e), "an inline file is not a member description");
}

static void test_images(void)
{
    xfolder_image_t i;
    CHECK(xfolder_image_of("tdeck-0.5.1.bin", &i) && !strcmp(i.board, "tdeck") && !strcmp(i.version, "0.5.1"), "tdeck");
    CHECK(xfolder_image_of("tdongle-s3-0.5.1.bin", &i) && !strcmp(i.board, "tdongle-s3"), "a dash in the board: '%s'", i.board);
    CHECK(xfolder_image_of("m5stack-core-0.3.1-beta.2.bin", &i) && !strcmp(i.version, "0.3.1-beta.2"), "a tag: '%s'", i.version);
    CHECK(!xfolder_image_of("release notes.txt", &i), "not an image");
    CHECK(xfolder_version_cmp("0.5.1", "0.5.0") > 0, "patch");
    CHECK(xfolder_version_cmp("0.10.0", "0.9.9") > 0, "numeric, not lexical");
    CHECK(xfolder_version_cmp("0.5.1", "0.5.1") == 0, "equal");
    CHECK(xfolder_version_cmp("0.5.1-beta.1", "0.5.1") < 0, "a beta before its release");
    CHECK(xfolder_version_cmp("0.5.1", "0.5") > 0, "a longer version is newer");
    CHECK(xfolder_version_cmp("0.5.0", "0.5") == 0, "a trailing zero is the same");
    xfolder_entry_t e; memset(&e, 0, sizeof e);
    strcpy(e.name, "tdeck-0.5.2.bin");
    CHECK(xfolder_newer_for(&e, "tdeck", "0.5.1", NULL), "newer for this board");
    CHECK(!xfolder_newer_for(&e, "tdeck", "0.5.2", NULL), "not newer than itself");
    CHECK(!xfolder_newer_for(&e, "tdongle-s3", "0.1.0", NULL), "another board's image");
    CHECK(xfolder_newer_for(&e, "tdeck", "", NULL), "nothing running: anything is newer");
}

int main(void)
{
    test_refs();
    test_listing();
    test_descriptions();
    test_images();
    printf("xfolder host tests: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
