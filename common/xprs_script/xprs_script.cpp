/* Phase-0 spike for a Wrench script host. See xprs_script.h.
 *
 * C++ only because Wrench's API is (references and default arguments); the
 * surface this offers the rest of the firmware is plain C. Compiled with
 * -fno-exceptions -fno-rtti so it cannot drag libstdc++ in behind us. */

#include "xprs_script.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "esp_partition.h"

#include "wrench.h"
#include "xs_bundle.h"
#include "xprs_config.h"
#include "xprs_health.h"
#include "xprssig.h"

static const char *TAG = "xs";

/* Bound into the signed line, so a bundle built for one board cannot install
 * on another. CONFIG_GEOGRAM_BOARD_* would work too; this keeps the string in
 * one place next to the code that uses it. */
#ifndef XS_BOARD
#define XS_BOARD "tdeck"
#endif

/* ── The pool ───────────────────────────────────────────────────────────────
 *
 * Every byte a script allocates comes from PSRAM, through a counter with a
 * hard ceiling. This is the single most important property in the design: a
 * script that leaks takes itself down and nothing else. Internal RAM is what
 * the WiFi driver's keepalive frame, every task stack and every DMA buffer
 * come from, and docs/esp32.md is a long list of what happens when something
 * else gets there first. */
#define XS_POOL_BYTES   (256 * 1024)

static size_t s_used;
static size_t s_peak;
static unsigned s_oom;

static void *xs_alloc(size_t n)
{
    if (s_used + n + sizeof(size_t) > XS_POOL_BYTES) { s_oom++; return NULL; }
    void *p = heap_caps_malloc(n + sizeof(size_t), MALLOC_CAP_SPIRAM);
    if (!p) { s_oom++; return NULL; }
    *(size_t *)p = n + sizeof(size_t);
    s_used += n + sizeof(size_t);
    if (s_used > s_peak) s_peak = s_used;
    return (char *)p + sizeof(size_t);
}

static void xs_free(void *p)
{
    if (!p) return;
    char *base = (char *)p - sizeof(size_t);
    s_used -= *(size_t *)base;
    heap_caps_free(base);
}

/* ── The natives ────────────────────────────────────────────────────────────
 *
 * Three, and each one is a bounded copy or a read of something already in
 * RAM. Nothing here allocates without a bound, takes a pointer from the
 * script, or returns a handle the script can hold across calls -- which is
 * the shape every native in the real host has to keep. */

static void n_log(WRContext *c, const WRValue *argv, const int argn,
                  WRValue &ret, void *usr)
{
    (void)c; (void)usr;
    wr_makeInt(&ret, 0);
    if (argn < 1) return;
    /* Capped hard. ESP_LOG is the most stack-hungry thing a small task does
     * and a diagnostic that crashes the board is worse than no diagnostic
     * (docs/esp32.md) -- twice, on this project. */
    char buf[128];
    argv[0].asString(buf, sizeof buf);
    ESP_LOGI(TAG, "script: %.100s", buf);
}

static void n_now_ms(WRContext *c, const WRValue *argv, const int argn,
                     WRValue &ret, void *usr)
{
    (void)c; (void)argv; (void)argn; (void)usr;
    wr_makeInt(&ret, (int)(esp_timer_get_time() / 1000));
}

