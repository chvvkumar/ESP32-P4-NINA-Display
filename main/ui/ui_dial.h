#pragma once

/**
 * @file ui_dial.h
 * @brief Placement maths for the round pages' shape widgets.
 *
 * Three things every radial board needs: a fixed arc segment (ring block,
 * safety crown, countdown tick), the rotation a countdown takes on a dial, and
 * the position of a bullseye dot for a value against its threshold. Angles are
 * degrees CLOCKWISE from twelve o'clock, which is how the mockups are drawn;
 * the LVGL frame (0 at three o'clock) is reached by adding 270.
 *
 * Header-only and static inline: a translation unit that does not use a helper
 * emits nothing for it. Every entry point runs with the LVGL display lock held
 * by the caller. Single precision only.
 */

#include <math.h>

#include "lvgl.h"

/** Full-scale window of the meridian-flip tick, in minutes. */
#define UI_DIAL_FLIP_SPAN_MIN 240

/**
 * Park angle (degrees clockwise from twelve o'clock) for the longest
 * countdowns the tick still shows. Radial board 1's safety crown is a 40
 * degree gap centred on twelve o'clock (+/-20); a tick let all the way to 360
 * would sit inside that gap, reading as "flip in progress" instead of "flip
 * is a long way off" (review C12 I-2). 360 - 20 - 2 keeps two degrees clear.
 */
#define UI_DIAL_TICK_PARK_DEG  338

/**
 * @brief Fixed arc segment centred on the parent's centre.
 * @param r_mid  centre-line radius in px
 * @param width  stroke width in px
 * @param a0     start angle, degrees clockwise from twelve o'clock
 * @param a1     end angle, must exceed a0
 * @return the lv_arc; only LV_PART_MAIN is drawn, the caller sets its colour
 */
static inline lv_obj_t *ui_dial_arc(lv_obj_t *parent, int r_mid, int width,
                                    int a0, int a1)
{
    lv_obj_t *a = lv_arc_create(parent);
    int side = 2 * r_mid + width;
    lv_obj_set_size(a, side, side);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_rotation(a, (270 + a0 + 360) % 360);
    lv_arc_set_bg_angles(a, 0, (a1 > a0) ? (a1 - a0) : 1);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return a;
}

/**
 * @brief Put a countdown tick where @p minutes falls on a @p span_min dial.
 *
 * Zero minutes is twelve o'clock; the tick travels counter-clockwise as the
 * countdown grows. A countdown longer than the window (@p minutes exceeding
 * @p span_min), or negative, hides the tick outright rather than drawing it
 * at the window edge: that edge sits right next to the crown, and a tick a
 * full lap away is a stale/unknown reading, not a due-soon one. Within the
 * window the tick still stops UI_DIAL_TICK_PARK_DEG short of a full lap, so
 * the top of the range parks just outside the crown instead of wrapping into
 * it (review C12 I-2).
 */
static inline void ui_dial_set_tick(lv_obj_t *tick, int minutes, int span_min)
{
    if (!tick) return;
    if (minutes < 0 || minutes > span_min || span_min <= 0) {
        lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int deg_ccw = (minutes * 360) / span_min;
    if (deg_ccw > UI_DIAL_TICK_PARK_DEG) deg_ccw = UI_DIAL_TICK_PARK_DEG;
    lv_arc_set_rotation(tick, (270 + 360 - deg_ccw) % 360);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Round cap that rides an arc's leading edge at the exact float angle.
 *
 * LVGL 9.5 maps an lv_arc's value to WHOLE degrees, and the software arc draw
 * casts its angles to int32 whatever LV_USE_FLOAT says, so at a rim radius near
 * 400 px the filled band jumps a 6 to 7 px step at a time. The cap is placed
 * from the same fraction in floating point, so the leading edge the eye follows
 * advances about 1.5 px per 200 ms tick while the quantised band trails at most
 * one degree behind it, always underneath the cap.
 *
 * The cap is a CHILD of the arc, so it hides, dims and dies with it.
 * @param diam  cap diameter, normally the arc's stroke width
 */
static inline lv_obj_t *ui_dial_cap_create(lv_obj_t *arc, int diam, uint32_t color)
{
    if (!arc) return NULL;
    lv_obj_t *c = lv_obj_create(arc);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, diam, diam);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

/**
 * @brief Put @p cap at @p frac along the ring described by the other arguments.
 *
 * @p a0_deg and @p sweep_deg are the arc's own start angle and span in degrees
 * clockwise from twelve o'clock. A fraction at or below zero hides the cap
 * (nothing has been exposed yet); above one it clamps. lv_obj_align stores an
 * offset from the parent's centre, which for an lv_arc with no padding IS the
 * arc's centre. A style write invalidates even when the value is unchanged,
 * and this panel is full-refresh, so the position is compared first: the cap
 * repaints the frame only when it has actually moved a pixel.
 * ponytail: at the rim that is still about 5 Hz during an exposure; if that
 * shows up in /api/perf, place on a 2 px grid instead.
 */
static inline void ui_dial_cap_place(lv_obj_t *cap, int r_mid, int a0_deg,
                                     int sweep_deg, float frac)
{
    if (!cap) return;
    if (frac <= 0.0f) {
        lv_obj_add_flag(cap, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (frac > 1.0f) frac = 1.0f;
    const float t = ((float)a0_deg + frac * (float)sweep_deg) * 0.017453292f;
    const int32_t dx = (int32_t)lroundf((float)r_mid * sinf(t));
    const int32_t dy = (int32_t)lroundf(-(float)r_mid * cosf(t));
    if (lv_obj_get_style_x(cap, 0) != dx || lv_obj_get_style_y(cap, 0) != dy
        || lv_obj_get_style_align(cap, 0) != LV_ALIGN_CENTER) {
        lv_obj_align(cap, LV_ALIGN_CENTER, dx, dy);
    }
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Place a bullseye dot at @p ratio of @p ring's radius along a bearing.
 *
 * ratio 1.0 sits on the tolerance ring; out-of-tolerance values are clamped at
 * 1.35 so the dot stays inside the parent. @p dot and @p ring must be siblings
 * in a parent that runs no layout, so lv_obj_set_pos() sticks.
 */
static inline void ui_dial_place_dot(lv_obj_t *dot, lv_obj_t *ring,
                                     float ratio, int bearing_deg)
{
    if (!dot || !ring) return;
    if (ratio < 0.0f)  ratio = 0.0f;
    if (ratio > 1.35f) ratio = 1.35f;
    int r  = lv_obj_get_width(ring) / 2;
    int cx = lv_obj_get_x(ring) + r;
    int cy = lv_obj_get_y(ring) + r;
    float t  = ((float)bearing_deg - 90.0f) * 0.017453292f;
    float dr = (float)r * ratio;
    lv_obj_set_pos(dot,
                   cx + (int)(dr * cosf(t)) - lv_obj_get_width(dot) / 2,
                   cy + (int)(dr * sinf(t)) - lv_obj_get_height(dot) / 2);
}
