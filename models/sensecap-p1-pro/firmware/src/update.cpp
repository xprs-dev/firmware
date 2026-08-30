/*
 * Installing firmware on a pole -- see update.h for the shape, and the
 * three facts about this chip that shaped it:
 *
 *   FLASH, WHILE THE SOFTDEVICE RUNS, IS WRITTEN THROUGH IT. sd_flash_write
 *   and sd_flash_page_erase are asynchronous and report through a SoC
 *   event, which tn_gatt_pump() drains and hands to tn_soc_event() below.
 *   So a write here is: ask, then pump until the event says done. The
 *   core's LittleFS layer waits on that same event with a semaphore that
 *   nothing on this task can give (firmware/README.md), which is why the
 *   staging area is raw pages and not a file.
 *
 *   THE BOOTLOADER WILL NOT SWAP AN APPLICATION FOR US. Adafruit's
 *   bootloader copies bank 1 to bank 0 only inside its own DFU session;
 *   at boot it checks bank 0 against its settings page (a CRC, when one
 *   is recorded) or, with the settings page erased, accepts any image
 *   whose vector table is plausible. So the copy is done here, from a
 *   routine that runs out of RAM (it erases the flash it would otherwise
 *   be executing from), and it ends by erasing the settings page.
 *
 *   THERE IS NO SECOND SLOT, SO WE MAKE ONE. The region above the
 *   application is big enough for three images: what runs, what is
 *   arriving, and a copy of what ran before the last install. A new image
 *   is on probation until it has been up two minutes with its radio
 *   working; the proof is a bit in GPREGRET2, which survives a soft reset
 *   and not a power cycle, and the verdict is read at the next boot before
 *   anything else. Three boots without proof and the copy goes back.
 */

#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include "update.h"

extern "C" {
#include "xprs_auth.h"
#include "xprssig.h"
#include "tinynimble.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
}
using namespace Adafruit_LittleFS_Namespace;

/* The board's (main.cpp), as xprs_auth.c declares them. */
extern "C" const char *xcfg_get(const char *key, const char *def);
extern "C" uint32_t xauth_platform_now(void);

#ifndef XPRS_FW_VERSION
#define XPRS_FW_VERSION "0.0.0"
#endif

/* ── The flash map ───────────────────────────────────────────────────────
 *
 * The application region is 0x27000..0xED000 (the linker script); the
 * internal LittleFS sits above it at 0xED000, the bootloader at 0xF4000,
 * its settings page at 0xFF000. Three 256 KB slots fit under 0xED000:
 *
 *   0x27000  APP      what runs (the linker's FLASH origin)
 *   0x67000  STAGE    what is arriving
 *   0xA7000  BACKUP   what ran before the last install
 *   0xE7000  (free)   24 KB, then the filesystem
 *
 * An image is therefore at most 256 KB; this one is 150 KB. */
#define XFW_APP      0x27000u
#define XFW_STAGE    0x67000u
#define XFW_BACKUP   0xA7000u
#define XFW_SLOT     0x40000u
#define XFW_PAGE     4096u
#define XFW_SETTINGS 0xFF000u

#define XFW_CHUNK    160          /* bytes a packet carries: 200 base85 characters */
#define XFW_MAX_CHUNKS (XFW_SLOT / XFW_CHUNK + 1)
#define XFW_IDLE_MS  (30u * 60u * 1000u)
#define XFW_PROVE_MS (120u * 1000u)
#define XFW_BOOTS_TO_GIVE_UP 3
#define PROBATION_PATH "/xprs/probation"
#define GPREGRET2_PROVED 0x50      /* "this image ran two minutes with a radio" */

static xfw_cfg_t s_cfg;
static uint32_t  s_boot;
static bool      s_probation;
static bool      s_proved;
static char      s_rolled_back[24];   /* the version the last boot put back, for zdiag */

/* The image being received. */
static struct {
    bool     open;
    char     ver[24];
    uint32_t size;
    uint8_t  sha[32];
    char     sig85[XPRSSIG_B85_LEN + 1];
    uint32_t nchunks, got;
    uint8_t  have[(XFW_MAX_CHUNKS + 7) / 8];
    uint32_t last_ms;
    char     from[16], id[8];
    xb_t    *bearer;
} s_ses;

