/**
 * @file xprs_script.h
 * @brief Phase-0 spike for a Wrench script host. An instrument, not a feature.
 *
 * The plan for scripting on this firmware rests on numbers that were measured
 * on a desk (flash size, no libstdc++) and on numbers that could only be
 * measured on the board. This exists to produce the second kind, and to be
 * deleted or grown into the real host depending on what they say.
 *
 * What it deliberately does NOT do yet: load from a partition, verify a
 * signature, dispatch triggers, or expose anything a script could do damage
 * with. One compiled-in bytecode blob, three natives, one task.
 *
 * The thresholds this is meant to answer, decided BEFORE measuring so the
 * answer cannot be talked into being acceptable afterwards:
 *
 *   flash delta                 <= 45 KB
 *   internal .bss/.data delta   <=  2 KB
 *   internal free heap          within 20 KB of the pre-script baseline
 *   task stack left at depth 32 >=  4 KB
 *   reachability during hot()   >= the idle baseline, n of m
 *
 * A hard stop on the last one. docs/esp32.md's whole "which processor the work
 * runs on" section is about a background task quietly taking the station off
 * the air, and a VM on core 1 has to prove it does not.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Claim the pool and start the script task on core 1.
 *
 * Call EARLY in app_main, where the heap is still one large block, for the
 * reason docs/esp32.md gives: "prefer claiming a large stack early over
 * hoping it fits later". Returns ESP_ERR_NOT_SUPPORTED when the board has no
 * PSRAM -- there is deliberately no fallback to the internal heap, because a
 * script host quietly eating the memory the radios need is the failure this
 * whole exercise was about.
 */
esp_err_t xs_start(void);

/**
 * Released by the application once xcfg_init() has run and the publisher key
 * has been seeded -- i.e. once "is this bundle trusted?" is answerable.
 *
 * xs_start() claims the task's stack at the top of app_main, where the heap
 * is still one large block, but the task must not LOAD before this: reading
 * `scriptkey` out of a config that has not been initialised yields nothing,
 * and the bundle would be refused for want of a key that is sitting in NVS.
 * That is exactly what happened on the bench the first time.
 *
 * Declared weak in xprs_app.c so a board without this component still links.
 */
void xs_app_ready(void);

/** True once the task is running and the module has loaded. */
bool xs_ready(void);

/**
 * Run the measurement sequence and print a table. Safe to call from any task;
 * the work happens on the script task.
 *
 * `hot_seconds` > 0 also runs the never-terminating script for that long, so
 * reachability can be measured against it from another machine. 0 skips it.
 */
void xs_spike(int hot_seconds);

#ifdef __cplusplus
}
#endif
