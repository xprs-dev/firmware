/*
 * Host test for opening sealed bodies, against bodies reticulum-dart sealed
 * (tool/gen_seal_vectors.dart).
 *
 * The IV is random, so C cannot reproduce Dart's bytes; it has to OPEN them,
 * which is the property a station needs: a phone sealed a password with the
 * app's code, and the station reads it with this. Getting it back exactly
 * checks the ECDH, the bare X coordinate as the key, AES-256-CBC, the padding
 * and the base64url at once. The negatives check that a body the station was
 * not meant to read does not open as something plausible.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "xprssig.h"
#include "xprsseal.h"

static int g_checks, g_fail;
#define CHECK(cond, fmt, ...) do {                                            \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++;                                                  \
        printf("  FAIL %s:%d  " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); } \
} while (0)

/* ── vectors from reticulum-dart (tool/gen_seal_vectors.dart) ────────────── */
/* Toy keys: 0x11.. is the phone, an owner; 0x22.. is the station. */
static const char *V_PHONE_PRIV   = "1111111111111111111111111111111111111111111111111111111111111111";
static const char *V_PHONE_X      = "4f355bdcb7cc0af728ef3cceb9615d90684bb5b2ca5f859ab0f0b704075871aa";
static const char *V_STATION_PRIV = "2222222222222222222222222222222222222222222222222222222222222222";
static const char *V_STATION_X    = "466d7fcae563e5cb09a0d1870bb580344804617879a14949cf22285f1bae3f27";
static const char *V_SHARED       = "77e0510d5042e2f5e9e59c977b81eeed590cf7d20c1c51da451a8eaa9fdc45ff";

static const struct { const char *plain, *x; } V[] = {
    /* XPRS.md 11.10's own example: a network name and a password with
     * spaces in both, 52 bytes, 107 characters sealed. */
    { "cmd:set\nssid:Casa do Mar\npass:sardinha na brasa 2026",
      "7YliRWjjpFYKgTZJHqW15uUZROnt3dKAbVHo0rzkl6QZIcHAZGOmlH4YLlvurWrPoV8hU7l0O4B2V8I5A9jXGOlyZ-2diyGYQpdKjukJMVE" },
    /* The longest WPA2 passphrase, 63 characters: 128 sealed. */
    { "cmd:set\npass:pppppppppppppppppppppppppppppppQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ",
      "Jk5nTSgmsx_8bF2XlTEu2k5EJzlfL8NW_hvV5hd8QxpuzSQjIP2E8VcPkqF6iI6OxFcmINoC7-sbNks_llntl_OEDi3zqDNBttctc5egkkp5o5cGwluMWXmQJ7lFKEjG" },
    /* An imported key, 76 bytes: the largest thing 11.10 seals. */
    { "cmd:set\nnsec:nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq",
      "eK9m9_OV26HAJz67zut-MXtoGT_Xeo3UndJmFzKRV9uDHxCv5gyuZ-ZhhR2K0O53-v6GGJ9VnkFS_HULjIJ-cG-EWNq3wld1CbfA8p-INAP0cg7BMyPnjVqzHPC58av1" },
};

static void unhex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_keys_and_shared_secret(void)
{
    uint8_t phone[32], station[32], px[32], sx[32], want[32], a[32], b[32];
    unhex(V_PHONE_PRIV, phone, 32);
    unhex(V_STATION_PRIV, station, 32);
    unhex(V_SHARED, want, 32);

    CHECK(xprssig_public_key(phone, px), "phone public key");
    CHECK(xprssig_public_key(station, sx), "station public key");
    unhex(V_PHONE_X, a, 32);
    CHECK(memcmp(px, a, 32) == 0, "phone x matches reticulum-dart");
    unhex(V_STATION_X, a, 32);
    CHECK(memcmp(sx, a, 32) == 0, "station x matches reticulum-dart");

    /* Both directions give the secret Dart computed: the whole point of 6.2
     * is that neither end sends it. */
    CHECK(xprssig_ecdh_x(station, px, a), "station side");
    CHECK(xprssig_ecdh_x(phone, sx, b), "phone side");
    CHECK(memcmp(a, want, 32) == 0, "station's secret is Dart's");
    CHECK(memcmp(b, want, 32) == 0, "phone's secret is Dart's");
}

static void test_opens_what_dart_sealed(void)
{
    uint8_t station[32], px[32], out[128];
    unhex(V_STATION_PRIV, station, 32);
    unhex(V_PHONE_X, px, 32);
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        memset(out, 0xAA, sizeof out);
        int n = xprsseal_open(station, px, V[i].x, strlen(V[i].x), out, sizeof out);
        CHECK(n == (int)strlen(V[i].plain), "vector %zu: length %d", i, n);
        CHECK(n > 0 && memcmp(out, V[i].plain, (size_t)n) == 0 && out[n] == 0,
              "vector %zu: plaintext", i);
    }
}

static void test_refuses_what_it_should(void)
{
    uint8_t station[32], phone[32], px[32], sx[32], out[128];
    unhex(V_STATION_PRIV, station, 32);
    unhex(V_PHONE_PRIV, phone, 32);
    unhex(V_PHONE_X, px, 32);
    unhex(V_STATION_X, sx, 32);
    const char *x = V[0].x;
    size_t n = strlen(x);
    char bad[256];

    /* A flipped byte in the last block ruins the padding. */
    memcpy(bad, x, n + 1);
    bad[n - 3] = bad[n - 3] == 'A' ? 'B' : 'A';
    CHECK(xprsseal_open(station, px, bad, n, out, sizeof out) < 0, "tampered tail");

    /* Sealed to the station, opened as if from the station itself: the wrong
     * key, which decrypts to noise and must not pass as plaintext. */
    CHECK(xprsseal_open(station, sx, x, n, out, sizeof out) < 0, "wrong sender key");
    /* The phone cannot open what it sent with its own key as the peer. */
    CHECK(xprsseal_open(phone, px, x, n, out, sizeof out) < 0, "wrong recipient");

    /* Not the shape 6.2 says `x:` has. */
    CHECK(xprsseal_open(station, px, "abc", 3, out, sizeof out) < 0, "too short");
    CHECK(xprsseal_open(station, px, x, n - 1, out, sizeof out) < 0, "not whole blocks");
    memcpy(bad, x, n + 1);
    bad[5] = '+';
    CHECK(xprsseal_open(station, px, bad, n, out, sizeof out) < 0, "base64, not base64url");
    CHECK(xprsseal_open(station, px, x, n, out, 16) < 0, "no room");

    /* A peer x that is not on the curve (x = 5 has no y on secp256k1). */
    uint8_t off[32] = {0};
    off[31] = 5;
    CHECK(xprsseal_open(station, off, x, n, out, sizeof out) < 0, "peer off the curve");
}

int main(void)
{
    test_keys_and_shared_secret();
    test_opens_what_dart_sealed();
    test_refuses_what_it_should();
    printf("xprsseal: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
