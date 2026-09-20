/*
 * Host test for xprs_meshcore's wire: the frame against MeshCore's
 * published layout, the crypto against OpenSSL, and the XPRS framing
 * against itself.
 *
 * What can be checked without a MeshCore node on the bench is checked here;
 * what cannot (whether a stock repeater carries our RAW_CUSTOM) is named in
 * docs/meshtastic.md as the first thing to try on the air.
 *
 * Run with test_mc_host.sh.
 */

#include "mc.h"

#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

#include "xlc.h"
#include "xprs.h"

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d  ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* The two AES seams, from OpenSSL. */
static bool aes_block(const uint8_t *key, int key_len, const uint8_t in[16],
                      uint8_t out[16], bool enc)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    const EVP_CIPHER *alg = key_len == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb();
    int n = 0;
    if (enc) EVP_EncryptInit_ex(c, alg, NULL, key, NULL);
    else     EVP_DecryptInit_ex(c, alg, NULL, key, NULL);
    EVP_CIPHER_CTX_set_padding(c, 0);
    if (enc) EVP_EncryptUpdate(c, out, &n, in, 16);
    else     EVP_DecryptUpdate(c, out, &n, in, 16);
    EVP_CIPHER_CTX_free(c);
    return n == 16;
}

bool xlc_aes_encrypt_block(const uint8_t *key, int key_len,
                           const uint8_t in[16], uint8_t out[16])
{
    return aes_block(key, key_len, in, out, true);
}

bool xlc_aes_decrypt_block(const uint8_t *key, int key_len,
                           const uint8_t in[16], uint8_t out[16])
{
    return aes_block(key, key_len, in, out, false);
}

static void test_header(void)
{
    /* The layout: [header][path length][path][payload], the header
     * 0bVVPPPPRR. A flood RAW_CUSTOM with two one-byte hops. */
    uint8_t frame[64], payload[8] = { 1, 2, 3, 4 };
    mc_pkt_t p;
    memset(&p, 0, sizeof p);
    p.route = MC_ROUTE_FLOOD;
    p.type = MC_PT_RAW_CUSTOM;
    p.hops = 2;
    p.hash_size = 1;
    p.path[0] = 0xAA;
    p.path[1] = 0xBB;
    p.payload = payload;
    p.payload_len = 4;
    int n = mc_build(&p, frame, sizeof frame);
    CHECK(n == 1 + 1 + 2 + 4, "a frame is header, path length, path, payload: %d", n);
    CHECK(frame[0] == (uint8_t)((MC_PT_RAW_CUSTOM << 2) | MC_ROUTE_FLOOD),
          "the header byte is 0bVVPPPPRR: %02x", frame[0]);
    CHECK(frame[1] == 2, "the path length byte carries the hop count: %02x", frame[1]);

    mc_pkt_t q;
    CHECK(mc_parse(frame, n, &q), "and it reads back");
    CHECK(q.route == MC_ROUTE_FLOOD && q.type == MC_PT_RAW_CUSTOM &&
          q.version == 0, "route, type and version");
    CHECK(q.hops == 2 && q.hash_size == 1 && q.path[0] == 0xAA &&
          q.path[1] == 0xBB, "the path");
    CHECK(q.payload_len == 4 && q.payload[3] == 4, "the payload");

    /* Three-byte hashes, which the top two bits of the length byte say. */
    p.hash_size = 3;
    p.hops = 2;
    n = mc_build(&p, frame, sizeof frame);
    CHECK(n == 1 + 1 + 6 + 4, "three-byte hops: %d", n);
    CHECK((frame[1] >> 6) == 2, "the hash size, minus one, in bits 6-7");
    CHECK(mc_parse(frame, n, &q) && q.hash_size == 3 && q.hops == 2,
          "and reads back");

    /* Transport codes, which only the transport route types carry. */
    p.route = MC_ROUTE_TRANSPORT_FLOOD;
    p.hash_size = 1;
    p.hops = 0;
    p.transport[0] = 0x1234;
    p.transport[1] = 0xABCD;
    n = mc_build(&p, frame, sizeof frame);
    CHECK(n == 1 + 4 + 1 + 4, "four bytes of transport codes: %d", n);
    CHECK(mc_parse(frame, n, &q) && q.transport[0] == 0x1234 &&
          q.transport[1] == 0xABCD, "and they read back");

    /* A repeater appends its hash and the rest is untouched. */
    p.route = MC_ROUTE_FLOOD;
    p.hops = 1;
    p.path[0] = 0x11;
    n = mc_build(&p, frame, sizeof frame);
    mc_parse(frame, n, &q);
    uint8_t relayed[64];
    uint8_t mine = 0x22;
    int rn = mc_path_append(&q, &mine, relayed, sizeof relayed);
    mc_pkt_t r;
    CHECK(rn == n + 1 && mc_parse(relayed, rn, &r), "one hop longer: %d", rn);
    CHECK(r.hops == 2 && r.path[0] == 0x11 && r.path[1] == 0x22, "our hash last");
    CHECK(mc_packet_hash(&q) == mc_packet_hash(&r),
          "and the packet is the same packet: the hash is over payload and type");

    /* What is not a frame. */
    CHECK(!mc_parse(frame, 1, &q), "one byte is not a frame");
    uint8_t liar[8] = { (uint8_t)((MC_PT_ADVERT << 2) | MC_ROUTE_FLOOD), 60 };
    CHECK(!mc_parse(liar, 2, &q), "a path that is not there");
}