static void n_free_internal(WRContext *c, const WRValue *argv, const int argn,
                            WRValue &ret, void *usr)
{
    (void)c; (void)argv; (void)argn; (void)usr;
    wr_makeInt(&ret, (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static WRState   *s_w;
static WRContext *s_ctx;
static TaskHandle_t s_task;
static QueueHandle_t s_q;
static volatile bool s_ready;
static volatile bool s_app_ready;

/* ── Loading a bundle ───────────────────────────────────────────────────────
 *
 * The order here is the whole security property, so it is written once and
 * not rearranged: read, range-check, VERIFY, and only then hand bytes to the
 * VM. A forgery costs a read of a partition nothing depends on. */

#define XH_SCRIPTS  "scripts"

static xsb_t s_bundle;
static uint8_t *s_image;          /* the whole bundle, in the pool */
static char s_slot[10];

/* The publisher key: `scriptkey`, falling back to `fwkey`.
 *
 * Two keys rather than one so that "may publish panels for this station" can
 * be delegated without also delegating "may reflash the roof". With NEITHER
 * set, nothing verifies and nothing runs -- a station that has not been told
 * whom to trust runs nobody's code. That is the same rule xprs_ota applies to
 * firmware, and it is not a default worth softening. */
static bool publisher_key(uint8_t pub[32])
{
    const char *hex = xcfg_get("scriptkey", NULL);
    if (!hex || !hex[0]) hex = xcfg_get("fwkey", NULL);
    if (!hex || strlen(hex) != 64) {
        ESP_LOGW(TAG, "no scripts key configured -- no bundle can be trusted");
        return false;
    }
    for (int i = 0; i < 32; i++) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        pub[i] = (uint8_t)byte;
    }
    return true;
}

static bool load_slot(const char *label)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, label);
    if (!p) return false;

    /* The header first, alone, so a partition full of noise costs 120 bytes
     * rather than a 128 KB copy into the pool. */
    uint8_t hdr[XSB_BODY_OFF];
    if (esp_partition_read(p, 0, hdr, sizeof hdr) != ESP_OK) return false;
    if (memcmp(hdr, XSB_MAGIC, 4) != 0) {
        ESP_LOGD(TAG, "%s: empty or not a bundle", label);
        return false;
    }
    uint32_t signed_len;
    memcpy(&signed_len, hdr + 112, 4);
    if (signed_len == 0 || signed_len > p->size - XSB_BODY_OFF) {
        ESP_LOGW(TAG, "%s: header claims %u bytes, partition holds %u",
                 label, (unsigned)signed_len, (unsigned)p->size);
        return false;
    }

    size_t total = XSB_BODY_OFF + signed_len;
    uint8_t *img = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!img) {
        ESP_LOGE(TAG, "%s: no room in the pool for %u bytes", label, (unsigned)total);
        return false;
    }
    if (esp_partition_read(p, 0, img, total) != ESP_OK) {
        heap_caps_free(img);
        return false;
    }

    xsb_t b;
    if (!xsb_parse(img, total, &b)) {
        ESP_LOGW(TAG, "%s: bundle does not parse", label);
        heap_caps_free(img);
        return false;
    }

    uint8_t pub[32];
    if (!publisher_key(pub) || !xsb_verify(img, total, &b, XS_BOARD, pub)) {
        /* Loud. This is either a misconfigured station or somebody trying to
         * put code on it, and the two look identical from here. */
        ESP_LOGE(TAG, "%s: bundle '%s' %s DOES NOT VERIFY -- refusing to run it",
                 label, b.id, b.version);
        heap_caps_free(img);
        return false;
    }

    /* Verified. Only now does the VM see a byte of it. One context per
     * module, so a module that dies does not take its neighbours with it. */
    int loaded = 0;
    for (uint16_t i = 0; i < b.nmod; i++) {
        WRContext *c = wr_run(s_w, img + b.mod[i].off, (int)b.mod[i].len);
        if (!c) {
            ESP_LOGE(TAG, "%s: module '%s' refused to load, WRError %d",
                     label, b.mod[i].name, (int)wr_getLastError(s_w));
            continue;
        }
        if (!s_ctx) s_ctx = c;          /* the spike calls into the first */
        loaded++;
    }
    if (!loaded) { heap_caps_free(img); return false; }

    s_image = img;
    s_bundle = b;
    snprintf(s_slot, sizeof s_slot, "%s", label);
    s_ready = true;
    ESP_LOGW(TAG, "scripts: '%s' %s from %s, %d/%u module%s, %u bytes, verified",
             b.id, b.version, label, loaded, b.nmod,
             b.nmod == 1 ? "" : "s", (unsigned)total);
    return true;
}

/* ── The task ───────────────────────────────────────────────────────────── */

#define XS_STACK_BYTES  12288      /* the number this spike exists to check */

