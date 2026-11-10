/*
 * Host test for rns_msgpack.c and rns_serve.c (with rns_link.c's responder
 * and rns_resource.c's receiver for uploads) against reticulum-dart: the Dart
 * RnsRequestClient (tool/request_client.dart) runs as a child process
 * speaking packets as hex lines. This side is the station: it answers links
 * to its xprs.wapp destination and serves requests by path; the client
 * prints what it got and this side checks it, byte for byte.
 *
 * The msgpack forms are pinned first, against bytes Dart's msgpackEncode
 * writes (lxmf_msgpack.dart), so a request the page sends decodes here.
 */
#include "rns_link.h"
#include "rns_msgpack.h"
#include "rns_serve.h"
#define OPENSSL_SUPPRESS_DEPRECATED 1
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

void randombytes(unsigned char *p, unsigned long long n)
{
    for (unsigned long long i = 0; i < n; i++) p[i] = (unsigned char)(rand() & 0xff);
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hexs(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
}

static int unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

/* ── msgpack, against Dart's bytes ── */
static void test_msgpack(void)
{
    printf("msgpack\n");
    /* msgpackEncode([1754668930.5, <16 bytes 0..15>, {'m':'GET','p':'/api/services','q':{'limit':'20'}}]) */
    uint8_t req[128];
    mp_wr_t w;
    mp_wr_init(&w, req, sizeof req);
    mp_arr(&w, 3);
    mp_f64(&w, 1754668930.5);
    uint8_t ph[16];
    for (int i = 0; i < 16; i++) ph[i] = (uint8_t)i;
    mp_bin(&w, ph, 16);
    mp_map(&w, 3);
    mp_cstr(&w, "m"); mp_cstr(&w, "GET");
    mp_cstr(&w, "p"); mp_cstr(&w, "/api/services");
    mp_cstr(&w, "q"); mp_map(&w, 1); mp_cstr(&w, "limit"); mp_cstr(&w, "20");
    static const char *dart =
        "93cb41da2587e0a00000c410000102030405060708090a0b0c0d0e0f83a16da3474554a170ad2f6170692f7365727669636573a17181a56c696d6974a23230";
    char got[300];
    hexs(req, w.n, got);
    CHECK(!w.err && !strcmp(got, dart), "a request packs as Dart packs it\n   got  %s\n   want %s", got, dart);

    mp_rd_t r;
    mp_rd_init(&r, req, w.n);
    mp_obj_t o;
    CHECK(mp_next(&r, &o) && o.type == MP_ARR && o.len == 3, "array of 3");
    CHECK(mp_next(&r, &o) && o.type == MP_FLOAT && o.f == 1754668930.5, "the time");
    CHECK(mp_next(&r, &o) && o.type == MP_BIN && o.len == 16 && !memcmp(o.s, ph, 16), "the path hash");
    CHECK(mp_next(&r, &o) && o.type == MP_MAP && o.len == 3, "the data map");
    mp_rd_t at;
    CHECK(mp_map_find(&r, 3, "p", &at) && mp_next(&at, &o) && o.type == MP_STR &&
          o.len == 13 && !memcmp(o.s, "/api/services", 13), "p found");
    CHECK(mp_map_find(&r, 3, "q", &at) && mp_next(&at, &o) && o.type == MP_MAP && o.len == 1, "q found");
    CHECK(!mp_map_find(&r, 3, "b", &at), "b absent");
    /* Integers in each width, read back (Dart: msgpackEncode([5, 200, 70000,
     * 5000000000, null, true]) is 96 then these bytes). */
    uint8_t b[64];
    mp_wr_init(&w, b, sizeof b);
    mp_uint(&w, 5); mp_uint(&w, 200); mp_uint(&w, 70000); mp_uint(&w, 5000000000ULL);
    mp_nil(&w); mp_bool(&w, true);
    hexs(b, w.n, got);
    CHECK(!strcmp(got, "05ccc8ce00011170cf000000012a05f200c0c3"), "integers as Dart writes them: %s", got);
    mp_rd_init(&r, b, w.n);
    int64_t want[4] = { 5, 200, 70000, 5000000000LL };
    for (int i = 0; i < 4; i++) CHECK(mp_next(&r, &o) && o.type == MP_INT && o.i == want[i], "int %d", i);
    /* A negative fixint and int8 (Dart writes -1 as 0xff, -100 as d0 9c). */
    const uint8_t neg[] = { 0xff, 0xd0, 0x9c, 0xd1, 0xfc, 0x18 };
    mp_rd_init(&r, neg, sizeof neg);
    CHECK(mp_next(&r, &o) && o.i == -1, "-1");
    CHECK(mp_next(&r, &o) && o.i == -100, "-100");
    CHECK(mp_next(&r, &o) && o.i == -1000, "-1000");
    /* Truncated input is refused, not read past. */
    mp_rd_init(&r, req, 10);
    CHECK(mp_next(&r, &o) && mp_next(&r, &o) && !mp_next(&r, &o), "a short buffer stops the reader");
    /* The request id: truncated sha256 of the packed request. */
    uint8_t h[32];
    SHA256(req, w.n, h);
    (void)h;
}

/* ── the link server against the Dart client ── */

static FILE *to_dart, *from_dart;
static int verbose;

static void send_line(const uint8_t *pkt, size_t len, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) fprintf(to_dart, "%02x", pkt[i]);
    fputc('\n', to_dart);
    fflush(to_dart);
}

