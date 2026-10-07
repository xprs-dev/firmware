/*
 * The station basics shared by the nRF52840 boards (see nrf_station.h).
 * Lifted out of models/sensecap-p1-pro/firmware/src/main.cpp unchanged in
 * behaviour, so the P1-Pro and the T1000-E cannot drift on the key, the
 * callsign, the config file or the clock.
 */
#include <Arduino.h>
#include <time.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

#include "nrf_station.h"

extern "C" {
#include "xprssig.h"
#include "xprsid.h"
#include "xprs_auth.h"
#include "bech32.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
}
using namespace Adafruit_LittleFS_Namespace;

#define KEY_PATH   "/xprs/key"
#define BOOT_PATH  "/xprs/boot"
#define CFG_PATH   "/xprs/cfg"
#define CFG_MAX    12

static nst_cfg_t s_cfg = { "X3", false };
static char      s_call[16];
static uint8_t   s_priv[XPRSSIG_KEY_LEN];
static uint8_t   s_pub[XPRSSIG_KEY_LEN];
static char      s_npub[80];
static bool      s_have_key;
static uint32_t  s_boot_epoch;

/* Entropy for the signer (xprssig.h). With the SoftDevice up, NRF_RNG is
 * its and the application asks it; before that the peripheral is ours. */
extern "C" void xprssig_platform_random(uint8_t *out, size_t len)
{
    uint8_t sd_on = 0;
    sd_softdevice_is_enabled(&sd_on);
    size_t done = 0;
    while (done < len) {
        if (sd_on) {
            uint8_t avail = 0;
            sd_rand_application_bytes_available_get(&avail);
            uint8_t take = avail;
            if (take > len - done) take = (uint8_t)(len - done);
            if (take == 0) { delay(1); continue; }
            if (sd_rand_application_vector_get(out + done, take) == NRF_SUCCESS) done += take;
        } else {
            NRF_RNG->TASKS_START = 1;
            while (!NRF_RNG->EVENTS_VALRDY) { }
            NRF_RNG->EVENTS_VALRDY = 0;
            out[done++] = (uint8_t)NRF_RNG->VALUE;
            NRF_RNG->TASKS_STOP = 1;
        }
    }
}

static bool file_read(const char *path, uint8_t *buf, size_t n)
{
    File f(InternalFS);
    if (!f.open(path, FILE_O_READ)) return false;
    size_t got = f.read(buf, n);
    f.close();
    return got == n;
}

static bool file_write(const char *path, const uint8_t *buf, size_t n)
{
    InternalFS.remove(path);
    File f(InternalFS);
    if (!f.open(path, FILE_O_WRITE)) return false;
    size_t put = f.write(buf, n);
    f.close();
    return put == n;
}

/* XPRS 3: the prefix, then the four characters after "npub1", uppercased --
 * what nostr_keys_derive_callsign() does on the ESP32 boards, so a receiver
 * can re-derive the callsign from the key. */
static void derive(const char *npub, char *out)
{
    out[0] = s_cfg.prefix[0]; out[1] = s_cfg.prefix[1];
    for (int i = 0; i < 4; i++) out[2 + i] = (char)toupper((unsigned char)npub[5 + i]);
    out[6] = 0;
}

static bool key_adopt(void)
{
    if (!xprssig_public_key(s_priv, s_pub)) return false;
    if (bech32_encode("npub", s_pub, sizeof s_pub, s_npub, sizeof s_npub) != ESP_OK) return false;
    s_have_key = true;
    derive(s_npub, s_call);
    return true;
}

/*
 * A volume another firmware left behind (Meshtastic keeps its LittleFS in
 * the same place, in the same format) mounts and may have no room left.
 * With format_if_full, a write that will not land clears it, loudly; the
 * other firmware's identity is gone if the board goes back to it.
 */
static void fs_begin(void)
{
    if (!InternalFS.begin()) {
        Serial.println("fs: would not mount -- formatting");
        InternalFS.format();
        InternalFS.begin();
    }
    InternalFS.mkdir("/xprs");
    if (!s_cfg.format_if_full || InternalFS.exists(KEY_PATH)) return;
    static const uint8_t probe[XPRSSIG_KEY_LEN] = {0};
    if (file_write("/xprs/probe", probe, sizeof probe)) {
        InternalFS.remove("/xprs/probe");
        return;
    }
    Serial.println("fs: FULL (another firmware's files?) -- formatting; that "
                   "firmware's identity on this board is gone");
    InternalFS.format();
    InternalFS.begin();
    InternalFS.mkdir("/xprs");
}