static void test_channel(void)
{
    /* The byte that names MeshCore's public channel is the first of the
     * SHA-256 of its key. */
    uint8_t want[32];
    xprs_sha256(mc_public_key, 16, want);
    CHECK(mc_channel_hash(mc_public_key) == want[0], "the channel hash");

    uint8_t payload[MC_PAYLOAD_MAX];
    int n = mc_grp_txt_build(mc_public_key, 1789900000u, "roof X3DCK0",
                             "anyone on the public channel?", payload,
                             sizeof payload);
    CHECK(n > 3 && (n - 3) % 16 == 0, "channel hash, MAC, whole cipher blocks: %d", n);
    CHECK(payload[0] == mc_channel_hash(mc_public_key), "addressed to the channel");

    mc_text_t t;
    char sender[32];
    CHECK(mc_grp_txt_open(mc_public_key, payload, n, &t, sender, sizeof sender),
          "and it opens");
    CHECK(t.timestamp == 1789900000u, "the timestamp");
    CHECK(strcmp(sender, "roof X3DCK0") == 0, "the sender's own word for itself: %s", sender);
    CHECK(strcmp(t.text, "anyone on the public channel?") == 0, "the words: %s", t.text);

    /* THE LONGEST MESSAGE THERE IS. The cipher pads to whole blocks, so a
     * body that fits before padding may not fit after it: the build has to
     * shorten it rather than fail, because a failure here is silent and
     * the message is simply never sent. */
    char big[300];
    memset(big, 'w', sizeof big);
    big[sizeof big - 1] = 0;
    n = mc_grp_txt_build(mc_public_key, 1789900000u, "roof X3DCK0", big,
                         payload, sizeof payload);
    CHECK(n > 0 && n <= MC_PAYLOAD_MAX, "a long channel message still goes: %d", n);
    CHECK(mc_grp_txt_open(mc_public_key, payload, n, &t, sender, sizeof sender),
          "and opens");
    CHECK(strcmp(sender, "roof X3DCK0") == 0, "with its sender intact: %s", sender);
    CHECK((int)strlen(t.text) > 100, "and most of the words: %d",
          (int)strlen(t.text));

    /* Another key does not open it, and a flipped bit does not pass. */
    uint8_t other[16];
    memcpy(other, mc_public_key, 16);
    other[0] ^= 0x01;
    CHECK(!mc_grp_txt_open(other, payload, n, &t, sender, sizeof sender),
          "another channel's key");
    payload[5] ^= 0x01;
    CHECK(!mc_grp_txt_open(mc_public_key, payload, n, &t, sender, sizeof sender),
          "a flipped bit fails the MAC");
}