static uint8_t *bytes_of(size_t size)
{
    uint8_t *out = malloc(size ? size : 1);
    uint32_t x = 0x2545F491u ^ (uint32_t)size;
    for (size_t i = 0; i < size; i++) {
        x = (x * 1103515245u + 12345u) & 0x7fffffffu;
        out[i] = (uint8_t)((x >> 16) & 0xff);
    }
    return out;
}

typedef struct { rns_src_t s; uint8_t *buf; } ram_t;
static bool ram_read(rns_src_t *s, size_t off, uint8_t *buf, size_t n)
{
    ram_t *r = (ram_t *)s;
    if (off + n > s->len) return false;
    memcpy(buf, r->buf + off, n);
    return true;
}
static int released;
static void ram_release(rns_src_t *s) { ram_t *r = (ram_t *)s; free(r->buf); free(r); released++; }
/* A response's data: msgpack bin of [n] bytes. */
static rns_src_t *bin_src(const uint8_t *b, size_t n)
{
    ram_t *r = calloc(1, sizeof *r);
    r->buf = malloc(n + 5);
    mp_wr_t w;
    mp_wr_init(&w, r->buf, n + 5);
    mp_bin(&w, b, n);
    r->s.len = w.n;
    r->s.read = ram_read;
    r->s.release = ram_release;
    return &r->s;
}
static rns_src_t *str_src(const char *t)
{
    ram_t *r = calloc(1, sizeof *r);
    size_t n = strlen(t);
    r->buf = malloc(n + 5);
    mp_wr_t w;
    mp_wr_init(&w, r->buf, n + 5);
    mp_str(&w, t, n);
    r->s.len = w.n;
    r->s.read = ram_read;
    r->s.release = ram_release;
    return &r->s;
}

typedef struct { int slot; uint32_t gen; uint8_t rid[16]; uint8_t *packed; size_t len; bool too; } job_t;
static job_t s_jobs[8];
static int s_njobs, s_too_large;
static uint8_t s_link_id[16];

static void on_request(int slot, uint32_t gen, const uint8_t link_id[16], const uint8_t rid[16],
                       const uint8_t *packed, size_t len, bool too_large, void *ctx)
{
    (void)ctx;
    memcpy(s_link_id, link_id, 16);
    if (s_njobs >= 8) return;
    job_t *j = &s_jobs[s_njobs++];
    j->slot = slot; j->gen = gen; memcpy(j->rid, rid, 16);
    j->too = too_large;
    j->len = len;
    j->packed = packed ? malloc(len) : NULL;
    if (packed) memcpy(j->packed, packed, len);
}

static void path_hash(const char *p, uint8_t out[16])
{
    uint8_t h[32];
    SHA256((const uint8_t *)p, strlen(p), h);
    memcpy(out, h, 16);
}

