/**
 * @file xprs_ui_paper.h
 * @brief What the e-paper UI takes from its board beyond xprs_ui.h.
 *
 * xprs_ui_paper implements the shared interface (xprs_ui.h) on a
 * black-and-white e-paper panel. Everything the station says arrives through
 * that interface like on every other board; this header is the little a
 * board with sensors has to add, because xprs_app has no idea what a
 * thermometer is.
 *
 * WHAT THE BOARD'S FLUSH RECEIVES. An e-paper panel is not a framebuffer
 * that takes a slice every few milliseconds: a partial refresh costs a
 * third of a second and a full one two, and the panel wears with each. So
 * this UI does not forward LVGL's flushes. It renders, thresholds every
 * pixel to pure black or pure white, compares the result with what the
 * panel was last given, and only when the picture has really changed (and
 * no sooner than XUP_MIN_COMMIT_MS after the previous one) calls the
 * board's flush ONCE, with the whole screen: (0, 0, w-1, h-1), every pixel
 * 0x0000 or 0xFFFF. The board packs that to one bit per pixel and decides
 * between a partial and a full refresh. It should do the slow part on a
 * task of its own and return quickly, because it is called from the UI
 * task.
 *
 * A screenshot (the 'S' console key, or GET /api/screen) shows what the
 * panel was last GIVEN, not what LVGL has drawn since, so a picture of the
 * screen is a picture of the glass.
 */
#ifndef XPRS_UI_PAPER_H
#define XPRS_UI_PAPER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The fewest milliseconds between two commits to the panel. The first
 *  frame, and a screenshot request with a change pending, do not wait.
 *  A minute, because the clock changes once a minute anyway. At 15 s the
 *  bench board refreshed every 15 to 30 s, on values that were changing
 *  without meaning anything. */
#define XUP_MIN_COMMIT_MS 60000

/**
 * The room's climate, for the headline. Any task; the value is picked up on
 * the UI task's next pass. A board without the sensor never calls it and
 * the screen shows dashes. Pass ok=false when a reading failed, so a stale
 * number is not left on the glass as if it were current.
 */
void xup_set_climate(bool ok, float temp_c, float rh_pct);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_UI_PAPER_H */
