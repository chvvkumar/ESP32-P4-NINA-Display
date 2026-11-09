#pragma once

/**
 * @file nina_sky_scene.h
 * @brief The sky scene renderer shared by NINA page layouts 8 ("Readout on
 *        the ground", nina_layout_ground.c) and 9 ("In the sky",
 *        nina_layout_skywords.c). Compiled into BOTH panel families.
 *
 * One full-panel LVGL object with one LV_EVENT_DRAW_MAIN_END callback that
 * issues lv_draw_rect / lv_draw_line / lv_draw_arc / lv_draw_triangle calls
 * (the ADS-B Sky Dome pattern, nina_adsb.c). All trigonometry runs in
 * nina_sky_scene_update() and in the animation tick; the draw callback reads
 * precomputed integer screen coordinates and nothing else. The maths lives in
 * main/sky_scene_math.h.
 *
 * What it draws, back to front: sky gradient, the target's path (travelled
 * part quiet, part to come bright amber), finished-picture beads, the rise and
 * set marks, the flip mark, the target marker, the ground under the horizon
 * outline (plus layout 8's ground band), the telescope on its mount in its true
 * pose, the cyan beam to where it aims, the exposure ring around the target,
 * and amber/cyan edge arrows for a target or aim point outside the frame. It
 * draws NO text: every word belongs to the layout.
 *
 * Words are the layout's job; this module answers where things are
 * (nina_sky_scene_anchor()) and what the rig is doing (nina_sky_scene_info()).
 *
 * LOCKS. Every entry point runs with the LVGL display lock held by the caller.
 * nina_sky_scene_update() additionally runs inside the caller's nina_client
 * lock (data_update_task takes the client lock first, LVGL second); it copies
 * every field it needs out of @p d. The scene's own lv_timer runs in the LVGL
 * task and NEVER touches nina_client_t, so it can never invert that order.
 *
 * MEMORY. The scene state (path, bead log, projected coordinates, about 5 KB)
 * is one heap_caps_malloc(MALLOC_CAP_SPIRAM) block, freed from the object's
 * LV_EVENT_DELETE together with the timer. Deleting the page root (any slot
 * rebuild) therefore frees everything; there is no destroy call.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "nina_client.h"
#include "sky_scene_math.h"

typedef struct nina_sky_scene_s nina_sky_scene_t;   /* opaque, PSRAM */

/** What differs between the two layouts. */
typedef struct {
    /** Layout 8: panel y (px) where the readout band starts. The scene fills
     *  everything below it with the ground colour, fits the camera above it
     *  and keeps the horizon at the look direction at or above it. 0 = no
     *  band (layout 9): the frame is the whole safe area. */
    int ground_band_top;
} nina_sky_scene_opts_t;

/**
 * Create the scene as a full-panel (screen_size() square) child of @p parent
 * at (0,0), not clickable and not scrollable, so swipes and taps reach the
 * page underneath. Create it FIRST so the layout's labels draw over it.
 * Frame: square panel -> rect 24..S-24 (y1 = ground_band_top when set);
 * round panel -> disc about the panel centre, r = screen_safe_radius() - 8,
 * cut at ground_band_top when set. The camera starts snapped (no fly) to the
 * home pose. Returns NULL when the PSRAM block cannot be allocated (the
 * layout then shows its words over the page background).
 */
nina_sky_scene_t *nina_sky_scene_create(lv_obj_t *parent, const nina_sky_scene_opts_t *opts,
                                        int instance);

/** The scene's LVGL object (for z-order or hit tests). */
lv_obj_t *nina_sky_scene_obj(const nina_sky_scene_t *s);

/**
 * Push one poll's data. Copies target coordinates, site, sidereal time,
 * mount pose inputs, flip minutes, horizon, plan counts, filter and exposure
 * state out of @p d; rebuilds the path; logs a bead when the done count went
 * up; re-decides the camera goal; re-projects; invalidates ONLY when a
 * projected coordinate or colour actually changed (the panel is full refresh:
 * every invalidate repaints the whole screen); starts the animation timer
 * when something has to move. @p gb is the colour-brightness setting.
 * NULL-safe on @p s.
 */
void nina_sky_scene_update(nina_sky_scene_t *s, const nina_client_t *d, int instance, int gb);