static void test_dm(void)
{
    /* Two identities, the secret they share, and a message between them. */
    uint8_t sk_a[64], pub_a[32], sk_b[64], pub_b[32], sec_a[32], sec_b[32];
    mc_node_keys("X1QZ3N", 6, sk_a, pub_a);
    uint8_t seed[32];
    memset(seed, 0x5b, sizeof seed);
    xlc_ed25519_from_seed(seed, sk_b, pub_b);

    CHECK(xlc_ed25519_key_exchange(sec_a, pub_b, sk_a), "our side of the secret");
    CHECK(xlc_ed25519_key_exchange(sec_b, pub_a, sk_b), "theirs");
    CHECK(memcmp(sec_a, sec_b, 32) == 0, "one secret");

    uint8_t payload[MC_PAYLOAD_MAX];
    int n = mc_dm_build(sec_a, mc_node_hash(pub_b), mc_node_hash(pub_a),
                        1789900123u, 0, "meet at the quay at six", payload,
                        sizeof payload);
    CHECK(n > 4 && (n - 4) % 16 == 0, "dest, src, MAC, cipher blocks: %d", n);
    CHECK(payload[0] == pub_b[0] && payload[1] == pub_a[0],
          "addressed by the first byte of each key");

    mc_text_t t;
    CHECK(mc_dm_open(sec_b, payload, n, &t), "the other side opens it");
    CHECK(strcmp(t.text, "meet at the quay at six") == 0, "the words: %s", t.text);
    CHECK(t.timestamp == 1789900123u && t.txt_type == 0, "timestamp and type");

    /* The same for a direct message, which has one byte less room. */
    char big[300];
    memset(big, 'w', sizeof big);
    big[sizeof big - 1] = 0;
    n = mc_dm_build(sec_a, mc_node_hash(pub_b), mc_node_hash(pub_a),
                    1789900123u, 0, big, payload, sizeof payload);
    CHECK(n > 0 && n <= MC_PAYLOAD_MAX, "a long direct message still goes: %d", n);
    CHECK(mc_dm_open(sec_b, payload, n, &t), "and opens");
    CHECK((int)strlen(t.text) == MC_TEXT_MAX, "shortened to what fits: %d",
          (int)strlen(t.text));

    uint8_t wrong[32];
    memset(wrong, 7, sizeof wrong);
    CHECK(!mc_dm_open(wrong, payload, n, &t), "another secret does not");

    /* The acknowledgement names nothing: it is a checksum over what was
     * said, so both ends compute the same four bytes. */
    uint32_t c1 = mc_ack_checksum(t.timestamp, 0, t.text, pub_a);
    uint32_t c2 = mc_ack_checksum(t.timestamp, 0, t.text, pub_a);
    uint32_t c3 = mc_ack_checksum(t.timestamp, 0, "other words", pub_a);
    uint32_t c4 = mc_ack_checksum(t.timestamp, 1, t.text, pub_a);
    CHECK(c1 == c2 && c1 != c3, "the ack checksum");
    CHECK(c1 != c4, "and the attempt byte is part of it, as MeshCore hashes it");
}