void nst_init(const nst_cfg_t *cfg)
{
    if (cfg) s_cfg = *cfg;
    if (!s_cfg.prefix || strlen(s_cfg.prefix) != 2) s_cfg.prefix = "X3";
    fs_begin();

    /* 15.7: the boots ordinal, so a clockless station's packets can still
     * be ordered by a receiver. */
    uint8_t b[4] = {0};
    if (file_read(BOOT_PATH, b, 4))
        s_boot_epoch = (uint32_t)b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    s_boot_epoch++;
    b[0] = s_boot_epoch; b[1] = s_boot_epoch >> 8; b[2] = s_boot_epoch >> 16; b[3] = s_boot_epoch >> 24;
    file_write(BOOT_PATH, b, 4);

    if (file_read(KEY_PATH, s_priv, sizeof s_priv) && key_adopt()) {
        Serial.printf("key: loaded, callsign %s\n", s_call);
        return;
    }
    if (!xprssig_generate(s_priv) || !key_adopt()) {
        Serial.println("key: could not generate -- this station will not sign");
        s_have_key = false;
        snprintf(s_call, sizeof s_call, "%s????", s_cfg.prefix);
        return;
    }
    bool kept = file_write(KEY_PATH, s_priv, sizeof s_priv);
    Serial.printf("key: generated, callsign %s -- %s\n", s_call, kept ? "kept" : "NOT SAVED");
}

const char    *nst_call(void)       { return s_call; }
const char    *nst_npub(void)       { return s_have_key ? s_npub : ""; }
bool           nst_have_key(void)   { return s_have_key; }
uint32_t       nst_boot_epoch(void) { return s_boot_epoch; }
const uint8_t *nst_priv(void)       { return s_priv; }

int nst_sign(char *wire, int len, int cap)
{
    if (!s_have_key) return len;
    return xprsid_sign(wire, len, cap, s_priv);
}

void nst_key_import(const char *nsec)
{
    char hrp[8]; uint8_t priv[64]; size_t n = sizeof priv;
    if (bech32_decode(nsec, hrp, priv, &n) != ESP_OK || n != 32 || strcmp(hrp, "nsec") != 0) {
        Serial.println("import: not an nsec"); return;
    }
    uint8_t keep[32]; memcpy(keep, s_priv, 32);
    memcpy(s_priv, priv, 32);
    if (!key_adopt()) { memcpy(s_priv, keep, 32); key_adopt(); Serial.println("import: not a valid key"); return; }
    Serial.printf("import: now %s -- writing and rebooting\n", s_call);
    Serial.flush(); delay(50);
    sd_softdevice_disable();
    file_write(KEY_PATH, s_priv, sizeof s_priv);
    NVIC_SystemReset();
}

/* ── Config ─────────────────────────────────────────────────────────── */
static struct { char key[12]; char val[96]; } s_kv[CFG_MAX];

void nst_cfg_load(void)
{
    File f(InternalFS);
    if (!f.open(CFG_PATH, FILE_O_READ)) return;
    static char buf[CFG_MAX * 110];
    int n = f.read((uint8_t *)buf, sizeof buf - 1);
    f.close();
    if (n <= 0) return;
    buf[n] = 0;
    int k = 0;
    for (char *line = strtok(buf, "\n"); line && k < CFG_MAX; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        snprintf(s_kv[k].key, sizeof s_kv[k].key, "%s", line);
        snprintf(s_kv[k].val, sizeof s_kv[k].val, "%s", eq + 1);
        k++;
    }
}

extern "C" const char *xcfg_get(const char *key, const char *def)
{
    for (int i = 0; i < CFG_MAX; i++)
        if (s_kv[i].key[0] && strcmp(s_kv[i].key, key) == 0) return s_kv[i].val;
    return def;
}

