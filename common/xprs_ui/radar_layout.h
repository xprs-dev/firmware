/*
 * radar_layout -- where the radar's contacts and their names go.
 *
 * Pure geometry: no LVGL, no allocation, so it is tested on the host
 * (test_radar_host.sh). xprs_ui.c measures the labels and draws; this
 * decides.
 *
 * Two rules, both from the T-Deck's home screen on 2026-10-24, where seven
 * stations within five metres drew their names as one unreadable stack at
 * the bottom of the scope, over the "100m" ring label and across the rim:
 *
 *  1. A name that has no clear place is HIDDEN, never pushed somewhere it
 *     does not belong. The dot stays and the home panel already counts the
 *     devices; a name drawn over another name tells nobody anything.
 *  2. Clear means clear of everything: inside the circle (not just its
 *     bounding square), off every dot and halo, off the centre flash, off
 *     the ring labels, off every name already placed.
 */
#ifndef RADAR_LAYOUT_H
#define RADAR_LAYOUT_H

#include <stdint.h>

#define RL_MAX 12          /* = XUI_BLIP_MAX */
#define RL_HALO 8          /* half the side of a contact's halo box */

typedef struct { int x, y, w, h; } rl_rect_t;

typedef struct {
    int cx, cy;            /* the scope's centre, in its own coordinates */
    int r;                 /* labels must stay inside this circle        */
    const rl_rect_t *keep_out;   /* ring labels, the centre flash        */
    int n_keep_out;
} rl_scope_t;

/* Where each contact's dot goes. [ang] in radians (0 = up, clockwise),
 * [rad] in pixels from the centre. A dot within [min_sep] px of one placed
 * before it, or on a keep-out (a ring label), is turned around the centre
 * in alternating steps, its radius -- which is its range -- untouched. Writes [bx], [by]. */
void rl_spread_dots(const rl_scope_t *s, const float *ang, const int *rad,
                    int n, int min_sep, int *bx, int *by);

/* The label forms a contact can take, longest first. */
enum { RL_HIDDEN = 0, RL_FULL = 1, RL_COMPACT = 2, RL_SHORT = 3 };
#define RL_FORMS 3

/* One form's size; w = 0 means this contact has no such form. */
typedef struct { int w, h; } rl_size_t;

/* Place the names. [order] is the order to try them in (nearest contact
 * first: it is the one a person is looking for). [forms][i] holds contact
 * i's sizes in order: FULL (name and range, two lines), COMPACT (the name),
 * SHORT (what a crowded scope can still afford: for a bridged Meshtastic
 * node the last four hex digits, which is the short name Meshtastic itself
 * gives a node). Each contact takes the longest form that has a clear
 * place. Writes the chosen form into [form] and its top-left into [out].
 * Returns how many names were placed. */
int rl_place_labels(const rl_scope_t *s, const int *bx, const int *by, int n,
                    const int *order, const rl_size_t (*forms)[RL_FORMS],
                    uint8_t *form, rl_rect_t *out);

/* The SHORT form of a name: for "MT" and eight hex digits (a bridged
 * Meshtastic node) the last four digits; anything else unchanged. */
void rl_short_name(const char *name, char *out, int cap);

#endif
