/* Host test for xwc_file: references as XPRS.md 7.7 writes them, and the
 * store behind /api/xprs/file (staff files, eviction, ranges).
 *   sh test_xwc_host.sh   (builds and runs this one too) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>

#include <openssl/sha.h>

#include "xwc_file.h"

static int fails;
#define CHECK(c, what) do { printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what); if (!(c)) fails++; } while (0)


/* ---- the store, on a directory in /tmp ---- */
static uint64_t s_free = 10u * 1024u * 1024u, s_total = 100u * 1024u * 1024u;
static bool space(uint64_t *f, uint64_t *t) { *f = s_free; *t = s_total; return true; }
#define DIR_T "/tmp/xwcf_store_test"

static bool there(const char *sub, const uint8_t sha[32])
{
    char hex[65], path[200];
    struct stat st;
    xwcf_hex(sha, hex);
    snprintf(path, sizeof path, DIR_T "%s/%s", sub, hex);
    return stat(path, &st) == 0;
}

/* Store [n] bytes of [data] under the digest of [as] (itself when NULL), fed
 * in pieces as a socket would. The HTTP status. */
static int put(const xwcf_store_t *s, const uint8_t *data, size_t n, const uint8_t *as,
               bool staff, bool preview, const char **why)
{
    uint8_t sha[32];
    SHA256(as ? as : data, n, sha);
    xwcf_put_t p;
    int code = xwcf_put_begin(s, &p, sha, n, staff, preview, why);
    if (code) return code;
    bool ok = true;
    for (size_t o = 0; o < n && ok; o += 777) ok = xwcf_put_add(&p, data + o, n - o < 777 ? n - o : 777);
    return xwcf_put_end(&p, ok, why);
}

static void age(const uint8_t sha[32], const char *sub, time_t t)
{
    char hex[65], path[200];
    xwcf_hex(sha, hex);
    snprintf(path, sizeof path, DIR_T "%s/%s", sub, hex);
    struct utimbuf u = { t, t };
    utime(path, &u);
}

/* ---- the rooms files were posted in (.acl) ---- */
static int s_seen;
static uint8_t s_last[32];
static void seen(const uint8_t sha[32], void *ctx) { (void)ctx; s_seen++; memcpy(s_last, sha, 32); }

/* A digest the store does not hold, for the prune. */
static const uint8_t *vs_held(const xwcf_store_t *s) { (void)s; static uint8_t z[32] = { 0xee }; return z; }

static void acl_tests(xwcf_store_t *s, const uint8_t held[32], const uint8_t gone[32])
{
    char room[XWCF_ACL_CALL];
    CHECK(!xwcf_acl_room(s, held, room), "a file never posted in a room has no room: public");
    CHECK(xwcf_acl_bind(s, held, "X5ROOMA", false) && xwcf_acl_room(s, held, room) && !strcmp(room, "X5ROOMA"),
          "a file posted in a room is bound to it");
    CHECK(xwcf_acl_bind(s, held, "X5ROOMB", true) && xwcf_acl_room(s, held, room) && !strcmp(room, "X5ROOMA"),
          "an upload naming a room does not re-bind a file already posted");
    CHECK(xwcf_acl_bind(s, held, "X5ROOMB", false) && xwcf_acl_room(s, held, room) && !strcmp(room, "X5ROOMB"),
          "a later post re-binds it: the newest wins (11.2)");
    CHECK(xwcf_acl_bind(s, gone, "X5ROOMA", false) && xwcf_acl_room(s, gone, room), "a second file, its own record");
    /* Full: the flash table holds 512; the record of a file not held goes. */
    s->own = false;
    uint8_t x[32] = { 0 };
    bool ok = true;
    for (int i = 0; i < 510 && ok; i++) { x[0] = 0x10; x[1] = (uint8_t)i; x[2] = (uint8_t)(i >> 8); ok = xwcf_acl_bind(s, x, "X5ROOMA", false); }
    CHECK(ok, "the table fills");
    x[1] = 0xff; x[2] = 0xff;
    CHECK(xwcf_acl_bind(s, x, "X5ROOMA", false) && xwcf_acl_room(s, held, room) && !xwcf_acl_room(s, gone, room),
          "full, the records of files no longer held go; a held one stays");
    s->own = true;
    char w[400], hex[65];
    xwcf_hex(held, hex);
    snprintf(w, sizeof w, "t:message f:X1ABC d:X5ROOMA file:%s.jpg m:see file:A5BYxvLAy0ksUzsKTRTvd8wPeKvMztUofYShogEc-4E.png. and xfile:nope", hex);
    s_seen = 0;
    int got = xwcf_refs(w, (int)strlen(w), seen, NULL), saw = s_seen;
    CHECK(got == 2 && saw == 2, "the field and a reference in the text are found, a sentence's stop left off");
    CHECK(xwcf_refs("t:message m:no files", 20, seen, NULL) == 0, "a wire with none has none");
}