void nst_cfg_console(char *line)
{
    char *cmd = strtok(line, " "), *key = strtok(NULL, " "), *val = strtok(NULL, "");
    if (!cmd) return;
    if (strcmp(cmd, "list") == 0) {
        for (int i = 0; i < CFG_MAX; i++)
            if (s_kv[i].key[0]) Serial.printf("%s=%s\n", s_kv[i].key, s_kv[i].val);
        return;
    }
    if (!key) { Serial.println("cfg: set <key> <value> | get <key> | list"); return; }
    if (strcmp(cmd, "get") == 0) { Serial.printf("%s=%s\n", key, xcfg_get(key, "")); return; }
    if (strcmp(cmd, "set") != 0) return;
    int slot = -1;
    for (int i = 0; i < CFG_MAX; i++) {
        if (strcmp(s_kv[i].key, key) == 0) { slot = i; break; }
        if (slot < 0 && !s_kv[i].key[0]) slot = i;
    }
    if (slot < 0) { Serial.println("cfg: full"); return; }
    snprintf(s_kv[slot].key, sizeof s_kv[slot].key, "%s", key);
    snprintf(s_kv[slot].val, sizeof s_kv[slot].val, "%s", val ? val : "");
    static char out[CFG_MAX * 110];
    int n = 0;
    for (int i = 0; i < CFG_MAX; i++)
        if (s_kv[i].key[0] && s_kv[i].val[0])
            n += snprintf(out + n, sizeof out - n, "%s=%s\n", s_kv[i].key, s_kv[i].val);
    Serial.printf("cfg: %s set -- writing and rebooting\n", key);
    Serial.flush(); delay(50);
    sd_softdevice_disable();
    file_write(CFG_PATH, (const uint8_t *)out, (size_t)n);
    NVIC_SystemReset();
}

/* What xprs_auth takes from the ESP32 stack, supplied here. */
extern "C" esp_err_t nostr_keys_derive_callsign(const char *npub, char *callsign)
{
    if (!npub || !callsign || strlen(npub) < 9 || strncmp(npub, "npub1", 5) != 0) return ESP_ERR_INVALID_ARG;
    derive(npub, callsign);
    return ESP_OK;
}

/* ── The clock ──────────────────────────────────────────────────────── */
static uint32_t s_clock_epoch, s_clock_set_ms;

uint32_t nst_now(void)
{
    return s_clock_epoch ? s_clock_epoch + (millis() - s_clock_set_ms) / 1000 : 0;
}

extern "C" uint32_t xauth_platform_now(void) { return nst_now(); }

uint32_t nst_ts_to_epoch(const char *ts)
{
    int y, mo, d, h, mi, se;
    if (sscanf(ts, "%4d-%2d-%2d_%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + h * 3600 + mi * 60 + se);
}

bool nst_clock_set(uint32_t epoch, const char *source)
{
    if (epoch < 1700000000u || epoch <= nst_now()) return false;
    bool first = s_clock_epoch == 0;
    s_clock_epoch = epoch;
    s_clock_set_ms = millis();
    if (first) Serial.printf("clock: set from %s\n", source ? source : "?");
    return true;
}

/* No RTC: the clock is whatever the newest signed owner command says (11.4),
 * forward only, or a better source through nst_clock_set(). */
void nst_clock_learn(const xprs_t *p)
{
    char from[16] = "", ts[24] = "";
    if (!xprs_get_str(p, "f", from, sizeof from) || !xprs_get_str(p, "ts", ts, sizeof ts)) return;
    if (xprs_get(p, "sig", NULL) == NULL || xprs_get(p, "via", NULL) != NULL) return;
    uint8_t pub[32];
    if (!xauth_owner_key_of(from, pub) || !xprsid_verify(p, pub)) return;
    nst_clock_set(nst_ts_to_epoch(ts), from);
}

int nst_time_field(char *out, int cap)
{
    uint32_t now = nst_now();
    if (now) {
        time_t t = (time_t)now;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        return (int)strftime(out, (size_t)cap, "ts:%Y-%m-%d_%H:%M:%S", &tmv);
    }
    return snprintf(out, (size_t)cap, "epoch:%lu.%lu",
                    (unsigned long)s_boot_epoch, (unsigned long)(millis() / 1000));
}