/* ── Flash through the SoftDevice ──────────────────────────────────────── */

static volatile int s_flash_evt;

extern "C" void tn_soc_event(uint32_t evt)
{
    if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) s_flash_evt = 1;
    else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) s_flash_evt = -1;
}

static bool sd_on(void)
{
    uint8_t on = 0;
    sd_softdevice_is_enabled(&on);
    return on != 0;
}

static bool flash_wait(void)
{
    for (uint32_t t0 = millis(); millis() - t0 < 3000; ) {
        tn_gatt_pump();
        if (s_flash_evt) return s_flash_evt > 0;
        delay(1);
    }
    return false;
}

static bool page_erase(uint32_t addr)
{
    if (!sd_on()) {
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
        while (!NRF_NVMC->READY) { }
        NRF_NVMC->ERASEPAGE = addr;
        while (!NRF_NVMC->READY) { }
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
        return true;
    }
    s_flash_evt = 0;
    uint32_t err;
    while ((err = sd_flash_page_erase(addr / XFW_PAGE)) == NRF_ERROR_BUSY) { tn_gatt_pump(); delay(1); }
    return err == NRF_SUCCESS && flash_wait();
}

static bool words_write(uint32_t addr, const uint32_t *src, uint32_t n)
{
    if (!sd_on()) {
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
        for (uint32_t i = 0; i < n; i++) {
            ((volatile uint32_t *)addr)[i] = src[i];
            while (!NRF_NVMC->READY) { }
        }
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
        return true;
    }
    s_flash_evt = 0;
    uint32_t err;
    while ((err = sd_flash_write((uint32_t *)addr, src, n)) == NRF_ERROR_BUSY) { tn_gatt_pump(); delay(1); }
    return err == NRF_SUCCESS && flash_wait();
}

/* ── The copier, out of RAM ──────────────────────────────────────────────
 *
 * Runs with the SoftDevice off and interrupts disabled, touches nothing but
 * registers and the addresses it is given, and never returns: it ends in a
 * system reset. Placed in .data so the startup code carries it into RAM
 * with the rest of the initialised data; the optimisation attributes stop
 * the compiler turning its loops into calls to memcpy in flash.
 *
 * [dst_a] <- [src_a] for [len_a] bytes, then [dst_b] <- [src_b] for [len_b]
 * (either may be 0), then the bootloader settings page is erased so the
 * bootloader judges bank 0 by its vector table and not by a CRC recorded
 * for the image that is no longer there. */
__attribute__((section(".data.xfw_copier"), noinline, optimize("O1"),
               optimize("no-tree-loop-distribute-patterns")))
static void xfw_copier(uint32_t dst_a, uint32_t src_a, uint32_t len_a,
                       uint32_t dst_b, uint32_t src_b, uint32_t len_b)
{
    volatile uint32_t *nvmc_cfg   = (volatile uint32_t *)0x4001E504;
    volatile uint32_t *nvmc_ready = (volatile uint32_t *)0x4001E400;
    volatile uint32_t *nvmc_erase = (volatile uint32_t *)0x4001E508;
    volatile uint32_t *wdt_rr0    = (volatile uint32_t *)0x40010600;
    for (int pass = 0; pass < 3; pass++) {
        uint32_t dst = pass == 0 ? dst_a : pass == 1 ? dst_b : XFW_SETTINGS;
        uint32_t src = pass == 0 ? src_a : src_b;
        uint32_t len = pass == 0 ? len_a : pass == 1 ? len_b : 0;
        if (pass < 2 && len == 0) continue;
        uint32_t pages = pass == 2 ? 1 : (len + XFW_PAGE - 1) / XFW_PAGE;
        for (uint32_t pg = 0; pg < pages; pg++) {
            *wdt_rr0 = 0x6E524635;                  /* keep the watchdog fed */
            *nvmc_cfg = 2;  while (!*nvmc_ready) { }
            *nvmc_erase = dst + pg * XFW_PAGE;
            while (!*nvmc_ready) { }
            *nvmc_cfg = 0;  while (!*nvmc_ready) { }
            if (pass == 2) break;
            *nvmc_cfg = 1;  while (!*nvmc_ready) { }
            volatile uint32_t *d = (volatile uint32_t *)(dst + pg * XFW_PAGE);
            const uint32_t    *s = (const uint32_t *)(src + pg * XFW_PAGE);
            uint32_t words = (len - pg * XFW_PAGE + 3) / 4;
            if (words > XFW_PAGE / 4) words = XFW_PAGE / 4;
            for (uint32_t i = 0; i < words; i++) { d[i] = s[i]; while (!*nvmc_ready) { } }
            *nvmc_cfg = 0;  while (!*nvmc_ready) { }
        }
    }
    /* SCB->AIRCR: VECTKEY | SYSRESETREQ */
    *(volatile uint32_t *)0xE000ED0C = 0x05FA0004;
    for (;;) { }
}