/* How many VM instructions before the VM must hand control back. The first
 * run used 2000 and measured a worst slice of 414 ms, which is far too long
 * for a board that also draws a screen -- so this is the other number the
 * spike is here to pin down. */
#define XS_SLICE_INSTRUCTIONS  500


typedef struct { int op; int arg; } xs_job_t;
enum { JOB_PROBE = 1, JOB_DEEP, JOB_HOT };

static int call_int(const char *fn, int arg, bool has_arg)
{
    /* No bundle loaded means no context, and wr_getFunction() dereferences
     * what it is given. Refusing a bundle is the ORDINARY case -- an erased
     * partition, no key configured, a signature that did not check -- so the
     * path where nothing is loaded has to be as solid as the path where
     * something is. It was not, and the board reboot-looped on
     * LoadProhibited until this guard existed. */
    if (!s_ready || !s_ctx) return -1;
    WRFunction *f = wr_getFunction(s_ctx, fn);
    if (!f) { ESP_LOGE(TAG, "no function '%s' in the module", fn); return -1; }
    WRValue a;
    wr_makeInt(&a, arg);
    WRValue *r = wr_callFunction(s_ctx, f, has_arg ? &a : 0, has_arg ? 1 : 0);
    if (!r) {
        /* NULL is "yielded" as well as "failed", and telling them apart is
         * the difference between a working time slice and a dead script. */
        if (wr_getYieldInfo(s_ctx)) return -2;
        ESP_LOGE(TAG, "'%s' failed, WRError %d", fn, (int)wr_getLastError(s_w));
        return -3;
    }
    return r->asInt();
}

static void stack_note(const char *what)
{
    ESP_LOGW(TAG, "  %-22s stack left %5u B, pool used %6u B (peak %6u, oom %u)",
             what, (unsigned)(uxTaskGetStackHighWaterMark(NULL)),
             (unsigned)s_used, (unsigned)s_peak, s_oom);
}

static void xs_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    wr_setGlobalAllocator(xs_alloc, xs_free);
    /* Explicit, not the WRENCH_DEFAULT_STACK_SIZE default. A build-time
     * define that silently fails to propagate is exactly the class of bug
     * docs/esp32.md keeps recording, and it happened here: the first run of
     * this spike raised the default to 256 in CMakeLists and the compile
     * still used 48. The value stack is per-context and comes out of the
     * PSRAM pool, so 1024 entries is 8 KB of memory nothing else wants. */
    s_w = wr_newState(1024);
    if (!s_w) { ESP_LOGE(TAG, "wr_newState failed"); vTaskDelete(NULL); return; }

    wr_registerFunction(s_w, "log",           n_log);
    wr_registerFunction(s_w, "now_ms",        n_now_ms);
    wr_registerFunction(s_w, "free_internal", n_free_internal);

    /* Bounded so a runaway script cannot hold the core. The number is what
     * the spike is measuring; 2000 is a starting guess, not a conclusion. */
    wr_setInstructionsPerSlice(s_w, XS_SLICE_INSTRUCTIONS);

    /* Wait for the station to finish making "is this trusted?" answerable.
     * Bounded, because a board whose app never signals must still end up in a
     * defined state (no scripts) rather than a task parked forever. */
    for (int i = 0; i < 100 && !s_app_ready; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_app_ready)
        ESP_LOGW(TAG, "app never signalled readiness -- loading anyway");

    /* Slot A, then B. A bundle that will not verify or will not load is not
     * an error to stop on: the station runs with no scripts, says so, and
     * keeps flying. Nothing here may take the radios down. */
    if (!load_slot("script_a") && !load_slot("script_b")) {
        ESP_LOGW(TAG, "no usable script bundle -- running with no scripts");
    }
    xh_set(XH_SCRIPTS, s_ready);
    stack_note("after load");

    for (;;) {
        xs_job_t j;
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }
        switch (j.op) {
        case JOB_PROBE:
            call_int("probe", 0, false);
            stack_note("after probe()");
            break;

        case JOB_DEEP: {
            int got = call_int("deep", j.arg, true);
            char what[32];
            snprintf(what, sizeof what, "after deep(%d)=%d", j.arg, got);
            stack_note(what);
            break;
        }

        case JOB_HOT: {
            /* The one that matters. hot() never returns, so every call comes
             * back as a yield; the loop counts them, feeds the watchdog and
             * gives the scheduler a tick. If the station stops answering
             * pings during this, the VM does not belong on this board. */
            /* Let the station finish coming up first: measuring a VM
             * against a board that is still starting its radios measures
             * the wrong thing. */
            while (esp_timer_get_time() < 25 * 1000000LL) {
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_task_wdt_reset();
            }
            ESP_LOGW(TAG, "  hot(): running a non-terminating script for %d s",
                     j.arg);
            int64_t end = esp_timer_get_time() + (int64_t)j.arg * 1000000;
            uint32_t slices = 0;
            int64_t worst = 0, total_us = 0;
            if (!s_ready || !s_ctx) { ESP_LOGW(TAG, "  hot(): no module loaded"); break; }
            WRFunction *f = wr_getFunction(s_ctx, "hot");
            WRValue *r = f ? wr_callFunction(s_ctx, f, 0, 0) : (WRValue *)1;
            while (!r && esp_timer_get_time() < end) {
                int64_t t0 = esp_timer_get_time();
                r = wr_continue(s_ctx);
                int64_t dt = esp_timer_get_time() - t0;
                if (dt > worst) worst = dt;
                total_us += dt;
                slices++;
                esp_task_wdt_reset();
                vTaskDelay(1);            /* never starve anything else */
            }
            ESP_LOGW(TAG, "  hot(): %u slices at %d instr, worst %lld us, "
                          "mean %lld us, %s",
                     (unsigned)slices, XS_SLICE_INSTRUCTIONS,
                     (long long)worst,
                     (long long)(slices ? total_us / slices : 0),
                     r ? "script ended" : "still yielding (correct)");
            stack_note("after hot()");
            break;
        }
        }
        esp_task_wdt_reset();
    }
}

