/* Does the DEVICE's reader understand what the BUILD TOOL produced?
 *
 * Two independent implementations -- Python in tools/mkassets.py, C in
 * common/xprs_assets/xasset.c -- have to agree byte for byte about a
 * format nobody will look at again until a splash comes out as noise on a
 * board with no debugger attached. So it is checked on the desk, the same
 * way xprs_ota checks the signing tool against the device verifier.
 *
 * The ESP-IDF partition API is stubbed by a flat file, which is exactly
 * what a raw partition is. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "xasset.h"

static FILE *g_img;
static esp_partition_t g_part;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t sub,
                                                const char *label)
{
    (void)type; (void)sub;
    if (!label || strcmp(label, "assets") != 0) return NULL;
    return g_img ? &g_part : NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off,
                             void *buf, size_t len)
{
    (void)p;
    if (off + len > g_part.size) return ESP_FAIL;
    if (fseek(g_img, (long)off, SEEK_SET) != 0) return ESP_FAIL;
    return fread(buf, 1, len, g_img) == len ? ESP_OK : ESP_FAIL;
}

const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }

#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <assets.bin>\n", argv[0]); return 2; }

    g_img = fopen(argv[1], "rb");
    CHECK(g_img, "cannot open the archive");
    fseek(g_img, 0, SEEK_END);
    g_part.size = (uint32_t)ftell(g_img);
    g_part.address = 0xF90000;

    CHECK(xasset_open() == ESP_OK, "xasset_open rejected a good archive");
    CHECK(xasset_ready(), "archive opened but reports not ready");

    /* The image the tool was told to store as 8x4. */
    xasset_t a;
    CHECK(xasset_find("splash", &a), "'splash' not found");
    CHECK(a.kind == XASSET_KIND_RGB565, "'splash' is not an image");
    CHECK(a.w == 8 && a.h == 4, "'splash' has the wrong dimensions");
    CHECK(a.len == 4u + 8u * 4u * 2u, "'splash' payload length disagrees");

    /* First pixel: the tool was given pure red (255,0,0) -> 0xF800.
     * Default (no --swap-bytes) stores it little-endian: 00 F8. */
    unsigned char px[2];
    CHECK(xasset_read(&a, 4, px, 2) == 2, "could not read the first pixel");
    CHECK(px[0] == 0x00 && px[1] == 0xF8, "first pixel is not little-endian 0xF800");

    /* A raw asset round-trips verbatim. */
    xasset_t r;
    CHECK(xasset_find("note", &r), "'note' not found");
    CHECK(r.kind == XASSET_KIND_RAW, "'note' is not raw");
    char txt[16] = {0};
    CHECK(xasset_read(&r, 0, txt, sizeof txt - 1) == 5, "'note' wrong length");
    CHECK(strcmp(txt, "hello") == 0, "'note' contents differ");

    /* Absence is ordinary, not an error, and must not be a false positive. */
    CHECK(!xasset_find("nope", &a), "found an asset that does not exist");
    /* A prefix of a real name must not match it. */
    CHECK(!xasset_find("spl", &a), "prefix matched a longer name");
    /* Reads are clipped, never overrun. */
    CHECK(xasset_find("note", &r), "'note' vanished");
    CHECK(xasset_read(&r, 4, txt, 99) == 1, "read past the end was not clipped");
    CHECK(xasset_read(&r, 999, txt, 4) == 0, "read beyond the asset returned data");

    printf("PASS: mkassets.py and xasset.c agree on the format\n");
    fclose(g_img);
    return 0;
}
