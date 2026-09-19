/*
 * Host test for xprs_meshtastic. Run ./test_mt_host.sh.
 *
 * The protobuf and AES-CTR vectors below were produced with Meshtastic's own
 * Python protobufs (the `meshtastic` package, meshtastic.protobuf.mesh_pb2)
 * and with the `cryptography` package's textbook AES-CTR over the nonce the
 * firmware builds (packet id u64 LE, from u32 LE, four zero bytes). They are
 * an independent check on code written from the field numbers, not a copy
 * of anything.
 */
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mt.h"
#include "mt_mesh.h"
#include "xprs.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

bool mt_aes_encrypt_block(const uint8_t *key, int key_len,
                          const uint8_t in[16], uint8_t out[16])
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0;
    bool ok = EVP_EncryptInit_ex(c, key_len == 32 ? EVP_aes_256_ecb()
                                                  : EVP_aes_128_ecb(),
                                 NULL, key, NULL) == 1 &&
              EVP_CIPHER_CTX_set_padding(c, 0) == 1 &&
              EVP_EncryptUpdate(c, out, &n, in, 16) == 1 && n == 16;
    EVP_CIPHER_CTX_free(c);
    return ok;
}

static int unhex(const char *h, uint8_t *out)
{
    int n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void test_protobuf(void)
{
    uint8_t want[128], got[128];
    /* Data{portnum:1, payload:"hello mesh", reply_id, emoji:1, bitfield:1} */
    int wn = unhex("0801120a68656c6c6f206d6573683d4433221145010000004801", want);
    mt_data_t d = { 0 };
    d.portnum = MT_PORT_TEXT;
    d.payload = (const uint8_t *)"hello mesh";
    d.payload_len = 10;
    d.reply_id = 0x11223344;
    d.emoji = 1;
    d.has_bitfield = true;
    d.bitfield = 1;
    int n = mt_data_encode(&d, got, sizeof got);
    CHECK(n == wn && memcmp(got, want, (size_t)n) == 0);
    mt_data_t r;
    CHECK(mt_data_decode(want, wn, &r));
    CHECK(r.portnum == 1 && r.payload_len == 10 &&
          memcmp(r.payload, "hello mesh", 10) == 0);
    CHECK(r.reply_id == 0x11223344 && r.emoji == 1 && r.has_bitfield &&
          r.bitfield == 1);

    /* User{id, long_name, short_name, hw_model:PRIVATE_HW} */
    wn = unhex("0a09216131623263336434120b6a6f616f205831515a334e1a04515a334e28ff01",
               want);
    mt_user_t u = { 0 };
    strcpy(u.id, "!a1b2c3d4");
    strcpy(u.long_name, "joao X1QZ3N");
    strcpy(u.short_name, "QZ3N");
    u.hw_model = MT_HW_PRIVATE;
    n = mt_user_encode(&u, got, sizeof got);
    CHECK(n == wn && memcmp(got, want, (size_t)n) == 0);
    mt_user_t ur;
    CHECK(mt_user_decode(want, wn, &ur));
    CHECK(strcmp(ur.long_name, "joao X1QZ3N") == 0 &&
          strcmp(ur.short_name, "QZ3N") == 0 && ur.hw_model == 255 &&
          !ur.has_public_key);

    /* Routing{error_reason: NONE} */
    n = mt_routing_encode_ack(got, sizeof got);
    CHECK(n == 2 && got[0] == 0x18 && got[1] == 0x00);
    int er = -1;
    CHECK(mt_routing_decode(got, n, &er) && er == 0);

    /* Data{portnum:0x158, payload:<an XPRS wire>} */
    const char *w = "t:message f:X1QZ3N ts:2026-08-08_14:26:40 m:OK";
    wn = unhex("08d802122e743a6d65737361676520663a5831515a334e2074733a323032362d"
               "30382d30385f31343a32363a3430206d3a4f4b", want);
    mt_data_t dx = { 0 };
    dx.portnum = MT_PORT_XPRS;
    dx.payload = (const uint8_t *)w;
    dx.payload_len = (int)strlen(w);
    n = mt_data_encode(&dx, got, sizeof got);
    CHECK(n == wn && memcmp(got, want, (size_t)n) == 0);

    /* Garbage (what a wrong key yields) must not decode as a Data. */
    uint8_t junk[12] = { 0x00, 0x00, 0xff, 0x01, 0x02, 0x03 };
    CHECK(!mt_data_decode(junk, sizeof junk, &r));
}

static void test_header(void)
{
    mt_hdr_t h = { .to = MT_BROADCAST, .from = 0xa1b2c3d4, .id = 0x0badf00d,
                   .hop_limit = 3, .hop_start = 3, .want_ack = true,
                   .via_mqtt = false, .channel = 0x08, .next_hop = 0,
                   .relay_node = 0xd4 };
    uint8_t b[MT_HDR_LEN];
    mt_hdr_build(&h, b);
    CHECK(b[0] == 0xff && b[4] == 0xd4 && b[7] == 0xa1 && b[8] == 0x0d);
    CHECK(b[12] == (3 | 0x08 | (3 << 5)));
    mt_hdr_t r;
    CHECK(mt_hdr_parse(b, sizeof b, &r));
    CHECK(r.to == h.to && r.from == h.from && r.id == h.id &&
          r.hop_limit == 3 && r.hop_start == 3 && r.want_ack &&
          !r.via_mqtt && r.channel == 8 && r.relay_node == 0xd4);
}

static void test_channels(void)
{
    CHECK(mt_longfast_hash() == 0x08);
    CHECK(mt_channel_hash(MT_CH_NAME_XPRS, NULL, 0) == MT_CH_HASH_XPRS);
    uint8_t psk = 1, key[32];
    CHECK(mt_psk_expand(&psk, 1, key) == 16 &&
          memcmp(key, mt_default_key, 16) == 0);
    psk = 0;
    CHECK(mt_psk_expand(&psk, 1, key) == 0);
    CHECK(mt_djb2("LongFast") == 130429955u);

    /* The published LongFast slots: EU_868 869.525, US 906.875 (slot 20),
     * ANZ 919.875 (slot 20). */
    CHECK(mt_slot_freq_hz(mt_region_find("eu"), "LongFast", 250000) ==
          869525000u);
    CHECK(mt_slot_freq_hz(mt_region_find("us"), "LongFast", 250000) ==
          906875000u);
    CHECK(mt_slot_freq_hz(mt_region_find("au"), "LongFast", 250000) ==
          919875000u);
    CHECK(mt_region_find("EU_868") == mt_region_find("eu"));
    CHECK(mt_region_find("eu-g1") == NULL);
}

static void test_crypt(void)
{
    uint8_t pt[128], ct[128], buf[128];
    int n = unhex("0801120a68656c6c6f206d6573683d44332211450100000048010801120a68"
                  "656c6c6f206d6573683d44332211450100000048010801120a68656c6c6f20"
                  "6d6573683d4433221145010000004801", pt);
    int m = unhex("bdf0ac5a612d9fcdc0aed1b19336da6028a2690654440fd21cdc7d39fc75a9"
                  "468bb63954e4a8f9d010caee501c3e0c4c74184d5f8db0617fe2762c903340"
                  "9d5c7fc0e5a44e95a1f200aa992f1dfd", ct);
    CHECK(n == m);
    memcpy(buf, pt, (size_t)n);
    CHECK(mt_crypt(mt_default_key, 16, 0xa1b2c3d4, 0x0badf00d, buf, n));
    CHECK(memcmp(buf, ct, (size_t)n) == 0);
    CHECK(mt_crypt(mt_default_key, 16, 0xa1b2c3d4, 0x0badf00d, buf, n));
    CHECK(memcmp(buf, pt, (size_t)n) == 0);
}

static void test_identities(void)
{
    uint32_t a = mt_node_of_call("X1QZ3N", 6);
    CHECK(a == mt_node_of_call("X1QZ3N-7", 8));     /* suffix dropped */
    CHECK(a == mt_node_of_call("x1qz3n", 6));       /* case folded */
    CHECK(a != mt_node_of_call("X1QZ3", 5));
    CHECK(a > 3 && a != MT_BROADCAST);

    char c[16];
    CHECK(mt_call_of_node(0xa1b2c3d4, c, sizeof c) == 10 &&
          strcmp(c, "MTA1B2C3D4") == 0);
    uint32_t n = 0;
    CHECK(mt_node_of_mtcall("MTA1B2C3D4", 10, &n) && n == 0xa1b2c3d4);
    CHECK(!mt_node_of_mtcall("MTa1b2c3d4", 10, &n));
    CHECK(!mt_node_of_mtcall("MTA1B2C3D", 9, &n));
    CHECK(!mt_node_of_mtcall("X1A1B2C3D4", 10, &n));

    char nick[20];
    CHECK(mt_nick_from_name("Joao's  boat \xf0\x9f\x9b\xa5", nick, sizeof nick) > 0 &&
          strcmp(nick, "Joaos-boat") == 0);
    CHECK(mt_nick_from_name("\xf0\x9f\x98\x80", nick, sizeof nick) == 0);
    CHECK(mt_nick_from_name("a very long name indeed yes", nick, sizeof nick) == 16);
}

static void test_xprs_frames(void)
{
    const char *w = "t:message f:X1QZ3N ts:2026-08-08_14:26:40 m:OK";
    int len = (int)strlen(w);
    uint8_t fr[2][MT_FRAME_MAX];
    int fl[2];
    uint32_t self = 0x11223344;
    CHECK(mt_xprs_wrap(w, len, self, fr, fl) == 1);
    CHECK(fl[0] == mt_xprs_frame_len(len, 0));
    mt_hdr_t h;
    CHECK(mt_hdr_parse(fr[0], fl[0], &h));
    CHECK(h.to == MT_BROADCAST && h.channel == MT_CH_HASH_XPRS &&
          h.from == mt_node_of_call("X1QZ3N", 6) && h.hop_limit == 3 &&
          h.hop_start == 3 && h.relay_node == 0x44);

    /* A relayed copy (via: appended) keeps (from, id) and spends a hop. */
    char relayed[300];
    int rl = xprs_append_via(w, len, "X3RLY7", relayed, sizeof relayed);
    CHECK(rl > 0);
    mt_hdr_t h2;
    CHECK(mt_xprs_hdr_of(relayed, rl, self, &h2));
    CHECK(h2.from == h.from && h2.id == h.id && h2.hop_limit == 2);

    mt_reasm_t ra;
    memset(&ra, 0, sizeof ra);
    char out[260];
    CHECK(mt_xprs_unwrap(&ra, fr[0], fl[0], 0, out, sizeof out) == len &&
          strcmp(out, w) == 0);

    /* The worst case: 250 bytes, two frames, each at most 255. */
    char big[251];
    int bl = snprintf(big, sizeof big, "t:message f:X1QZ3N ts:2026-08-08_14:26:40 m:");
    while (bl < 250) { big[bl] = (char)('a' + (bl % 26)); bl++; }
    big[bl] = 0;
    CHECK(mt_xprs_frames_for(bl) == 2);
    CHECK(mt_xprs_wrap(big, bl, self, fr, fl) == 2);
    CHECK(fl[0] <= MT_FRAME_MAX && fl[1] <= MT_FRAME_MAX);
    CHECK(fl[0] == mt_xprs_frame_len(bl, 0) && fl[1] == mt_xprs_frame_len(bl, 1));
    mt_hdr_t f0, f1;
    mt_hdr_parse(fr[0], fl[0], &f0);
    mt_hdr_parse(fr[1], fl[1], &f1);
    CHECK(f0.from == f1.from && f0.id != f1.id);
    /* Out of order, and a stranger's fragment in between. */
    CHECK(mt_xprs_unwrap(&ra, fr[1], fl[1], 100, out, sizeof out) == 0);
    CHECK(mt_xprs_unwrap(&ra, fr[0], fl[0], 200, out, sizeof out) == bl &&
          memcmp(out, big, (size_t)bl) == 0);
    /* A fragment whose sibling never comes ages out. */
    CHECK(mt_xprs_unwrap(&ra, fr[1], fl[1], 1000, out, sizeof out) == 0);
    CHECK(mt_xprs_unwrap(&ra, fr[0], fl[0], 1000 + MT_XPRS_FRAG_TTL_MS + 1,
                         out, sizeof out) == 0);

    /* A 233-byte wire is one frame of exactly 255. */
    big[233] = 0;
    CHECK(mt_xprs_frames_for(233) == 1);
    CHECK(mt_xprs_wrap(big, 233, self, fr, fl) == 1 && fl[0] == 255);

    /* A beacon is nobody's to flood: no Meshtastic hops. */
    const char *ob = "t:observation f:X3RLY7 link:lora peers:2 ts:2026-08-08_14:26:40";
    mt_hdr_t ho;
    CHECK(mt_xprs_hdr_of(ob, (int)strlen(ob), self, &ho) && ho.hop_limit == 0 &&
          ho.hop_start == 0);

    /* LongFast traffic is not ours. */
    uint8_t lf[40];
    mt_hdr_t lh = { .to = MT_BROADCAST, .from = 1234, .id = 5,
                    .channel = 0x08 };
    mt_hdr_build(&lh, lf);
    memset(lf + 16, 0, 24);
    CHECK(mt_xprs_unwrap(&ra, lf, sizeof lf, 0, out, sizeof out) == -1);
}

static void test_pki(void)
{
    /* RFC 7748 section 5.2, the first X25519 vector. */
    uint8_t k[32], u[32], want[32], got[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
    unhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", want);
    mt_x25519(got, k, u);
    CHECK(memcmp(got, want, 32) == 0);

    /* The derived pair of X3DCK0, and a Meshtastic node's, as Python's
     * `cryptography` computes them. */
    uint8_t xpriv[32], xpub[32], mpriv[32], mpub[32];
    mt_node_keys("X3DCK0", 6, xpriv, xpub);
    unhex("006222e2aac3bf84a9877abd87ec442636dca6568c1cb1a0b7098f49a1e27b4a", want);
    CHECK(memcmp(xpriv, want, 32) == 0);
    unhex("ba7f7679919b51e427653dae95e34b3babbc2bc8e4d299497c02c8550835300f", want);
    CHECK(memcmp(xpub, want, 32) == 0);
    uint8_t xp2[32];
    mt_node_keys("x3dck0-4", 8, xp2, NULL);                  /* bare, folded */
    CHECK(memcmp(xp2, xpriv, 32) == 0);
    unhex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20", mpriv);
    mt_x25519_base(mpub, mpriv);
    unhex("07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c", want);
    CHECK(memcmp(mpub, want, 32) == 0);

    /* A DM from that node to X3DCK0's, sealed by `cryptography`'s AESCCM
     * over the firmware's nonce: id, extra nonce, from. */
    uint8_t pt[64], pki[96], out[96];
    int pn = unhex("0801121a444d20746f20746865206465636b2c207365636f6e642074727948003801", pt);
    int kn = unhex("4334d717d5092f0c0941129d8ccd2ee4e102b0e6df6b3d364e4342387c6e35c516e1f1466196b45b0c9e44332211", pki);
    CHECK(kn == pn + MT_PKI_OVERHEAD);
    CHECK(mt_pki_decrypt(xpriv, mpub, 0x0c39f654, 0x2604e6a, pki, kn, out) == pn &&
          memcmp(out, pt, (size_t)pn) == 0);
    /* The same, sealed here, is byte for byte what Python made. */
    CHECK(mt_pki_encrypt(mpriv, xpub, 0x0c39f654, 0x2604e6a, 0x11223344, pt, pn,
                         out, sizeof out) == kn && memcmp(out, pki, (size_t)kn) == 0);
    /* A flipped bit does not authenticate. */
    pki[3] ^= 1;
    CHECK(mt_pki_decrypt(xpriv, mpub, 0x0c39f654, 0x2604e6a, pki, kn, out) < 0);
}

void test_mesh(void);   /* test_mt_mesh_host.c */

int main(void)
{
    test_protobuf();
    test_header();
    test_channels();
    test_crypt();
    test_identities();
    test_xprs_frames();
    test_pki();
    test_mesh();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("xprs_meshtastic: all checks passed\n");
    return 0;
}

int mt_test_failures(void) { return g_fail; }
void mt_test_fail(const char *file, int line, const char *what)
{
    printf("FAIL %s:%d  %s\n", file, line, what);
    g_fail++;
}