extern "C" esp_err_t xs_start(void)
{
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram < XS_POOL_BYTES) {
        /* No fallback to internal, on purpose. See the header. */
        ESP_LOGE(TAG, "no PSRAM (%u free) -- script host will not start",
                 (unsigned)psram);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* required = FALSE, and that boolean is load-bearing.
     *
     * xh_all_ok() is the verdict the OTA rollback self-test consumes
     * (xprs_health.h). If a script's failure could make it false, then a bad
     * third-party bundle would roll back the FIRMWARE -- handing anyone who
     * can publish a script the power to un-install a release. A station with
     * no scripts is a station. */
    xh_expect(XH_SCRIPTS, false);

    s_q = xQueueCreate(8, sizeof(xs_job_t));
    if (!s_q) { ESP_LOGE(TAG, "queue failed"); return ESP_ERR_NO_MEM; }

    /* Core 1, checked, logged. docs/esp32.md lists four outages caused by an
     * xTaskCreate whose result nobody read. */
    if (xTaskCreatePinnedToCore(xs_task, "script", XS_STACK_BYTES, NULL,
                                2, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "script task failed to start -- no scripts will run");
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" void xs_app_ready(void) { s_app_ready = true; }

extern "C" bool xs_ready(void) { return s_ready; }

extern "C" void xs_spike(int hot_seconds)
{
    if (!s_q) return;
    const int depths[] = { 1, 8, 32, 64, 128, 200 };
    xs_job_t j;

    j.op = JOB_PROBE; j.arg = 0;
    xQueueSend(s_q, &j, portMAX_DELAY);

    for (unsigned i = 0; i < sizeof depths / sizeof depths[0]; i++) {
        j.op = JOB_DEEP; j.arg = depths[i];
        xQueueSend(s_q, &j, portMAX_DELAY);
    }

    if (hot_seconds > 0) {
        j.op = JOB_HOT; j.arg = hot_seconds;
        xQueueSend(s_q, &j, portMAX_DELAY);
    }
}