static void test_advert(void)
{
    uint8_t sk[64], pub[32], payload[MC_PAYLOAD_MAX];
    mc_node_keys("X3DCK0", 6, sk, pub);
    int n = mc_advert_build(sk, pub, 1789900000u, MC_ADV_CHAT, "roof X3DCK0",
                            payload, sizeof payload);
    CHECK(n == 32 + 4 + 64 + 1 + 11, "key, timestamp, signature, flags, name: %d", n);

    mc_advert_t a;
    CHECK(mc_advert_open(payload, n, &a), "it verifies");
    CHECK(memcmp(a.pub, pub, 32) == 0, "the key it names");
    CHECK(a.timestamp == 1789900000u, "the timestamp");
    CHECK((a.flags & MC_ADV_CHAT) && (a.flags & MC_ADV_HAS_NAME), "flags");
    CHECK(strcmp(a.name, "roof X3DCK0") == 0, "the name: %s", a.name);

    /* The signature is what makes the name believable, so a changed name
     * must not verify. */
    payload[105] ^= 0x20;
    CHECK(!mc_advert_open(payload, n, &a), "a changed name does not verify");
    payload[105] ^= 0x20;
    payload[40] ^= 0x01;
    CHECK(!mc_advert_open(payload, n, &a), "nor a changed signature");

    /* OpenSSL agrees with the signature we produced. */
    payload[40] ^= 0x01;
    uint8_t signed_part[32 + 4 + 1 + 31];
    memcpy(signed_part, payload, 36);
    memcpy(signed_part + 36, payload + 100, (size_t)(n - 100));
    EVP_PKEY *vk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(md, NULL, NULL, NULL, vk);
    CHECK(EVP_DigestVerify(md, payload + 36, 64, signed_part,
                           (size_t)(36 + n - 100)) == 1,
          "OpenSSL verifies the advert");
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(vk);

    /* THE APP DATA'S OPTIONAL BLOCKS, which is what a real node's advert
     * carries and what this firmware got wrong until one was read off the
     * air: the name comes after the location and the feature words, not
     * after the flags byte (mc.h). Built here by hand, the way MeshCore's
     * AdvertDataHelpers.cpp writes it. */
    {
        uint8_t app[64];
        int k = 0;
        app[k++] = MC_ADV_REPEATER | MC_ADV_LATLON | MC_ADV_FEAT1 | MC_ADV_HAS_NAME;
        int32_t lat = 38736946, lon = -9142685;      /* Lisbon, times 1e6 */
        for (int b = 0; b < 4; b++) app[k++] = (uint8_t)(((uint32_t)lat >> (8 * b)) & 0xFF);
        for (int b = 0; b < 4; b++) app[k++] = (uint8_t)(((uint32_t)lon >> (8 * b)) & 0xFF);
        app[k++] = 0x34; app[k++] = 0x12;            /* feat1 = 0x1234 */
        const char *nm = "Heltec Repeater";
        memcpy(app + k, nm, strlen(nm));
        k += (int)strlen(nm);

        uint8_t signed_part[32 + 4 + 64];
        memcpy(signed_part, pub, 32);
        uint32_t ts = 1789900000u;
        for (int b = 0; b < 4; b++) signed_part[32 + b] = (uint8_t)((ts >> (8 * b)) & 0xFF);
        memcpy(signed_part + 36, app, (size_t)k);
        uint8_t sig2[64];
        xlc_ed25519_sign(sig2, signed_part, (size_t)(36 + k), sk, pub);
        uint8_t pl[MC_PAYLOAD_MAX];
        memcpy(pl, pub, 32);
        memcpy(pl + 32, signed_part + 32, 4);
        memcpy(pl + 36, sig2, 64);
        memcpy(pl + 100, app, (size_t)k);

        mc_advert_t b2;
        CHECK(mc_advert_open(pl, 100 + k, &b2), "an advert with every block");
        CHECK((b2.flags & MC_ADV_TYPE) == MC_ADV_REPEATER, "the type is the low nibble");
        CHECK(b2.lat == lat && b2.lon == lon, "the location: %d %d", (int)b2.lat, (int)b2.lon);
        CHECK(b2.feat1 == 0x1234, "the feature word: %04x", b2.feat1);
        CHECK(strcmp(b2.name, "Heltec Repeater") == 0,
              "and the name AFTER them, which is the whole point: %s", b2.name);
    }

    /* And the callsign a MeshCore node wears in XPRS. */
    char call[11];
    mc_call_of_pub(pub, call);
    CHECK(strlen(call) == 10 && call[0] == 'M' && call[1] == 'C',
          "MC and eight hex: %s", call);
    CHECK(xprs_is_foreign_call(call, 10), "which the codec already knows");
}