/* The size of the image that is running: text up to __etext, plus the
 * initialised data that follows it in flash. */
extern "C" uint32_t __etext, __data_start__, __data_end__;
static uint32_t running_image_size(void)
{
    uint32_t text = (uint32_t)&__etext - XFW_APP;
    uint32_t data = (uint32_t)&__data_end__ - (uint32_t)&__data_start__;
    return text + data;
}

/* ── Probation, read and written before the SoftDevice ───────────────── */

static bool probation_read(uint32_t *boot, char *ver, uint32_t *backup_len)
{
    File f(InternalFS);
    if (!f.open(PROBATION_PATH, FILE_O_READ)) return false;
    char buf[64] = {0};
    f.read((uint8_t *)buf, sizeof buf - 1);
    f.close();
    unsigned b = 0, l = 0;
    if (sscanf(buf, "%u %23s %u", &b, ver, &l) != 3) return false;
    *boot = b; *backup_len = l;
    return true;
}

static void probation_write(uint32_t boot, const char *ver, uint32_t backup_len)
{
    InternalFS.remove(PROBATION_PATH);
    File f(InternalFS);
    if (!f.open(PROBATION_PATH, FILE_O_WRITE)) return;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%lu %s %lu\n", (unsigned long)boot, ver, (unsigned long)backup_len);
    f.write((const uint8_t *)buf, (size_t)n);
    f.close();
}

void xfw_init(const xfw_cfg_t *cfg, uint32_t boot_epoch)
{
    s_cfg = *cfg;
    s_boot = boot_epoch;
    uint32_t since = 0, backup_len = 0;
    char ver[24] = "";
    if (!probation_read(&since, ver, &backup_len)) return;

    bool proved = (NRF_POWER->GPREGRET2 & GPREGRET2_PROVED) != 0;
    NRF_POWER->GPREGRET2 = 0;
    if (proved) {
        InternalFS.remove(PROBATION_PATH);
        Serial.printf("update: %s proved itself last boot -- keeping it\n", XPRS_FW_VERSION);
        return;
    }
    if (boot_epoch - since >= XFW_BOOTS_TO_GIVE_UP && backup_len && backup_len <= XFW_SLOT) {
        /* Three boots and not one of them got to two minutes with a radio.
         * Put back what worked. The note survives for zdiag to report. */
        Serial.printf("update: %s failed to prove itself in %lu boots -- restoring the previous image\n",
                      XPRS_FW_VERSION, (unsigned long)(boot_epoch - since));
        Serial.flush();
        InternalFS.remove(PROBATION_PATH);
        File f(InternalFS);
        if (f.open("/xprs/rolledback", FILE_O_WRITE)) { f.write(XPRS_FW_VERSION); f.close(); }
        delay(50);
        __disable_irq();
        xfw_copier(XFW_APP, XFW_BACKUP, backup_len, 0, 0, 0);
    }
    s_probation = true;
    Serial.printf("update: %s on probation (boot %lu of %d)\n", XPRS_FW_VERSION,
                  (unsigned long)(boot_epoch - since + 1), XFW_BOOTS_TO_GIVE_UP);
    File f(InternalFS);
    if (f.open("/xprs/rolledback", FILE_O_READ)) {
        int n = f.read((uint8_t *)s_rolled_back, sizeof s_rolled_back - 1);
        s_rolled_back[n > 0 ? n : 0] = 0;
        f.close();
    }
}

/* ── Answers ─────────────────────────────────────────────────────────── */