static void store_tests(void)
{
    if (system("rm -rf " DIR_T) != 0) { /* nothing there yet */ }
    xwcf_store_t s = { .dir = DIR_T, .visitor_max = 2000, .budget = 5000, .reserve = 0,
                       .own = true, .space = space };
    const char *why = "";
    static uint8_t film[300000];
    for (size_t i = 0; i < sizeof film; i++) film[i] = (uint8_t)(i * 31 + 7);
    uint8_t fsha[32];
    SHA256(film, sizeof film, fsha);

    printf("the store\n");
    CHECK(put(&s, film, sizeof film, NULL, true, false, &why) == 200 && there("/own", fsha),
          "the staff store a film far past the visitor's cap: the station's own");
    CHECK(put(&s, film, 3000, NULL, false, false, &why) == 413 && !strcmp(why, "larger than this station keeps"),
          "a stranger past the cap is refused 413");
    uint8_t other[4000];
    memset(other, 9, sizeof other);
    uint8_t osha[32];
    SHA256(other, sizeof other, osha);
    CHECK(put(&s, other, sizeof other, film, true, false, &why) == 400 && !there("/own", osha),
          "bytes that do not hash to the reference are refused 400");
    {
        struct stat st;
        uint8_t wsha[32];
        SHA256(film, sizeof other, wsha);   /* the reference it claimed */
        CHECK(!there("/own", wsha) && stat(DIR_T "/own/part.tmp", &st) != 0,
              "and leave no file, not even a part");
    }
    s_free = 1024u * 1024u + 1000;      /* a 4 kB file would cut into the 1 MB reserve */
    CHECK(put(&s, other, sizeof other, NULL, true, false, &why) == 413 && !strcmp(why, "the card is full"),
          "the staff are refused only when the card would keep less than its reserve");
    s_free = 10u * 1024u * 1024u;

    /* A stranger's deposits: capped together, the oldest goes. */
    uint8_t v[4][1500], vs[4][32];
    for (int i = 0; i < 4; i++) {
        memset(v[i], 'a' + i, sizeof v[i]);
        SHA256(v[i], sizeof v[i], vs[i]);
    }
    CHECK(put(&s, v[0], sizeof v[0], NULL, false, false, &why) == 200 && there("", vs[0]), "a stranger's file is stored");
    CHECK(put(&s, v[1], sizeof v[1], NULL, false, true, &why) == 200 && there("/.thumbnails", vs[1]),
          "a stranger's preview in .thumbnails");
    CHECK(put(&s, v[2], sizeof v[2], NULL, false, false, &why) == 200, "a third");
    age(vs[0], "", 1000); age(vs[1], "/.thumbnails", 2000); age(vs[2], "", 3000);
    CHECK(put(&s, v[3], sizeof v[3], NULL, false, false, &why) == 200 && !there("", vs[0]) &&
          there("/.thumbnails", vs[1]) && there("", vs[3]),
          "past the budget the oldest stranger's file goes");
    CHECK(there("/own", fsha), "and the station's own never does");
    CHECK(put(&s, v[2], sizeof v[2], NULL, true, false, &why) == 200 && there("/own", vs[2]) && !there("", vs[2]),
          "the staff storing a stranger's file keep it as the station's own");
    CHECK(put(&s, v[0], sizeof v[0], NULL, true, true, &why) == 200 && there("/own/.thumbnails", vs[0]),
          "a staff preview goes in own/.thumbnails");
    xwcf_store_t flash = s;
    flash.own = false;
    CHECK(put(&flash, film, 5000, NULL, true, false, &why) == 413, "without a card the staff keep the visitor's cap");

    /* Reading. */
    uint64_t size = 0;
    FILE *f = xwcf_open(&s, fsha, &size);
    CHECK(f && size == sizeof film, "a held file opens, wherever it is");
    if (f) fclose(f);
    f = xwcf_open(&s, vs[0], &size);
    CHECK(f && size == sizeof v[0], "a preview too");
    if (f) fclose(f);
    CHECK(xwcf_open(&s, osha, &size) == NULL, "and one not held does not");

    printf("ranges\n");
    uint64_t a = 0, b = 0;
    CHECK(xwcf_range("bytes=0-99", 1000, &a, &b) == 1 && a == 0 && b == 99, "a-b");
    CHECK(xwcf_range("bytes=900-", 1000, &a, &b) == 1 && a == 900 && b == 999, "a-");
    CHECK(xwcf_range("bytes=-100", 1000, &a, &b) == 1 && a == 900 && b == 999, "-n");
    CHECK(xwcf_range("bytes=-5000", 1000, &a, &b) == 1 && a == 0 && b == 999, "-n past the start is the whole");
    CHECK(xwcf_range("bytes=990-5000", 1000, &a, &b) == 1 && a == 990 && b == 999, "b past the end stops at it");
    CHECK(xwcf_range("bytes=1000-", 1000, &a, &b) == -1 && xwcf_range("bytes=-0", 1000, &a, &b) == -1,
          "past the end, or nothing: unsatisfiable (416)");
    CHECK(xwcf_range(NULL, 1000, &a, &b) == 0 && xwcf_range("bytes=0-1,5-9", 1000, &a, &b) == 0 &&
          xwcf_range("items=0-1", 1000, &a, &b) == 0 && xwcf_range("bytes=9-1", 1000, &a, &b) == 0,
          "none, several, another unit, nonsense: the whole (200)");
    /* A part read as h_file_get reads it: seek, then chunks. */
    f = xwcf_open(&s, fsha, &size);
    int ok = f && xwcf_range("bytes=123456-", size, &a, &b) == 1 && fseek(f, (long)a, SEEK_SET) == 0;
    uint8_t chunk[4096];
    uint64_t at = a;
    while (ok && at <= b) {
        size_t r = fread(chunk, 1, sizeof chunk, f);
        if (!r || memcmp(chunk, film + at, r) != 0) ok = 0;
        at += r;
    }
    CHECK(ok && at == size, "the part read from the file is the film's bytes from there");
    if (f) fclose(f);
    CHECK(strcmp(xwcf_type("mov"), "video/quicktime") == 0 && strcmp(xwcf_type("webm"), "video/webm") == 0 &&
          strcmp(xwcf_type("mkv"), "video/x-matroska") == 0 && strcmp(xwcf_type("heic"), "image/heic") == 0 &&
          strcmp(xwcf_type("m4a"), "audio/mp4") == 0 && strcmp(xwcf_type("wav"), "audio/wav") == 0 &&
          strcmp(xwcf_type("ogv"), "video/ogg") == 0 && strcmp(xwcf_type("m4v"), "video/mp4") == 0,
          "films, sound and phone photographs have their types");
    acl_tests(&s, fsha, vs_held(&s));
    if (system("rm -rf " DIR_T) != 0) { /* left for a look */ }
}

