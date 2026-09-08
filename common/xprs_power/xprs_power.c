/*
 * The battery gauge and the battery policy. See xprs_power.h for why the
 * gauge learns a clock rather than trusting a voltage.
 *
 * The file is in two halves. Everything above "the policy" is arithmetic over
 * a small struct and is compiled and tested on the host by
 * test_xpwr_host.sh -- no board, no cell, no waiting four hours to find out
 * that a percentage went backwards. Everything below it touches radios and
 * the CPU clock and can only be judged on a bench with a battery.
 */
#include "xprs_power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XPWR_HOST_TEST
/* On the host there is no flash, no log and no power manager. The store
 * becomes a byte array the test can inspect and corrupt. Everything below
 * this point is the firmware's own code, unaltered. */
static uint8_t s_fake[256];
static size_t  s_fake_n;
static bool store_load(void *p, size_t n)
{
    if (s_fake_n != n) return false;
    memcpy(p, s_fake, n);
    return true;
}
static void store_save(const void *p, size_t n)
{
    if (n > sizeof s_fake) return;
    memcpy(s_fake, p, n);
    s_fake_n = n;
}
static void store_erase(void) { s_fake_n = 0; }
static void cpu_scaling_on(void) { }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static const char *TAG __attribute__((unused)) = "xpwr";
#else
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "xpwr";

/* Its own namespace, not xcfg. xcfg's key table is a fixed static array whose
 * every entry is also rendered into the user-facing config.ini and reachable
 * from `cfg set` on the console -- and a learned calibration is not a
 * setting. A user who edits it has broken their gauge for no gain. The
 * precedent for a module owning its own namespace is xprs_ota, xprs_ssh and
 * wifi_bsp, all of which do exactly this. */
#define XPWR_NVS_NS  "xprspwr"
#define XPWR_NVS_KEY "learn"