static void test_xprs_frames(void)
{
    const char *wire = "t:message f:X1QZ3N ts:2026-09-20_09:00:00 m:one frame";
    int len = (int)strlen(wire);
    uint8_t frames[2][MC_FRAME_MAX];
    int fl[2];
    CHECK(mc_xprs_frames_for(len) == 1, "a short wire is one frame");
    CHECK(mc_xprs_wrap(wire, len, frames, fl) == 1, "and it wraps");
    CHECK(fl[0] == mc_xprs_frame_len(len, 0), "the ledger charges what was aired");

    mc_pkt_t p;
    CHECK(mc_parse(frames[0], fl[0], &p), "it is a MeshCore frame");
    CHECK(p.type == MC_PT_RAW_CUSTOM && p.route == MC_ROUTE_FLOOD,
          "flood-routed RAW_CUSTOM, which a repeater carries");

    mc_reasm_t r;
    memset(&r, 0, sizeof r);
    char back[XPRS_MAX_WIRE + 1];
    int n = mc_xprs_unwrap(&r, frames[0], fl[0], 1000, back, sizeof back);
    CHECK(n == len && strcmp(back, wire) == 0, "and unwraps to the same wire");

    /* A long one goes as two, in either order, and only then. */
    char big[XPRS_MAX_WIRE + 1];
    int bl = snprintf(big, sizeof big, "t:message f:X1QZ3N ts:2026-09-20_09:00:00 m:%s",
                      "wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww"
                      " wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww"
                      " wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww wwwwwwwww");
    CHECK(bl > MC_XPRS_ONE_FRAME, "a wire past one frame: %d", bl);
    CHECK(mc_xprs_wrap(big, bl, frames, fl) == 2, "goes as two");
    memset(&r, 0, sizeof r);
    CHECK(mc_xprs_unwrap(&r, frames[1], fl[1], 2000, back, sizeof back) == 0,
          "the second half alone is not a message");
    n = mc_xprs_unwrap(&r, frames[0], fl[0], 2000, back, sizeof back);
    CHECK(n == bl && strcmp(back, big) == 0, "the pair is: %d", n);

    /* A half whose sibling never comes is forgotten. */
    memset(&r, 0, sizeof r);
    mc_xprs_unwrap(&r, frames[0], fl[0], 3000, back, sizeof back);
    CHECK(mc_xprs_unwrap(&r, frames[1], fl[1], 3000 + MC_XPRS_FRAG_TTL_MS + 1,
                         back, sizeof back) == 0,
          "after the TTL the other half is a stranger");

    /* Two stations airing one packet air the same bytes. */
    uint8_t other[2][MC_FRAME_MAX];
    int ol[2];
    mc_xprs_wrap(big, bl, other, ol);
    CHECK(ol[0] == fl[0] && memcmp(other[0], frames[0], (size_t)fl[0]) == 0,
          "byte for byte, whoever airs it");

    /* Somebody else's frame is somebody else's. */
    uint8_t adv[MC_FRAME_MAX];
    uint8_t sk[64], pub[32], ap[MC_PAYLOAD_MAX];
    mc_node_keys("X3H3MZ", 6, sk, pub);
    int an = mc_advert_build(sk, pub, 1789900000u, MC_ADV_CHAT, "n", ap, sizeof ap);
    mc_pkt_t q;
    memset(&q, 0, sizeof q);
    q.route = MC_ROUTE_FLOOD;
    q.type = MC_PT_ADVERT;
    q.hash_size = 1;
    q.payload = ap;
    q.payload_len = an;
    int qn = mc_build(&q, adv, sizeof adv);
    CHECK(mc_xprs_unwrap(&r, adv, qn, 4000, back, sizeof back) == -1,
          "an advert is not XPRS, and says so");
}

/* The mesh harness (test_mc_mesh_host.c) reports its failures here. */
void mc_test_fail(const char *file, int line, const char *what);
void mc_test_fail(const char *file, int line, const char *what)
{
    printf("FAIL %s:%d  %s\n", file, line, what);
    g_fail++;
}
void test_mesh(void);

int main(void)
{
    test_header();
    test_channel();
    test_dm();
    test_advert();
    test_xprs_frames();
    test_mesh();
    if (g_fail) {
        printf("xprs_meshcore: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("xprs_meshcore: all checks passed\n");
    return 0;
}