static size_t s_max_bytes;

/* Run the queued requests, as the station's worker task does. */
static void run_jobs(void)
{
    uint8_t echo[16], map[16], big[16];
    path_hash("/echo", echo); path_hash("/map", map); path_hash("/big", big);
    for (int k = 0; k < s_njobs; k++) {
        job_t *j = &s_jobs[k];
        if (j->too) { s_too_large++; rns_serve_respond(j->slot, j->gen, j->rid, str_src("413")); continue; }
        mp_rd_t r;
        mp_obj_t o, ph, d;
        mp_rd_init(&r, j->packed, j->len);
        if (!mp_next(&r, &o) || o.type != MP_ARR || o.len < 3 || !mp_next(&r, &o) ||
            !mp_next(&r, &ph) || ph.type != MP_BIN || ph.len != 16) { CHECK(0, "a request decodes"); continue; }
        mp_rd_t data = r;
        if (!mp_next(&r, &d)) { CHECK(0, "request data"); continue; }
        if (!memcmp(ph.s, echo, 16) && (d.type == MP_STR || d.type == MP_BIN)) {
            rns_serve_respond(j->slot, j->gen, j->rid, bin_src(d.s, d.len));
        } else if (!memcmp(ph.s, map, 16) && d.type == MP_MAP) {
            mp_rd_t at;
            mp_obj_t m, p, q;
            char out[96] = "";
            if (mp_map_find(&r, d.len, "m", &at) && mp_next(&at, &m) &&
                mp_map_find(&r, d.len, "p", &at) && mp_next(&at, &p) &&
                mp_map_find(&r, d.len, "q", &at) && mp_next(&at, &q) && q.type == MP_MAP) {
                mp_rd_t lim;
                mp_obj_t l;
                if (mp_map_find(&at, q.len, "limit", &lim) && mp_next(&lim, &l))
                    snprintf(out, sizeof out, "%.*s %.*s %.*s", (int)m.len, m.s, (int)p.len, p.s, (int)l.len, l.s);
            }
            rns_serve_respond(j->slot, j->gen, j->rid, bin_src((const uint8_t *)out, strlen(out)));
        } else if (!memcmp(ph.s, big, 16) && d.type == MP_INT) {
            uint8_t *b = bytes_of((size_t)d.i);
            rns_serve_respond(j->slot, j->gen, j->rid, bin_src(b, (size_t)d.i));
            free(b);
        }
        (void)data;
        free(j->packed);
    }
    s_njobs = 0;
}