/**
 * Exposure ring around the target, from the layout's p->alt.elapsed_cb
 * (spine 200 ms tick). @p frac in [0,1] fills clockwise from twelve o'clock;
 * frac < 0 hides it. @p color is final (already Red Night and brightness
 * mapped by the layout). Drawn only while the scene judges the mount on
 * target. Invalidates only when the drawn end angle moves a whole degree or
 * the colour changes.
 */
void nina_sky_scene_set_exposure(nina_sky_scene_t *s, float frac, uint32_t color);

/** Re-derive the palette (fixed scene colours through the Red Night luminance
 *  map and app_config_apply_brightness(@p gb)) and invalidate. */
void nina_sky_scene_apply_theme(nina_sky_scene_t *s, int gb);

/** Called after every re-projection that moved an anchor (data update or an
 *  animation tick), with the LVGL lock held, so layout 9 can move its labels.
 *  One callback per scene; NULL clears it. */
typedef void (*nina_sky_scene_moved_cb_t)(void *user);
void nina_sky_scene_set_moved_cb(nina_sky_scene_t *s, nina_sky_scene_moved_cb_t cb, void *user);

/** Things a label can be placed against. */
typedef enum {
    NINA_SKY_ANCHOR_TARGET = 0,   /* target marker; the rise point while the target is down */
    NINA_SKY_ANCHOR_FLIP,         /* the flip mark on the path */
    NINA_SKY_ANCHOR_RIG,          /* centre of the telescope's screen bounding box */
    NINA_SKY_ANCHOR_N,
} nina_sky_anchor_id_t;

typedef struct {
    bool    shown;     /* false: nothing to anchor to (no target, no flip, ...) */
    bool    at_edge;   /* true: off frame; x, y is the edge arrow, place the label inward of it */
    int16_t x, y;      /* panel px, page-root coordinates */
} nina_sky_anchor_t;

/** Fill @p out for @p id; returns out->shown. */
bool nina_sky_scene_anchor(const nina_sky_scene_t *s, nina_sky_anchor_id_t id,
                           nina_sky_anchor_t *out);

/** Telescope screen bounding box (panel px), for label avoidance. */
void nina_sky_scene_rig_box(const nina_sky_scene_t *s, lv_area_t *out);

/** True when all four corners of @p box lie inside the scene frame shrunk by
 *  8 px (sky_frame_contains()), i.e. a label there is fully on the glass. */
bool nina_sky_scene_box_fits(const nina_sky_scene_t *s, const lv_area_t *box);

/** True when @p box overlaps a drawn mark a label must not cover: the bright
 *  part of the path (sample points, 10 px slack), the target marker (r 24),
 *  the flip mark, or the beam (sampled every 16 px). */
bool nina_sky_scene_box_hits_marks(const nina_sky_scene_t *s, const lv_area_t *box);

/** Derived state for the words, refreshed by nina_sky_scene_update(). */
typedef struct {
    sky_activity_t activity;      /* sky_activity() over this poll's data */
    bool  target_known;           /* coordinates, site and sidereal time all known */
    bool  target_up;              /* above the horizon outline now */
    bool  rise_known;             /* target down and a rise crossing exists */
    float hours_to_rise;          /* sky_hours_until_ha(now, rise) when rise_known */
    bool  flip_known;             /* nina_client_t.flip_valid and under 12 h */
    float minutes_to_flip;
    bool  on_target;              /* mount within SKY_ON_TARGET_DEG of the target, tracking */
    int   done, total;            /* sky_plan_counts(); total <= 0 = unknown */
} nina_sky_info_t;

/** Never NULL for a live scene; NULL when @p s is NULL. */
const nina_sky_info_t *nina_sky_scene_info(const nina_sky_scene_t *s);

/**
 * Format the local clock time @p hours from now as "HH:MM" (24 h, local TZ
 * via localtime_r). "Now" is the NINA PC clock (nina_client_now_epoch() pair
 * captured by the last update) when known, else time(NULL) when it reads at
 * least 1700000000 (SNTP has run). Returns false and writes "" when neither
 * is trustworthy: the caller then words it without a clock time.
 */
bool nina_sky_scene_clock_after(const nina_sky_scene_t *s, float hours, char *buf, size_t n);
