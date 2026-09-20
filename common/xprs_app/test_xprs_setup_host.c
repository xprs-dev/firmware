/*
 * Host test for XPRS.md 11.10's words (xprs_setup.h): the lines a sealed
 * body carries, and the values a station accepts for each key. What a
 * station does with them is xprs_app.c's and needs a radio; whether they are
 * well formed does not, and that is where a phone and a station first
 * disagree.
 */
#include <stdio.h>
#include <string.h>

#include "xprs_setup.h"

static int g_checks, g_fail;
#define CHECK(cond, fmt, ...) do {                                            \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++;                                                  \
        printf("  FAIL %s:%d  " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); } \
} while (0)

static void test_lines(void)
{
    xsetup_kv_t kv[XSETUP_KV_MAX];

    /* 11.10's own example: both values keep their spaces. */
    int n = xsetup_lines("cmd:set\nssid:Casa do Mar\npass:sardinha na brasa 2026",
                         kv, XSETUP_KV_MAX);
    CHECK(n == 2, "two fields, got %d", n);
    CHECK(strcmp(kv[0].key, "ssid") == 0 && strcmp(kv[0].val, "Casa do Mar") == 0,
          "ssid kept its spaces: '%s'", kv[0].val);
    CHECK(strcmp(kv[1].key, "pass") == 0 &&
          strcmp(kv[1].val, "sardinha na brasa 2026") == 0, "pass: '%s'", kv[1].val);

    /* A colon inside a value is the value's. */
    n = xsetup_lines("cmd:set\npass:a:b:c:d:e:f", kv, XSETUP_KV_MAX);
    CHECK(n == 1 && strcmp(kv[0].val, "a:b:c:d:e:f") == 0, "colons: '%s'", kv[0].val);

    /* CRLF from a phone that thinks in Windows, and a trailing newline. */
    n = xsetup_lines("cmd:set\r\nnick:roof\r\n", kv, XSETUP_KV_MAX);
    CHECK(n == 1 && strcmp(kv[0].val, "roof") == 0, "crlf: n=%d '%s'", n, kv[0].val);

    /* A bare cmd:set says nothing, and that is not an error here. */
    CHECK(xsetup_lines("cmd:set", kv, XSETUP_KV_MAX) == 0, "bare cmd:set");

    /* 11.4: a body that is not a cmd:set is a 400. */
    CHECK(xsetup_lines("ssid:x\npass:yyyyyyyy", kv, XSETUP_KV_MAX) < 0, "no cmd:");
    CHECK(xsetup_lines("cmd:update\nver:1", kv, XSETUP_KV_MAX) < 0, "not set");
    CHECK(xsetup_lines("cmd:settle\nnick:a", kv, XSETUP_KV_MAX) < 0, "cmd:settle");
    CHECK(xsetup_lines("cmd:set nick:roof", kv, XSETUP_KV_MAX) < 0,
          "packet grammar on one line is not lines");
    CHECK(xsetup_lines("cmd:set\njunk", kv, XSETUP_KV_MAX) < 0, "line without a colon");
    CHECK(xsetup_lines("cmd:set\nPass:x", kv, XSETUP_KV_MAX) < 0, "uppercase key");
    CHECK(xsetup_lines("cmd:set\n:x", kv, XSETUP_KV_MAX) < 0, "empty key");
    char longv[128] = "cmd:set\npass:";
    memset(longv + strlen(longv), 'p', 70);
    CHECK(xsetup_lines(longv, kv, XSETUP_KV_MAX) < 0, "value too long");
    CHECK(xsetup_lines("cmd:set\na:1\nb:2\nc:3", kv, 2) < 0, "more than max");
}

static void test_keys(void)
{
    CHECK(xsetup_is_key("ssid") && xsetup_is_key("ap") && xsetup_is_key("key"),
          "setup keys");
    CHECK(!xsetup_is_key("owner") && !xsetup_is_key("use") && !xsetup_is_key("cmd"),
          "policy keys are not setup keys");
    CHECK(xsetup_is_secret("pass") && xsetup_is_secret("nsec") &&
          xsetup_is_secret("ssid"), "secrets");
    CHECK(!xsetup_is_secret("nick") && !xsetup_is_secret("wifi"), "not secrets");
}