static void expect_sha(const char *v, size_t n)
{
    uint8_t *b = bytes_of(n), h[32];
    SHA256(b, n, h);
    free(b);
    char want[100], hx[65];
    hexs(h, 32, hx);
    snprintf(want, sizeof want, "%zu %s", n, hx);
    CHECK(!strcmp(v, want), "%zu bytes arrive whole: %s", n, v);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    verbose = getenv("VERBOSE") != NULL;
    test_msgpack();

    const char *dart = getenv("DART");
    if (!dart) dart = "dart";
    const char *tool = argc > 1 ? argv[1] : "../../../reticulum-dart/tool/request_client.dart";

    /* The station's identity: X25519 || Ed25519, as xprsrns keeps it. */
    uint8_t prv[64], pub[64], edsec[64];
    randombytes(prv, 32);
    rns_x25519_base(prv, pub);
    rns_ed25519_keypair(pub + 32, edsec);
    memcpy(prv + 32, edsec, 32);
    rns_identity_t id;
    rns_identity_init(prv, pub, &id);
    uint8_t name[RNS_NAME_HASH_LEN], dest[RNS_HASH_LEN];
    const char *aspects[] = { "wapp" };
    rns_name_hash("xprs", aspects, 1, name);
    rns_destination_hash(name, id.hash, dest);

    rns_serve_cfg_t cfg = {
        .send = send_line, .request = on_request, .id = &id,
        .max_links = 2, .idle_s = 600, .upload_max = 32768, .mtu_cap = 500,
    };
    memcpy(cfg.dest, dest, RNS_HASH_LEN);
    rns_serve_init(&cfg);

    char pubhex[129];
    hexs(pub, 64, pubhex);
    int in[2], outp[2];
    if (pipe(in) || pipe(outp)) return 2;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(outp[1], 1);
        close(in[1]); close(outp[0]);
        execlp(dart, dart, "run", tool, pubhex, (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(outp[1]);
    to_dart = fdopen(in[1], "w");
    from_dart = fdopen(outp[0], "r");

    printf("requests over a link, from the Dart client\n");
    static char line[1 << 13];
    static uint8_t raw[1 << 12];
    char upload_got[100] = "", upload_want[100] = "";
    bool done = false;
    uint32_t now = 1000;
    int results = 0;
    while (!done && fgets(line, sizeof line, from_dart)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!strncmp(line, "result ", 7)) {
            results++;
            char *name = line + 7, *v = strchr(name, ' ');
            if (!v) continue;
            *v++ = 0;
            printf("  %s: %.80s\n", name, v);
            if (!strcmp(name, "open")) CHECK(!strcmp(v, "true"), "the link opened");
            else if (!strcmp(name, "link")) {
                char hx[33]; hexs(s_link_id, 16, hx);
                CHECK(rns_serve_links() == 1, "one link open");
                (void)hx;
            }
            else if (!strcmp(name, "echo")) CHECK(!strcmp(v, "hello there"), "a string echoed in one packet");
            else if (!strcmp(name, "map")) CHECK(!strcmp(v, "GET /api/services 20"), "an /xprs/api-shaped map read");
            else if (!strcmp(name, "big1000")) expect_sha(v, 1000);
            else if (!strcmp(name, "big70000")) expect_sha(v, 70000);
            else if (!strcmp(name, "big1100000")) expect_sha(v, 1100000);
            else if (!strcmp(name, "upload")) snprintf(upload_got, sizeof upload_got, "%s", v);
            else if (!strcmp(name, "upload_want")) snprintf(upload_want, sizeof upload_want, "%s", v);
            else if (!strcmp(name, "too_large")) CHECK(!strcmp(v, "413") && s_too_large == 1, "past the upload cap: answered with its id");
            else if (!strcmp(name, "nothing")) CHECK(!strcmp(v, "null"), "no handler: no answer");
            continue;
        }
        if (!strcmp(line, "done")) { done = true; break; }
        int rl = unhex(line, raw, sizeof raw);
        if (rl <= 0) { if (verbose) printf("  dart: %s\n", line); continue; }
        rns_packet_t p;
        if (!rns_packet_parse(raw, (size_t)rl, &p)) { CHECK(0, "a packet from Dart parses"); continue; }
        rns_serve_feed(&p, now);
        for (int s = 0; s < RNS_SERVE_LINKS_MAX; s++) {
            size_t b = rns_serve_link_bytes(s);
            if (b > s_max_bytes) s_max_bytes = b;
        }
        run_jobs();
    }
    CHECK(done && results >= 10, "the client ran every request (%d results)", results);
    CHECK(upload_got[0] && !strcmp(upload_got, upload_want), "an upload (a request as a Resource) arrives whole: %s", upload_got);
    printf("  most held for one link: %zu bytes (during the 1.1 MB answer)\n", s_max_bytes);
    /* LINKCLOSE from the client ends the link; every source was released. */
    for (int k = 0; k < 20 && rns_serve_links(); k++) {
        if (!fgets(line, sizeof line, from_dart)) break;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n')) line[--n] = 0;
        int rl = unhex(line, raw, sizeof raw);
        rns_packet_t p;
        if (rl > 0 && rns_packet_parse(raw, (size_t)rl, &p)) rns_serve_feed(&p, now);
    }
    CHECK(rns_serve_links() == 0, "the client's LINKCLOSE closes the link");
    CHECK(released == 7, "every response source released once (%d)", released);
    rns_serve_tick(now + 10000);
    fclose(to_dart);
    int status = 0;
    waitpid(pid, &status, 0);
    printf("rns serve host tests: %d checks, %s (%d failed)\n", checks, fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