/* A C3's flash volume: 792 KB in 4 KB clusters. A file is billed the
 * clusters it takes, and the reserve is held under the archive's own floor. */
static void small_volume_tests(void)
{
    if (system("rm -rf " DIR_T) != 0) { /* nothing there yet */ }
    xwcf_store_t s = { .dir = DIR_T, .visitor_max = 96u * 1024u, .budget = 3u * 4096u,
                       .reserve = 128u * 1024u, .cluster = 4096, .own = false, .space = space };
    const char *why = "";
    uint8_t v[4][1500], vs[4][32];
    for (int i = 0; i < 4; i++) {
        memset(v[i], 'k' + i, sizeof v[i]);
        SHA256(v[i], sizeof v[i], vs[i]);
    }
    printf("a small volume\n");
    s_total = 792u * 1024u;
    s_free = 60u * 1024u;               /* under the 128 KB reserve, over a sixteenth */
    int r0 = put(&s, v[0], sizeof v[0], NULL, false, false, &why);
    if (r0 != 200) printf("    got %d %s\n", r0, why);
    CHECK(r0 == 200, "a file fits while a sixteenth of the volume stays free");
    s_free = 40u * 1024u;               /* under a sixteenth (49.5 KB) */
    int r1 = put(&s, v[1], sizeof v[1], NULL, false, false, &why);
    if (r1 != 503) printf("    got %d\n", r1);
    CHECK(r1 == 503, "and none when it would not");
    s_free = 200u * 1024u;
    CHECK(put(&s, v[1], sizeof v[1], NULL, false, false, &why) == 200, "a second");
    CHECK(put(&s, v[2], sizeof v[2], NULL, false, false, &why) == 200, "a third: three clusters");
    age(vs[0], "", 1000); age(vs[1], "", 2000); age(vs[2], "", 3000);
    /* 4 x 1500 bytes is 6000, inside 12 KB by bytes; by clusters it is 16 KB. */
    CHECK(put(&s, v[3], sizeof v[3], NULL, false, false, &why) == 200 && !there("", vs[0]) &&
          there("", vs[1]) && there("", vs[3]),
          "a fourth costs a fourth cluster: the oldest goes");
    s_total = 100u * 1024u * 1024u;
    s_free = 10u * 1024u * 1024u;
}

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
    store_tests();
    small_volume_tests();
    printf(fails ? "xwc_file: %d failed\n" : "xwc_file: all passed\n", fails);
    return fails ? 1 : 0;
}