static bool store_load(void *p, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(XPWR_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    esp_err_t err = nvs_get_blob(h, XPWR_NVS_KEY, p, &len);
    nvs_close(h);
    return err == ESP_OK && len == n;
}

static void store_save(const void *p, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(XPWR_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, XPWR_NVS_KEY, p, n) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void store_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(XPWR_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, XPWR_NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

/*
 * Dynamic frequency scaling, and NOT light sleep.
 *
 * Light sleep is the headline saving on an ESP32 and it is unavailable to
 * this station, for a reason worth writing down so nobody turns it on again
 * hoping. Automatic light sleep only enters when no task holds a power lock
 * and the radios agree to be asleep. This board holds WiFi at WIFI_PS_NONE on
 * purpose (xprsnow.c: a station that modem-sleeps misses ESP-NOW frames),
 * scans BLE, and keeps the SX1262 in continuous RX. There is no window to
 * enter. Enabling it would buy nothing and add real ISR-latency risk to a
 * panel and a radio that both sit on SPI.
 *
 * Frequency scaling has no such precondition: with nothing runnable the core
 * drops to 80 MHz on its own, and with the UI task backed off to 20 Hz while
 * the screen is dark, nothing is runnable most of the time.
 */
static void cpu_scaling_on(void)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_ERR_NOT_SUPPORTED)
        ESP_LOGW(TAG, "CPU scaling off: CONFIG_PM_ENABLE is not set");
    else if (err != ESP_OK)
        ESP_LOGW(TAG, "CPU scaling: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "CPU scaling 160/80 MHz, light sleep off");
}
#endif

/* ── The learned state ─────────────────────────────────────────────────── */

#define XPWR_MAGIC   0x5850574CU      /* 'XPWL' */
#define XPWR_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t cycles;        /* discharge runs folded into full_s        */
    uint16_t ocv[11];       /* learned mV at 0,10,..,100%               */
    uint32_t full_s;        /* learned full->empty runtime, seconds     */
    uint32_t discharge_s;   /* seconds discharging in the CURRENT run   */
    uint16_t pct_at_start;  /* charge when the current run began        */
    uint16_t reserved;
} xpwr_learn_t;

/*
 * The seed. A single-cell lithium curve, which is wrong for any particular
 * cell and right enough for all of them -- it exists so that a board that has
 * never been discharged still shows a plausible number on its first boot
 * instead of a dash. Every entry is replaced by measurement once the runtime
 * is known; see learn_curve().
 */
static const uint16_t k_seed_ocv[11] = {
    3300, 3600, 3690, 3730, 3770, 3820, 3870, 3930, 3990, 4070, 4150
};

static xpwr_learn_t s_l;
static bool     s_dirty;

static int      s_mv = -1;         /* last raw reading, for display        */
static int      s_smooth_mv = -1;  /* and the smoothed one, for the gauge  */
static int      s_ring[XPWR_RING];
static int      s_n;
static xpwr_state_t s_state = XPWR_UNKNOWN;
static uint32_t s_next_ms;
static uint32_t s_last_ms;
static bool     s_have_last;
static int      s_last_pct = -1;   /* the ratchet: never climbs on battery */
static int      s_warned;          /* 0 none, 1 low, 2 critical            */
static bool     s_run;             /* a discharge run is in progress       */
static bool     s_was_charger;     /* the charger was there last sample    */
static int      s_dis_mv = -1;     /* smoothed mV at the last DISCHARGING
                                    * sample -- see close_run()            */

#define XPWR_LOW_PCT      15
#define XPWR_CRITICAL_PCT  5

static void seed(void)
{
    memset(&s_l, 0, sizeof s_l);
    s_l.magic   = XPWR_MAGIC;
    s_l.version = XPWR_VERSION;
    memcpy(s_l.ocv, k_seed_ocv, sizeof s_l.ocv);
    s_l.pct_at_start = 100;
}

static void persist(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    store_save(&s_l, sizeof s_l);
}

void xpwr_init(void)
{
    seed();
    xpwr_learn_t got;
    if (store_load(&got, sizeof got) &&
        got.magic == XPWR_MAGIC && got.version == XPWR_VERSION) {
        s_l = got;
        ESP_LOGI(TAG, "battery: %u cycles learned, full run %lus",
                 (unsigned)s_l.cycles, (unsigned long)s_l.full_s);
    } else {
        ESP_LOGI(TAG, "battery: nothing learned yet, seeded curve");
    }
}

void xpwr_forget(void)
{
    seed();
    s_dirty = false;
    s_last_pct = -1;
    store_erase();
    ESP_LOGW(TAG, "battery: learned state thrown away");
}

/* ── Reading the curve ─────────────────────────────────────────────────── */

/* Non-decreasing, with a floor of 5 mV per band so no interpolation below
 * ever divides by zero. Learning one band cannot invert its neighbours. */
static void monotonic(void)
{
    for (int i = 1; i <= 10; i++)
        if (s_l.ocv[i] <= s_l.ocv[i - 1])
            s_l.ocv[i] = (uint16_t)(s_l.ocv[i - 1] + 5);
}

static int volt_pct(int mv)
{
    if (mv < 0) return -1;
    const uint16_t *c = s_l.ocv;
    if (mv <= c[0])  return 0;
    if (mv >= c[10]) return 100;
    for (int i = 0; i < 10; i++) {
        if (mv < c[i + 1]) {
            int lo = c[i], hi = c[i + 1];
            return i * 10 + (mv - lo) * 10 / (hi - lo);
        }
    }
    return 100;
}

/* What the clock says is left. -1 until a runtime has been learned. */
static int time_pct(void)
{
    if (!s_l.full_s || s_state != XPWR_DISCHARGING) return -1;
    int used = (int)((uint64_t)s_l.discharge_s * 100u / s_l.full_s);
    int p = (int)s_l.pct_at_start - used;
    return p < 0 ? 0 : (p > 100 ? 100 : p);
}

int xpwr_confidence(void)
{
    int c = s_l.cycles > 4 ? 4 : (int)s_l.cycles;
    return c * 25;
}

int xpwr_pct(void)
{
    if (s_smooth_mv < 0) return -1;
    int vp = volt_pct(s_smooth_mv);
    int tp = time_pct();
    int p  = vp;
    if (tp >= 0) {
        int w = xpwr_confidence();
        p = (vp * (100 - w) + tp * w) / 100;
    }
    /* The ratchet. A phone's percentage does not climb while it is unplugged,
     * and neither does this one: under a TX burst the terminal voltage sags
     * and rebounds, and a gauge that follows it looks broken even when the
     * arithmetic is right.
     *
     * Keyed on the RUN, not on the trend. The trend is allowed to wobble into
     * "charging" on a rebound -- that is what a rebound looks like to six
     * samples -- and if the ratchet let go there the number would climb on
     * exactly the readings it exists to hide. Only a charger releases it. */
    if (s_run && s_last_pct >= 0 && p > s_last_pct)
        p = s_last_pct;
    return p;
}

int xpwr_secs_left(void)
{
    if (s_l.cycles == 0 || !s_l.full_s) return -1;
    if (s_state != XPWR_DISCHARGING)    return -1;
    int p = xpwr_pct();
    if (p < 0) return -1;
    return (int)((uint64_t)s_l.full_s * (uint32_t)p / 100u);
}

int xpwr_mv(void)                { return s_mv; }
xpwr_state_t xpwr_state(void)    { return s_state; }
uint16_t xpwr_cycles(void)       { return s_l.cycles; }
uint32_t xpwr_full_s(void)       { return s_l.full_s; }

const char *xpwr_state_name(void)
{
    return s_state == XPWR_CHARGING    ? "charging"
         : s_state == XPWR_DISCHARGING ? "discharging" : "unknown";
}

bool xpwr_critical(void)
{
    int p = xpwr_pct();
    return p >= 0 && p <= XPWR_CRITICAL_PCT && s_state == XPWR_DISCHARGING;
}

/* ── Learning ──────────────────────────────────────────────────────────── */

/*
 * Fold a finished discharge run into the learned runtime.
 *
 * A run does NOT have to be a clean 100% -> 0%, and insisting on one is how a
 * self-learning gauge never learns anything: nobody runs a station flat on
 * purpose, and the ones that do get there are rarely charged from empty
 * afterwards. A run that covered at least 40% of the range is extrapolated to
 * a full one and folded in with an EMA, so four ordinary partial discharges
 * teach the gauge as much as one heroic full one.
 *
 * The span is measured with the VOLTAGE percentage on purpose. Measuring it
 * with the blended percentage would fold the clock's own answer back into the
 * clock, and the runtime would converge on whatever it first guessed.
 */
static void close_run(void)
{
    if (s_l.discharge_s < 60) return;
    /* s_dis_mv, NOT the current reading. close_run() is called on the sample
     * that FOUND the charger, and by then the rail is already being held up:
     * measuring the span there credits the run with the charger's own volts
     * and inflates every runtime it ever learns. Cost a full rewrite of this
     * function to notice, because the number it produced was plausible. */
    int now_pct = volt_pct(s_dis_mv);
    if (now_pct < 0) return;
    int span = (int)s_l.pct_at_start - now_pct;
    if (span < 40) {
        ESP_LOGI(TAG, "battery: run of %d%% too short to learn from", span);
        return;
    }
    uint32_t est = (uint32_t)((uint64_t)s_l.discharge_s * 100u / (uint32_t)span);
    /* Ten minutes to a week. Anything outside that is a clock fault or a
     * board that was on USB the whole time, not a battery. */
    if (est < 600u || est > 7u * 24u * 3600u) return;
    s_l.full_s = s_l.full_s
               ? (uint32_t)(((uint64_t)s_l.full_s * 3 + est) / 4)
               : est;
    if (s_l.cycles < 0xFFFF) s_l.cycles++;
    s_dirty = true;
    ESP_LOGI(TAG, "battery: learned from a %d%% run -- full is now %lus "
                  "(%u cycles)", span, (unsigned long)s_l.full_s,
             (unsigned)s_l.cycles);
}

/*
 * Learn what a percentage LOOKS like on this board, under this load.
 *
 * Only once a runtime exists, because the clock is what makes this a
 * measurement rather than a fixed point chasing itself: the clock says "60%",
 * the ADC says "3870 mV", and that pairing is the fact. Only samples sitting
 * within 3% of a band centre are used, so a reading is never attributed to
 * the band it is halfway out of.
 */
static void learn_curve(int mv)
{
    if (s_l.cycles == 0) return;
    int tp = time_pct();
    if (tp < 0) return;
    int band = (tp + 5) / 10;
    if (band < 0 || band > 10) return;
    if (abs(tp - band * 10) > 3) return;
    if (mv < 2500 || mv > 4400) return;
    uint16_t now = (uint16_t)((s_l.ocv[band] * 3 + mv) / 4);
    if (now == s_l.ocv[band]) return;
    s_l.ocv[band] = now;
    monotonic();
    s_dirty = true;
}

/* ── The state machine ─────────────────────────────────────────────────── */

bool xpwr_should_sample(uint32_t now_ms)
{
    return (int32_t)(now_ms - s_next_ms) >= 0;
}

void xpwr_feed(int mv, uint32_t now_ms)
{
    if (mv < 0) return;
    s_next_ms = now_ms + XPWR_SAMPLE_MS;
    s_mv = mv;
    s_smooth_mv = s_smooth_mv < 0 ? mv : (s_smooth_mv * 3 + mv) / 4;

    for (int i = XPWR_RING - 1; i > 0; i--) s_ring[i] = s_ring[i - 1];
    s_ring[0] = mv;
    if (s_n < XPWR_RING) s_n++;

    xpwr_state_t was = s_state;

    /* First the hard fact: a lithium cell never exceeds ~4.2 V, so a node
     * reading above 4.3 V is being held up by USB. The trend cannot be
     * trusted there -- measured on a bench T-Deck, the ADC wandered between
     * 4456 and 4568 mV on USB, which a 15 mV threshold read as "discharging"
     * and put the screen out. Below that, the trend decides, with a band wide
     * enough to sit outside that noise, and only once a full minute of
     * samples exists. */
    bool charger = (mv >= XPWR_USB_MV);
    if (charger) {
        s_state = XPWR_CHARGING;
    } else if (s_n >= XPWR_RING) {
        int delta = s_ring[0] - s_ring[XPWR_RING - 1];
        if (delta <= -XPWR_TREND_MV)     s_state = XPWR_DISCHARGING;
        else if (delta >= XPWR_TREND_MV) s_state = XPWR_CHARGING;
        /* flat and below the USB line: whatever it was, unchanged */
    }

    /*
     * A RUN ENDS WHEN A CHARGER APPEARS, not when the trend says "charging".
     *
     * These are not the same event and treating them as one broke both halves
     * of the gauge. Below the USB line the cell is still the only thing
     * powering the board, so a rising trend there is a rebound -- the sag
     * from a LoRa burst recovering -- and not a charger. Ending the run on it
     * threw away the accumulated time; releasing the percentage ratchet on it
     * let the number climb back up in front of the user, which is the exact
     * thing the ratchet exists to prevent.
     */
    if (charger && !s_was_charger && s_run) {
        close_run();
        s_run = false;
        s_l.discharge_s = 0;
        s_last_pct = -1;
        s_warned = 0;
        s_smooth_mv = mv;
        s_dirty = true;
    }

    if (!charger && !s_run && s_state == XPWR_DISCHARGING) {
        /* A run begins. Coming straight off a charger the cell is full by
         * definition, and this first sample below the USB line is what "full"
         * looks like on this board -- the one reading of the whole cycle that
         * can be labelled without knowing anything else. */
        if (s_was_charger) {
            s_smooth_mv = mv;    /* the charger's volts are not this cell's */
            if (mv > 3900 && mv < 4250) {
                s_l.ocv[10] = (uint16_t)((s_l.ocv[10] * 3 + mv) / 4);
                monotonic();
                s_l.pct_at_start = 100;
            } else {
                s_l.pct_at_start = 100;
            }
        } else {
            int vp = volt_pct(s_smooth_mv);
            s_l.pct_at_start = (uint16_t)(vp < 0 ? 100 : vp);
        }
        s_l.discharge_s = 0;
        s_run = true;
        s_last_pct = -1;
        s_dis_mv = s_smooth_mv;
        s_dirty = true;
    }

    /* Accumulate against the real clock, not against an assumed tick: a
     * station that was busy, or that rebooted, must not be credited with time
     * it did not spend on the battery. */
    if (s_run && s_state == XPWR_DISCHARGING && was == XPWR_DISCHARGING &&
        s_have_last) {
        uint32_t dt = now_ms - s_last_ms;
        if (dt < 5u * 60u * 1000u) s_l.discharge_s += dt / 1000u;
    }
    s_last_ms = now_ms;
    s_have_last = true;
    s_was_charger = charger;

    if (s_state != was)
        ESP_LOGI(TAG, "battery %d mV %d%% %s", mv, xpwr_pct(),
                 xpwr_state_name());

    if (s_state == XPWR_DISCHARGING) learn_curve(s_smooth_mv);

    int p = xpwr_pct();
    if (p >= 0) {
        /* Persist on a band crossing, not on a tick. A few writes per
         * discharge is nothing; one every ten seconds would be a flash
         * lifetime measured in months. */
        if (s_last_pct < 0 || p / 10 != s_last_pct / 10) s_dirty = true;
        s_last_pct = p;
    }
    if (s_state == XPWR_DISCHARGING) s_dis_mv = s_smooth_mv;
    persist();
}

bool xpwr_warning(char *buf, size_t cap)
{
    int p = xpwr_pct();
    if (p < 0 || s_state != XPWR_DISCHARGING) return false;

    int level = p <= XPWR_CRITICAL_PCT ? 2 : (p <= XPWR_LOW_PCT ? 1 : 0);
    if (level == 0) { s_warned = 0; return false; }
    if (level <= s_warned) return false;
    s_warned = level;

    int left = xpwr_secs_left();
    if (left > 0)
        snprintf(buf, cap, "Battery %d%% -- about %dh%02dm left", p,
                 left / 3600, (left % 3600) / 60);
    else
        snprintf(buf, cap, "Battery %d%% -- charge this station", p);
    ESP_LOGW(TAG, "%s", buf);
    return true;
}

int xpwr_status_json(char *buf, size_t cap)
{
    return snprintf(buf, cap,
        "\"mv\":%d,\"pct\":%d,\"state\":\"%s\",\"secs_left\":%d,"
        "\"confidence\":%d,\"cycles\":%u,\"full_s\":%lu,\"saving\":%s",
        s_mv, xpwr_pct(), xpwr_state_name(), xpwr_secs_left(),
        xpwr_confidence(), (unsigned)s_l.cycles, (unsigned long)s_l.full_s,
        xpwr_saving() ? "true" : "false");
}

/* ── The policy ────────────────────────────────────────────────────────── */

/* The AP stands down after this long with nobody on it ... */
#define AP_IDLE_MS   (5u * 60u * 1000u)
/* ... and comes back this often, for AP_IDLE_MS, so a walk-up user can still
 * find the station. Standing it down forever would silently delete the
 * hotspot, which on a board with no other way in is not a power saving, it is
 * a fault. */
#define AP_PEEK_MS  (20u * 60u * 1000u)

static xpwr_hooks_t s_hooks;
static bool     s_saving;
static bool     s_ap_down;
static uint32_t s_ap_idle_ms;
static uint32_t s_ap_back_ms;
static char     s_note[64];

bool xpwr_saving(void) { return s_saving; }

const char *xpwr_saving_note(void)
{
    if (!s_saving) { s_note[0] = 0; return s_note; }
    snprintf(s_note, sizeof s_note, "Saving: BLE scan%s",
             s_ap_down ? ", hotspot down" : "");
    return s_note;
}

void xpwr_policy_init(const xpwr_hooks_t *hooks)
{
    if (hooks) s_hooks = *hooks;
    cpu_scaling_on();
}

static void ap_policy(uint32_t now_ms)
{
    if (!s_hooks.ap_stand_down) return;

    if (!s_ap_down) {
        /* Two guards, and both matter. The AP only goes down while a station
         * interface is up, so the board stays reachable on the LAN rather
         * than vanishing; and never while somebody is associated to it,
         * because that somebody is using it right now. */
        bool reachable = s_hooks.sta_up && s_hooks.sta_up();
        int clients = s_hooks.ap_clients ? s_hooks.ap_clients() : -1;
        if (!reachable || clients != 0) { s_ap_idle_ms = now_ms; return; }
        uint32_t wait = xpwr_critical() ? 0 : AP_IDLE_MS;
        if (now_ms - s_ap_idle_ms < wait) return;
        if (s_hooks.ap_stand_down(true)) {
            s_ap_down = true;
            s_ap_back_ms = now_ms + AP_PEEK_MS;
        }
    } else {
        if (xpwr_critical()) return;      /* nearly flat: no more peeking */
        if ((int32_t)(now_ms - s_ap_back_ms) < 0) return;
        if (s_hooks.ap_stand_down(false)) {
            s_ap_down = false;
            s_ap_idle_ms = now_ms;
        }
    }
}

/*
 * Slow to start saving, instant to stop. The asymmetry is deliberate.
 *
 * The discharging verdict is a TREND below the USB line, and on a board whose
 * USB rail reads 4276-4366 mV -- straddling the 4300 mV clamp, measured on
 * X3DCK0 -- that verdict can flicker while the station is plugged in. Acting
 * on the first sample meant stopping and restarting the BLE scan every time
 * it did. So engaging waits for two solid minutes of "discharging", while
 * releasing happens on the first sample that is not.
 *
 * Getting this backwards would be the expensive mistake: a station left in
 * the low-power configuration while on mains would quietly answer slower for
 * as long as it stayed plugged in, and nothing on the screen would say so.
 */
#define SAVE_ARM_MS (2u * 60u * 1000u)

void xpwr_policy_tick(uint32_t now_ms)
{
    static uint32_t since_ms;
    static bool     was_dis;

    bool dis = (s_state == XPWR_DISCHARGING);
    if (!dis || !was_dis) since_ms = now_ms;
    was_dis = dis;

    bool want = dis && (now_ms - since_ms >= SAVE_ARM_MS);
    if (want != s_saving) {
        s_saving = want;
        if (s_hooks.ble_scan_duty) s_hooks.ble_scan_duty(want);
        if (!want && s_ap_down && s_hooks.ap_stand_down &&
            s_hooks.ap_stand_down(false))
            s_ap_down = false;
        s_ap_idle_ms = now_ms;
        ESP_LOGI(TAG, "power saving %s", want ? "on" : "off");
    }
    if (s_saving) ap_policy(now_ms);
}

#ifdef XPWR_HOST_TEST
/* The host test drives many independent cycles through one process. */
void xpwr_test_reset(void);
void xpwr_test_reset(void)
{
    store_erase();
    seed();
    s_dirty = false;
    s_mv = s_smooth_mv = -1;
    memset(s_ring, 0, sizeof s_ring);
    s_n = 0;
    s_state = XPWR_UNKNOWN;
    s_next_ms = 0;
    s_last_ms = 0;
    s_have_last = false;
    s_last_pct = -1;
    s_warned = 0;
    s_run = false;
    s_was_charger = false;
    s_dis_mv = -1;
    s_saving = false;
    s_ap_down = false;
    memset(&s_hooks, 0, sizeof s_hooks);
}
#endif
