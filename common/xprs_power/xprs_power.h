/**
 * @file xprs_power.h
 * @brief How much battery is left, and what to switch off to keep it there.
 *
 * WHAT THIS REPLACES. Every board in this tree already offers `battery_mv`
 * (xapp_board_t) and the app already turned that into three facts: a
 * millivolt reading, a charging/discharging trend, and a screen that blanks.
 * That is all it turned it into. A user asking "how long have I got" was
 * shown "3980 mV", which is not an answer, and a T-Deck that ran flat in two
 * hours gave no warning before it did.
 *
 * WHY A VOLTAGE IS NOT A PERCENTAGE. A lithium cell spends most of its charge
 * between 3.9 V and 3.7 V, so the interesting 60% of the battery lives inside
 * 200 mV -- and on this board that 200 mV is measured through a 100k/100k
 * divider on an ADC that was watched wandering 4456..4568 mV on a bench with
 * nothing changing. Worse, the reading is TERMINAL voltage under a ~250 mA
 * load with LoRa and WiFi transmitting in bursts, so it sags and rebounds
 * constantly. A percentage read straight off that curve walks backwards in
 * front of the user, which reads as a broken gauge and is one.
 *
 * SO IT LEARNS THE CLOCK INSTEAD. Time is monotonic and this board has a good
 * one. Given how long this cell actually lasted last time, elapsed discharge
 * time IS the state of charge, and it can only go one way. The gauge blends:
 *
 *   voltage-%  interpolated over an OCV curve. Always available, including on
 *              a board that has never been discharged. Noisy.
 *   time-%     what is left of the learned full->empty runtime. Smooth and
 *              monotonic. Worthless until at least one discharge is observed.
 *
 * The blend weight IS the confidence: nothing learned means pure voltage, four
 * discharge cycles means mostly clock. The gauge never pretends -- ask it for
 * a time remaining before it has earned one and it answers -1.
 *
 * AND IT LEARNS THE CURVE FROM THE CLOCK, not from itself. Once the runtime is
 * known, the clock says "we are at 60%" and the ADC says "60% looks like
 * 3870 mV on this board, under this load". Learning the curve from the
 * voltage-percentage the curve itself produced would be a fixed-point
 * iteration chasing its own tail; learning it from the clock is a
 * measurement. That also means the curve converges to the LOADED curve for
 * this board's real duty, which is the curve we actually want, not the
 * datasheet's resting one.
 *
 * A NOTE ON THE CONSTANTS. `XPWR_USB_MV` and `XPWR_TREND_MV` below were paid
 * for with a bench session and moved here unaltered from xprs_app.c. A 15 mV
 * trend band read the ADC's own wander as "discharging" and put the screen
 * out on a board sitting on USB. Do not tighten them without a meter.
 *
 * GENERIC BY CONSTRUCTION. Nothing here knows about a T-Deck. It is fed
 * millivolts and a clock; boards differ only in the divider arithmetic they
 * already do inside their own `battery_mv`. A board that cannot measure its
 * battery never calls this and every reader gets -1, which is the same idiom
 * as raw_key and touch_read: offering the number is what earns the behaviour.
 */
#ifndef XPRS_POWER_H
#define XPRS_POWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Above any lithium cell, so the charger is holding the rail up. */
#define XPWR_USB_MV    4300
/* Wider than the ADC's own wander. See the note above before touching it. */
#define XPWR_TREND_MV    40
/* A cell at or below this is done; what remains is not usable capacity. */
#define XPWR_EMPTY_MV  3300
/* Six samples ten seconds apart: one minute of trend. */
#define XPWR_RING         6
#define XPWR_SAMPLE_MS 10000

typedef enum {
    XPWR_UNKNOWN = 0,
    XPWR_CHARGING,
    XPWR_DISCHARGING,
} xpwr_state_t;

/** Load the learned state from NVS and seed the curve. Call once, early. */
void xpwr_init(void);

/** True when a sample is due. The caller reads its ADC only when this says
 *  so -- on some boards a battery read is eight conversions and a mutex. */
bool xpwr_should_sample(uint32_t now_ms);

/** Hand in one battery reading in millivolts. A negative value means the
 *  board could not measure and is ignored. */
void xpwr_feed(int mv, uint32_t now_ms);

/** The last reading, or -1 if there has never been one. */
int xpwr_mv(void);

/** State of charge, 0..100, or -1 when nothing has been measured. */
int xpwr_pct(void);

/** Seconds of discharge left, or -1 until a full runtime has been learned.
 *  Only meaningful while discharging. */
int xpwr_secs_left(void);

/** How much the gauge trusts itself, 0 (voltage guess) .. 100 (learned). */
int xpwr_confidence(void);

xpwr_state_t xpwr_state(void);
const char  *xpwr_state_name(void);

/** Complete-enough discharges folded into the learned runtime. */
uint16_t xpwr_cycles(void);

/** The learned full->empty runtime in seconds, 0 until one is learned. */
uint32_t xpwr_full_s(void);

/**
 * The low-battery line, once per crossing.
 *
 * Returns true and fills `buf` the first time the charge falls to or below a
 * warning threshold, and not again until it has been back above it. Returns
 * false otherwise, so a caller can put this straight in its tick loop.
 */
bool xpwr_warning(char *buf, size_t cap);

/** True below the critical threshold: the caller should stop being generous
 *  with the screen and the hotspot whatever its configuration says. */
bool xpwr_critical(void);

/** The `"battery":{...}` members for /api/status, without the braces. */
int xpwr_status_json(char *buf, size_t cap);

/** Throw the learned state away. For a cell swap, or a bench run. */
void xpwr_forget(void);

/* ── The policy: what gets switched off, and when ──────────────────────────
 *
 * Separated from the gauge on purpose. The gauge is arithmetic and is tested
 * on the host; the policy touches radios and can only be judged on a bench
 * with a battery. Keeping them in one file but two halves means the half that
 * CAN be proven is proven.
 *
 * The actions arrive as hooks rather than as direct calls, so this component
 * does not have to depend on the BLE bearer or on WiFi to decide when to use
 * them -- and a board that offers neither hook simply saves nothing, with no
 * #ifdef anywhere. Same idiom as xapp_board_t.
 */
typedef struct {
    /** Duty-cycle the BLE scan. true = the low-power window. NULL: no BLE. */
    void (*ble_scan_duty)(bool low);
    /** Take the SoftAP down (true) or put it back (false). Returns whether it
     *  acted. NULL: the board has no hotspot to stand down. */
    bool (*ap_stand_down)(bool down);
    /** How many stations are associated to our SoftAP right now, or -1 when
     *  the board cannot say. The AP is never stood down under a live user. */
    int  (*ap_clients)(void);
    /** Whether a station interface is up and carrying an address. The AP only
     *  stands down when this is true, so the board stays reachable. */
    bool (*sta_up)(void);
} xpwr_hooks_t;

/** Wire the actions in and set the CPU's frequency-scaling policy. */
void xpwr_policy_init(const xpwr_hooks_t *hooks);

/** Drive the policy. Cheap; call it beside the gauge. */
void xpwr_policy_tick(uint32_t now_ms);

/** True while the battery savings are engaged. */
bool xpwr_saving(void);

/** What the policy has switched off, for a status line. Never NULL. */
const char *xpwr_saving_note(void);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_POWER_H */
