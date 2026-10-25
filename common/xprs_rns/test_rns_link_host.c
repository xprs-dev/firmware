/*
 * Host test for rns_link.c and rns_resource.c against reticulum-dart: the
 * Dart `xprs/files` responder (tool/files_responder.dart) runs as a child
 * process speaking packets as hex lines, this side opens a link, asks for
 * each file with GET_FILE, streams the resource through the sink and checks
 * the sha256 of what arrived against what the responder announced. One file
 * spans several hashmap batches, the other two segments.
 */
#include "rns_link.h"
#include "rns_resource.h"
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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static FILE *to_dart, *from_dart;

static int verbose;
static void send_hex(const uint8_t *pkt, size_t len, void *ctx)
{
    (void)ctx;
    if (verbose) printf("  -> %zu bytes ctx=%02x type=%d\n", len, len > 18 ? pkt[18] : 0, pkt[0] & 3);
    for (size_t i = 0; i < len; i++) fprintf(to_dart, "%02x", pkt[i]);
    fputc('\n', to_dart);
    fflush(to_dart);
}

static int read_line(char *buf, size_t cap)
{
    if (!fgets(buf, (int)cap, from_dart)) return -1;
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (verbose) printf("  <- %.70s%s\n", buf, n > 70 ? "..." : "");
    return (int)n;
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

/* The sink: a running sha256 and a byte count. */
typedef struct { SHA256_CTX sha; uint32_t n; bool done, ok; char why[48]; } sink_t;
static bool sink_write(const uint8_t *d, size_t len, void *ctx)
{ sink_t *s = ctx; SHA256_Update(&s->sha, d, len); s->n += (uint32_t)len; return true; }
static void sink_done(bool ok, const char *why, void *ctx)
{ sink_t *s = ctx; s->done = true; s->ok = ok; snprintf(s->why, sizeof s->why, "%s", why); }

static uint8_t peer_pub[RNS_PUB_LEN], files_dest[RNS_HASH_LEN];

/* Fetch one file by sha; true when the bytes verified. */
static bool fetch(const uint8_t sha[32], uint32_t size)
{
    rns_link_t link;
    rns_link_init(&link, files_dest, peer_pub, NULL);
    uint8_t pkt[RNS_MTU * 2];
    int n = rns_link_request(&link, pkt, sizeof pkt);
    CHECK(n > 0, "request built");
    send_hex(pkt, (size_t)n, NULL);

    sink_t sink; memset(&sink, 0, sizeof sink); SHA256_Init(&sink.sha);
    rns_res_sink_t sk = { sink_write, sink_done, &sink };
    rns_res_t *res = NULL;
    bool asked = false;
    char line[4096];
    uint8_t raw[1024];
    int lines = 0;
    while (!sink.done) {
        if (read_line(line, sizeof line) < 0) { CHECK(0, "responder went away"); break; }
        if (!strncmp(line, "active", 6) || !strncmp(line, "serving", 7)) continue;
        if (!strncmp(line, "error", 5)) { CHECK(0, "responder: %s", line); break; }
        int rl = unhex(line, raw, sizeof raw);
        if (rl <= 0) continue;
        lines++;
        rns_packet_t p;
        if (!rns_packet_parse(raw, (size_t)rl, &p)) { CHECK(0, "unparsable packet"); continue; }
        if (!rns_link_owns(&link, &p)) continue;
        if (link.state == RNS_LINK_PENDING) {
            int rn = rns_link_proof(&link, &p, pkt, sizeof pkt);
            CHECK(rn > 0, "proof verified and LRRTT built");
            if (rn <= 0) return false;
            send_hex(pkt, (size_t)rn, NULL);
            /* GET_FILE */
            uint8_t get[33]; get[0] = 0x01; memcpy(get + 1, sha, 32);
            int gn = rns_link_encrypt(&link, RNS_CTX_NONE, get, sizeof get, pkt, sizeof pkt);
            CHECK(gn > 0, "GET_FILE built");
            send_hex(pkt, (size_t)gn, NULL);
            asked = true;
            res = rns_res_new(&link, &sk);
            continue;
        }
        if (!asked || !res) continue;
        if (p.context == RNS_CTX_NONE) {
            uint8_t plain[64];
            int pn = rns_link_decrypt(&link, &p, plain, sizeof plain);
            if (pn > 0 && plain[0] == 0x81) { CHECK(0, "responder: not found"); break; }
            continue;
        }
        rns_res_feed(res, &p, send_hex, NULL);
    }
    char st[160];
    if (res) printf("  %s, %d packets in\n", rns_res_state(res, st, sizeof st), lines);
    CHECK(sink.done && sink.ok, "transfer finished: %s", sink.why);
    CHECK(sink.n == size, "size %u of %u", (unsigned)sink.n, (unsigned)size);
    uint8_t got[32];
    SHA256_Final(got, &sink.sha);
    CHECK(memcmp(got, sha, 32) == 0, "sha256 of what arrived");
    bool ok = sink.done && sink.ok && sink.n == size && memcmp(got, sha, 32) == 0;
    if (res) rns_res_free(res);
    return ok;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    verbose = getenv("VERBOSE") != NULL;
    const char *dart = getenv("DART");
    if (!dart) dart = "dart";
    const char *tool = argc > 1 ? argv[1] : "../../../reticulum-dart/tool/files_responder.dart";
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return 2;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1);
        close(in[1]); close(out[0]);
        execlp(dart, dart, "run", tool, "70000", "1300000", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    to_dart = fdopen(in[1], "w");
    from_dart = fdopen(out[0], "r");

    uint8_t shas[2][32]; uint32_t sizes[2]; int nf = 0;
    char line[512];
    bool ready = false;
    while (!ready && read_line(line, sizeof line) >= 0) {
        if (!strncmp(line, "pub ", 4)) CHECK(unhex(line + 4, peer_pub, sizeof peer_pub) == RNS_PUB_LEN, "pub");
        else if (!strncmp(line, "file ", 5) && nf < 2) {
            char h[65]; unsigned sz;
            if (sscanf(line + 5, "%64s %u", h, &sz) == 2) { unhex(h, shas[nf], 32); sizes[nf] = sz; nf++; }
        } else if (!strncmp(line, "dest ", 5)) CHECK(unhex(line + 5, files_dest, sizeof files_dest) == RNS_HASH_LEN, "dest");
        else if (!strcmp(line, "ready")) ready = true;
    }
    CHECK(ready && nf == 2, "responder started (%d files)", nf);
    if (ready && nf == 2) {
        /* The destination hash as this side derives it must be the one the
         * responder announced: name hash of xprs/files, identity hash of pub. */
        rns_identity_t idt;
        rns_identity_from_public(peer_pub, &idt);
        uint8_t name[RNS_NAME_HASH_LEN], dest[RNS_HASH_LEN];
        const char *aspects[] = { "files" };
        rns_name_hash("xprs", aspects, 1, name);
        rns_destination_hash(name, idt.hash, dest);
        CHECK(memcmp(dest, files_dest, RNS_HASH_LEN) == 0, "xprs/files destination derived");

        printf("file 1: %u bytes (one segment, several hashmap batches)\n", (unsigned)sizes[0]);
        fetch(shas[0], sizes[0]);
        printf("file 2: %u bytes (two segments)\n", (unsigned)sizes[1]);
        fetch(shas[1], sizes[1]);
    }
    fprintf(to_dart, "quit\n"); fflush(to_dart);
    fclose(to_dart);
    int status = 0;
    waitpid(pid, &status, 0);
    printf("rns link/resource host tests: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