static void test_values(void)
{
    CHECK(!xsetup_check("ssid", "Casa do Mar"), "ssid with spaces");
    CHECK(!xsetup_check("ssid", "abcdefghijklmnopqrstuvwxyz012345"), "32-byte ssid");
    CHECK(xsetup_check("ssid", "abcdefghijklmnopqrstuvwxyz0123456"), "33-byte ssid");
    CHECK(xsetup_check("ssid", ""), "empty ssid");

    CHECK(!xsetup_check("pass", "12345678"), "8-char pass");
    CHECK(xsetup_check("pass", "1234567"), "7-char pass");
    char p63[64];
    memset(p63, 'x', 63); p63[63] = 0;
    CHECK(!xsetup_check("pass", p63), "63-char pass");
    char h64[65];
    memset(h64, 'a', 64); h64[64] = 0;
    CHECK(xsetup_check("pass", h64), "64 characters is past a passphrase");
    CHECK(xsetup_check("pass", "tab\there!"), "control char in pass");

    CHECK(!xsetup_check("nsec", "nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq"),
          "nsec shape");
    CHECK(xsetup_check("nsec", "npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq"),
          "an npub is not an nsec");

    CHECK(!xsetup_check("wifi", "join") && !xsetup_check("wifi", "off"), "wifi");
    CHECK(xsetup_check("wifi", "on"), "wifi:on is not a word");
    CHECK(!xsetup_check("ap", "on") && !xsetup_check("ap", "off"), "ap");
    CHECK(!xsetup_check("key", "new") && xsetup_check("key", "old"), "key");

    CHECK(!xsetup_check("nick", "roof-north"), "nick");
    CHECK(!xsetup_check("nick", "a_b-C9"), "nick chars");
    CHECK(xsetup_check("nick", "roof north"), "nick with a space");
    CHECK(xsetup_check("nick", "abcdefghijklmnopq"), "17-char nick");

    CHECK(!xsetup_check("zone", "auto"), "zone auto");
    CHECK(!xsetup_check("zone", "+01:00") && !xsetup_check("zone", "-00:30") &&
          !xsetup_check("zone", "+05:45"), "offsets");
    CHECK(xsetup_check("zone", "Europe/Berlin"), "a zone name is not an offset");
    CHECK(xsetup_check("zone", "+15:00"), "past +14:00");

    CHECK(!xsetup_check("lora", "xprs") && !xsetup_check("lora", "meshtastic") &&
          !xsetup_check("lora", "meshcore"), "the three LoRa modes");

    /* 14.8: the channel itself, because not every board is an 868 MHz
     * board and a 433 community picks its own. */
    CHECK(!xsetup_check("freq", "433.9"), "MHz with a decimal point");
    CHECK(!xsetup_check("freq", "869618000"), "or plain Hz");
    CHECK(!xsetup_check("freq", "preset"), "or back to the region's own");
    CHECK(!xsetup_check("freq", "433.900MHz"),
          "and the shape section 14 writes a frequency in");
    CHECK(xsetup_check("freq", "MHz"), "a suffix on its own is not one");
    CHECK(xsetup_check("freq", "70"), "70 MHz is not something it can tune");
    CHECK(xsetup_check("freq", "2400"), "nor 2.4 GHz");
    CHECK(xsetup_check("freq", "433,9"), "a comma is not a decimal point");
    CHECK(xsetup_check("freq", "433.9.1"), "nor two points");
    CHECK(xsetup_check("freq", ""), "and something must be said");
    CHECK(!xsetup_check("region", "eu-433"), "a preset name");
    CHECK(!xsetup_check("region", "eu"), "a short one");
    CHECK(xsetup_check("region", "EU"), "lowercase, as the table has it");
    CHECK(xsetup_check("region", "e"), "and long enough to mean something");
    CHECK(xsetup_is_key("freq") && xsetup_is_key("region"),
          "both are setup keys");
    CHECK(xsetup_check("lora", "lorawan") && xsetup_check("lora", "XPRS"),
          "not a LoRa mode");
    CHECK(xsetup_is_key("lora") && !xsetup_is_secret("lora"), "lora is a clear key");

    CHECK(xsetup_check("owner", "X1QZ3N"), "not a setup key");
}

int main(void)
{
    test_lines();
    test_keys();
    test_values();
    printf("xprs_setup: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