static void answer(xb_t *b, const char *to, const char *id, int code, const char *m)
{
    char w[XPRS_MAX_WIRE + 1];
    int n = snprintf(w, sizeof w, "t:result f:%s d:%s r:%s code:%d", s_cfg.call, to, id, code);
    if (m && m[0]) n += snprintf(w + n, sizeof w - n - 70, " m:%s", m);
    if (n > XPRS_MAX_WIRE - 66) n = XPRS_MAX_WIRE - 66;
    w[n] = 0;
    n = s_cfg.sign(w, n, (int)sizeof w);
    Serial.printf("update: -> %s\n", w);
    xb_send(b, w, n);
}

/* ── The session ─────────────────────────────────────────────────────── */

static bool unhex(const char *h, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static bool publisher_key(uint8_t out[32])
{
    const char *k = xcfg_get("fwkey", "");
    return strlen(k) == 64 && unhex(k, out, 32);
}

static bool have_chunk(uint32_t i) { return (s_ses.have[i >> 3] >> (i & 7)) & 1; }
static void mark_chunk(uint32_t i)  { s_ses.have[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static void session_close(void) { memset(&s_ses, 0, sizeof s_ses); }

/* Erase the staging pages the image will need. A few seconds for a full
 * slot, spent before answering 202, so the first chunk finds room. */
static bool stage_prepare(uint32_t size)
{
    uint32_t pages = (size + XFW_PAGE - 1) / XFW_PAGE;
    for (uint32_t p = 0; p < pages; p++)
        if (!page_erase(XFW_STAGE + p * XFW_PAGE)) return false;
    return true;
}

static int cmd_update(xb_t *b, const xprs_t *p, const char *from, const char *id, char *m, int mcap)
{
    char ver[24] = "", sizes[16] = "", sha[80] = "";
    xprs_get_str(p, "ver", ver, sizeof ver);
    xprs_get_str(p, "size", sizes, sizeof sizes);
    xprs_get_str(p, "sha", sha, sizeof sha);
    uint32_t size = (uint32_t)strtoul(sizes, NULL, 10);
    uint8_t pub[32];
    if (!publisher_key(pub))   { snprintf(m, mcap, "no firmware key pinned"); return 403; }
    if (!ver[0] || !size || strlen(sha) != 64) { snprintf(m, mcap, "need ver: size: sha:"); return 400; }
    if (strcmp(ver, XPRS_FW_VERSION) == 0) { snprintf(m, mcap, "already running %s", ver); return 200; }
    if (size > XFW_SLOT)       { snprintf(m, mcap, "%lu bytes does not fit a %lu slot", (unsigned long)size, (unsigned long)XFW_SLOT); return 413; }
    if (s_ses.open && strcmp(s_ses.ver, ver) != 0) session_close();
    if (!s_ses.open) {
        memset(&s_ses, 0, sizeof s_ses);
        if (!stage_prepare(size)) { snprintf(m, mcap, "could not erase staging"); return 500; }
        s_ses.open = true;
        snprintf(s_ses.ver, sizeof s_ses.ver, "%s", ver);
        s_ses.size = size;
        unhex(sha, s_ses.sha, 32);
        s_ses.nchunks = (size + XFW_CHUNK - 1) / XFW_CHUNK;
    }
    s_ses.last_ms = millis();
    s_ses.bearer = b;
    snprintf(s_ses.from, sizeof s_ses.from, "%s", from);
    snprintf(s_ses.id, sizeof s_ses.id, "%s", id);
    snprintf(m, mcap, "send %lu zfw chunks of %d, then zfwsig", (unsigned long)s_ses.nchunks, XFW_CHUNK);
    return 202;
}

/* Everything received: hash it, check the approval, copy it in. */
static void try_install(xb_t *b)
{
    if (!s_ses.open || s_ses.got < s_ses.nchunks || !s_ses.sig85[0]) return;

    uint8_t sha[32];
    xprs_sha256((const uint8_t *)XFW_STAGE, s_ses.size, sha);
    if (memcmp(sha, s_ses.sha, 32) != 0) {
        answer(b, s_ses.from, s_ses.id, 500, "image does not hash to sha: -- resend with zfwq");
        memset(s_ses.have, 0, sizeof s_ses.have); s_ses.got = 0;
        return;
    }
    /* The approval: the same line xprs_ota checks, the same pinned key. */
    char shahex[65];
    for (int i = 0; i < 32; i++) snprintf(shahex + 2 * i, 3, "%02x", sha[i]);
    char line[160];
    int n = snprintf(line, sizeof line, "xprsfw1 %s %s %lu %s", s_cfg.board, s_ses.ver,
                     (unsigned long)s_ses.size, shahex);
    uint8_t digest[32], sig[XPRSSIG_LEN], pub[32];
    xprs_sha256((const uint8_t *)line, (size_t)n, digest);
    if (!publisher_key(pub) ||
        xprssig_b85_decode(s_ses.sig85, XPRSSIG_B85_LEN, sig, sizeof sig) != XPRSSIG_LEN ||
        !xprssig_verify(digest, sig, pub)) {
        Serial.printf("update: approval does not verify for: %s\n", line);
        answer(b, s_ses.from, s_ses.id, 403, "approval does not verify for this image");
        s_ses.sig85[0] = 0;
        return;
    }

    answer(b, s_ses.from, s_ses.id, 202, "installing -- back in a minute, on probation");
    s_cfg.flush(4000);
    Serial.printf("update: installing %s (%lu bytes), keeping %s aside\n",
                  s_ses.ver, (unsigned long)s_ses.size, XPRS_FW_VERSION);
    Serial.flush(); delay(50);

    uint32_t old_len = running_image_size();
    sd_softdevice_disable();
    probation_write(s_boot, s_ses.ver, old_len);
    __disable_irq();
    xfw_copier(XFW_BACKUP, XFW_APP, old_len, XFW_APP, XFW_STAGE, s_ses.size);
}

static bool cmd_zfw(xb_t *b, const xprs_t *p)
{
    (void)b;
    if (!s_ses.open) return true;
    char ns[12] = "", m[XPRS_MAX_WIRE + 1] = "";
    xprs_get_str(p, "n", ns, sizeof ns);
    xprs_get_str(p, "m", m, sizeof m);
    uint32_t i = (uint32_t)strtoul(ns, NULL, 10);
    size_t len = strlen(m);
    if (!ns[0] || i >= s_ses.nchunks || len == 0 || len % 5 || len > XFW_CHUNK / 4 * 5) return true;
    if (have_chunk(i)) return true;
    uint32_t buf[XFW_CHUNK / 4];
    memset(buf, 0xFF, sizeof buf);
    int got = xprssig_b85_decode(m, len, (uint8_t *)buf, sizeof buf);
    if (got <= 0) return true;
    if (!words_write(XFW_STAGE + i * XFW_CHUNK, buf, (uint32_t)(got + 3) / 4)) {
        Serial.printf("update: flash write failed at chunk %lu\n", (unsigned long)i);
        return true;
    }
    mark_chunk(i);
    s_ses.got++;
    s_ses.last_ms = millis();
    if ((s_ses.got % 50) == 0 || s_ses.got == s_ses.nchunks)
        Serial.printf("update: %lu/%lu chunks\n", (unsigned long)s_ses.got, (unsigned long)s_ses.nchunks);
    try_install(b);
    return true;
}

static bool cmd_zfwsig(xb_t *b, const xprs_t *p)
{
    if (!s_ses.open) return true;
    char m[80] = "";
    xprs_get_str(p, "m", m, sizeof m);
    if (strlen(m) != XPRSSIG_B85_LEN) return true;
    snprintf(s_ses.sig85, sizeof s_ses.sig85, "%s", m);
    s_ses.last_ms = millis();
    try_install(b);
    return true;
}

/* Which chunks are still missing, as "3,7,20-41", as many as fit; 200
 * when none. Answered at most once every 3 s: it is unsigned, and a
 * stranger asking in a loop should cost the channel little. */
static bool cmd_zfwq(xb_t *b, const xprs_t *p)
{
    static uint32_t last_ms;
    if (!s_ses.open || millis() - last_ms < 3000) return true;
    last_ms = millis();
    char from[16] = "", id[8];
    xprs_get_str(p, "f", from, sizeof from);
    xprs_id(p, id);
    if (s_ses.got >= s_ses.nchunks) {
        answer(b, from, id, s_ses.sig85[0] ? 202 : 200,
               s_ses.sig85[0] ? "complete, installing" : "complete, send zfwsig");
        return true;
    }
    char m[160]; int n = 0;
    for (uint32_t i = 0; i < s_ses.nchunks && n < (int)sizeof m - 16; ) {
        if (have_chunk(i)) { i++; continue; }
        uint32_t j = i;
        while (j + 1 < s_ses.nchunks && !have_chunk(j + 1)) j++;
        n += (j > i) ? snprintf(m + n, sizeof m - n, "%s%lu-%lu", n ? "," : "", (unsigned long)i, (unsigned long)j)
                     : snprintf(m + n, sizeof m - n, "%s%lu", n ? "," : "", (unsigned long)i);
        i = j + 1;
    }
    answer(b, from, id, 206, m);
    return true;
}

static int cmd_zdiag(char *m, int mcap)
{
    uint32_t rx = 0, tx = 0, can = 0;
    (void)rx; (void)tx; (void)can;
    snprintf(m, mcap, "fw:%s uptime:%lus boots:%lu probation:%d%s%s stage:%lu/%lu",
             XPRS_FW_VERSION, (unsigned long)(millis() / 1000), (unsigned long)s_boot,
             (int)s_probation, s_rolled_back[0] ? " rolledback:" : "", s_rolled_back,
             (unsigned long)s_ses.got, (unsigned long)s_ses.nchunks);
    return 200;
}

bool xfw_handle(xb_t *b, const xprs_t *p)
{
    char cmd[16] = "";
    if (!xprs_get_str(p, "cmd", cmd, sizeof cmd)) return false;

    /* The unsigned lanes: only meaningful inside a session an owner opened. */
    if (strcmp(cmd, "zfw") == 0)    return cmd_zfw(b, p);
    if (strcmp(cmd, "zfwsig") == 0) return cmd_zfwsig(b, p);
    if (strcmp(cmd, "zfwq") == 0)   return cmd_zfwq(b, p);
    if (strcmp(cmd, "update") != 0 && strcmp(cmd, "zdiag") != 0) return false;

    /* The signed ones: one gate, the ESP32's (xprs_auth). */
    char id[8], from[16];
    int prev = 0;
    xauth_verdict_t v = xauth_check(p, s_cfg.call, id, from, &prev);
    switch (v) {
    case XAUTH_SILENT: return true;
    case XAUTH_403:    answer(b, from, id, 403, "not on this station's allow-list"); return true;
    case XAUTH_408:    answer(b, from, id, 408, xauth_platform_now() ? "stale" : "this station has no clock yet"); return true;
    case XAUTH_429:    answer(b, from, id, 429, "busy"); return true;
    case XAUTH_REPEAT: answer(b, from, id, prev, "repeat"); return true;
    case XAUTH_OK:     break;
    }
    char m[200] = "";
    int code = strcmp(cmd, "update") == 0 ? cmd_update(b, p, from, id, m, sizeof m)
                                           : cmd_zdiag(m, sizeof m);
    xauth_remember(id, code);
    answer(b, from, id, code, m);
    return true;
}

void xfw_tick(uint32_t now_ms, bool radio_up)
{
    if (s_probation && !s_proved && radio_up && now_ms >= XFW_PROVE_MS) {
        /* Two minutes up with a radio: this image is worth keeping. The
         * proof is read at the next boot; nothing is written now, because
         * flash files are not writable with the SoftDevice up. */
        uint32_t v = 0;
        sd_power_gpregret_get(1, &v);
        sd_power_gpregret_set(1, GPREGRET2_PROVED);
        s_proved = true;
        Serial.printf("update: %s has proved itself -- kept at the next boot\n", XPRS_FW_VERSION);
    }
    if (s_ses.open && now_ms - s_ses.last_ms > XFW_IDLE_MS) {
        Serial.println("update: session idle for 30 min -- dropped");
        session_close();
    }
}

const char *xfw_version(void) { return XPRS_FW_VERSION; }
bool xfw_probation(void) { return s_probation && !s_proved; }
void xfw_progress(uint32_t *got, uint32_t *of)
{
    *got = s_ses.open ? s_ses.got : 0;
    *of  = s_ses.open ? s_ses.nchunks : 0;
}
