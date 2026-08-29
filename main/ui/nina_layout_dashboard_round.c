/**
 * @file nina_layout_dashboard_round.c
 * @brief NINA layout 0 (Dashboard) on a round panel: radial board 1.
 *
 * The rim carries progress and time: an exposure ring at ui_rim_radius() - 12
 * whose 40 degree gap at twelve o'clock is filled by a safety crown, a sub
 * ring at ui_rim_radius() - 40 built by nina_subbar, and a meridian-flip tick
 * that climbs the left rim and reaches the crown at flip. The centre holds one
 * spine of identity, target, step, elapsed seconds and the filter plus counter
 * row; below it two bullseyes turn guiding RMS and HFR into a dot's distance
 * from centre, with the figure under each and the flip figure under those.
 *
 * The page owns the data, the timers and the formatting: this file creates and
 * places widgets and writes the handles, nothing else. Every widget it does not
 * build stays NULL and nina_dashboard_update.c null-checks it. Runs with the
 * LVGL display lock held by the caller.
 *
 * Geometry is centre-relative and in absolute pixels, so the 800 panel gets the
 * same composition with more black margin and wider chords.
 */

#include "nina_layout_alt.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_dial.h"
#include "ui_helpers.h"
#include "ui_round.h"

LV_FONT_DECLARE(lv_font_hanken_black_96);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

/* Ring radii. The board pinned these to the rim radius (radial.md: Rs-12 and
 * Rs-40), which left a 10 px black band outside the exposure ring. The ring is
 * now FLUSH with the glass the way octoprint_layout_glass_round.c's rim is: its
 * centreline sits half a stroke in, so its outer edge lands on the panel edge.
 * The offsets below are kept and reused as the SPACING between the three rim
 * features, so the whole stack slides outward together and the board's
 * 28 px ring separation and 3 px tick bias survive. */
#define DR_R_EXPOSURE_OFF   12
#define DR_R_SUB_OFF        40
#define DR_W_EXPOSURE       16
#define DR_W_SUB            14
#define DR_GAP_DEG          40    /* crown width, and the gap both rings leave */

/* Flip tick: 30 px long, centred on the exposure ring, 2 degrees wide. */
#define DR_R_TICK_OFF       15
#define DR_W_TICK           30

/* Centre spine and the block below it, as offsets from the panel centre. */
#define DR_SPINE_TOP_DY    (-268)
#define DR_BULL_DY          126
#define DR_BULL_DX          88
#define DR_BULL_R           44
#define DR_BULL_DOT         14
/* Review C12 M-5: the builder aligns these labels' CENTRE at the offset, but
 * the mockup's CSS `top` values (556, 626) name the box TOP; the 44 px line
 * height of Montserrat 40 needs the difference added back in. */
#define DR_VALUE_DY         218
#define DR_FLIP_DY          283

/* Spine gaps. The 96 px Hanken face reports line_height 69, not the 108 px box
 * the mockup draws, so the column runs about 95 px short of radial.html unless
 * the two gaps below the step absorb it (review C minor M-10, margin option).
 * With spine top at centre-268 = y 92 the boxes land at: identity 92..122,
 * target 132..176, step 182..212, elapsed 242..311, filter row 376..406, which
 * is within 8 px of the mockup's 100 / 144 / 192 / 250 / 384 and leaves the
 * 36 px the mockup leaves above the bullseyes at y 442. */
#define DR_GAP_TARGET       10
#define DR_GAP_STEP          6
#define DR_GAP_ELAPSED      30
#define DR_GAP_FILTER       65

/* Bullseye bearings: DR_BEARING_RMS / DR_BEARING_HFR now live in
 * nina_dashboard_internal.h (review C12 M-1), single owner shared with
 * nina_dashboard_update.c. */

#define DR_TRACK_COLOR      0x161616
#define DR_RING_BORDER      0x262a30

/* Fonts. The 108 px elapsed of the mockup maps to the existing 96 px Hanken
 * Black, whose glyph set is digits and colon only, so its unit "s" is a
 * separate 28 px Hanken Bold label in the same row. The 40 px bullseye figures
 * map to Montserrat 40, not to Hanken Bold 44: that face carries digits, minus
 * and degree only, and an RMS figure needs a period and an inch mark. */
#define DR_FONT_IDENT      (&lv_font_montserrat_28)
#define DR_FONT_TARGET     (&lv_font_montserrat_40)
#define DR_FONT_STEP       (&lv_font_montserrat_28)
#define DR_FONT_ELAPSED    (&lv_font_hanken_black_96)
#define DR_FONT_UNIT       (&lv_font_hanken_bold_28)
#define DR_FONT_FILTER     (&lv_font_montserrat_28)
#define DR_FONT_COUNT      (&lv_font_hanken_bold_28)
#define DR_FONT_VALUE      (&lv_font_montserrat_40)
#define DR_FONT_FLIP       (&lv_font_montserrat_28)

/* ---- helpers ------------------------------------------------------------ */

static lv_obj_t *dr_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, const char *text);
static lv_obj_t *dr_row(lv_obj_t *parent, int gap);
static lv_obj_t *dr_bullseye(lv_obj_t *parent, int dx, int dy, int r,
                             uint32_t dot_color, lv_obj_t **out_dot);
static void      dr_elapsed_cb(void *ud, int secs);

static lv_obj_t *dr_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* Transparent content-sized row, children bottom aligned. */
static lv_obj_t *dr_row(lv_obj_t *parent, int gap) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_gap(r, gap, 0);
    return r;
}

/* Tolerance ring plus a half-scale ring, and a dot created as the ring's
 * SIBLING so an out-of-tolerance value can sit outside the ring without being
 * clipped by it. ui_dial_place_dot() reads the ring's box for both. */
static lv_obj_t *dr_bullseye(lv_obj_t *parent, int dx, int dy, int r,
                             uint32_t dot_color, lv_obj_t **out_dot) {
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, 2 * r, 2 * r);
    lv_obj_align(ring, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(DR_RING_BORDER), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);

    lv_obj_t *half = lv_obj_create(ring);
    lv_obj_remove_style_all(half);
    lv_obj_remove_flag(half, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(half, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(half, r, r);
    lv_obj_center(half);
    lv_obj_set_style_radius(half, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(half, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(half, 1, 0);
    lv_obj_set_style_border_color(half, lv_color_hex(DR_RING_BORDER), 0);
    lv_obj_set_style_border_opa(half, LV_OPA_50, 0);

    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, DR_BULL_DOT, DR_BULL_DOT);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    *out_dot = dot;
    return ring;
}

/* Sole writer of the elapsed digits: the 200 ms tick through the sub ring, and
 * the idle reset that arrives as -1 (nina_subbar_reset_elapsed). */
static void dr_elapsed_cb(void *ud, int secs) {
    dashboard_page_t *p = (dashboard_page_t *)ud;
    if (!p || !p->alt.lbl_elapsed || !p->alt.row_vals) return;
    if (secs < 0) {
        /* The 96 px face has digits and a colon only, so there is no "--" to
         * fall back to: hide the whole row, unit included. */
        lv_obj_add_flag(p->alt.row_vals, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (secs > 9999) secs = 9999;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", secs);
    if (strcmp(lv_label_get_text(p->alt.lbl_elapsed), buf) != 0) {
        lv_label_set_text(p->alt.lbl_elapsed, buf);
    }
    lv_obj_remove_flag(p->alt.row_vals, LV_OBJ_FLAG_HIDDEN);
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_dashboard_round_create(dashboard_page_t *p, lv_obj_t *parent,
                                        int page_index) {
    if (!p || !parent || !current_theme) return;

    int gb = app_config_get()->color_brightness;
    p->alt.inst = page_index;

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* Exposure ring flush with the glass; the sub ring and the flip tick keep
     * their board spacing relative to it. */
    const int r_exp  = screen_center() - DR_W_EXPOSURE / 2;
    const int r_sub  = r_exp - (DR_R_SUB_OFF - DR_R_EXPOSURE_OFF);
    const int r_tick = r_exp - (DR_R_TICK_OFF - DR_R_EXPOSURE_OFF);

    /* 1: the exposure ring. This IS p->arc_exposure, so every line of the
     * shipped exposure model (seed, long linear anim, completion snap, gap
     * fade) drives it with no change. ring_exposure names the same object so
     * the stale cue knows there is a rim ring to dim. */
    p->arc_exposure = lv_arc_create(parent);
    lv_obj_set_size(p->arc_exposure, 2 * r_exp + DR_W_EXPOSURE,
                                     2 * r_exp + DR_W_EXPOSURE);
    lv_obj_center(p->arc_exposure);
    lv_arc_set_rotation(p->arc_exposure, (270 + DR_GAP_DEG / 2) % 360);
    lv_arc_set_bg_angles(p->arc_exposure, 0, 360 - DR_GAP_DEG);
    lv_arc_set_range(p->arc_exposure, 0, ARC_RANGE);
    lv_arc_set_value(p->arc_exposure, 0);
    lv_obj_remove_style(p->arc_exposure, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(p->arc_exposure, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(p->arc_exposure, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(p->arc_exposure, DR_W_EXPOSURE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->arc_exposure, DR_W_EXPOSURE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(p->arc_exposure, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(p->arc_exposure, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(DR_TRACK_COLOR),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(p->arc_exposure,
        lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)),
        LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(p->arc_exposure, 0, LV_PART_INDICATOR);
    p->ring_exposure = p->arc_exposure;

    /* 2: the safety crown filling the ring's gap. update_safety_icon paints it. */
    p->ring_crown = ui_dial_arc(parent, r_exp, DR_W_EXPOSURE,
                                -DR_GAP_DEG / 2, DR_GAP_DEG / 2);
    lv_obj_set_style_arc_color(p->ring_crown, lv_color_hex(0x2a2a2a), LV_PART_MAIN);

    /* 3: the meridian-flip tick. Hidden until a countdown arrives. */
    p->ring_flip_tick = ui_dial_arc(parent, r_tick, DR_W_TICK, 0, 2);
    lv_obj_set_style_arc_color(p->ring_flip_tick,
        lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
        LV_PART_MAIN);
    lv_obj_add_flag(p->ring_flip_tick, LV_OBJ_FLAG_HIDDEN);

    /* 4: the sub ring, same block rule as the square ledge. */
    nina_subbar_create_ring(&p->subbar, parent, r_sub, DR_W_SUB, DR_GAP_DEG);
    nina_subbar_set_elapsed_cb(&p->subbar, dr_elapsed_cb, p);

    /* 5: the centre spine. One flex column on the widest chords it needs. */
    lv_obj_t *spine = lv_obj_create(parent);
    lv_obj_remove_style_all(spine);
    lv_obj_remove_flag(spine, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spine, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(spine, 2 * ui_chord_half(DR_SPINE_TOP_DY));
    lv_obj_set_height(spine, LV_SIZE_CONTENT);
    lv_obj_align(spine, LV_ALIGN_TOP_MID, 0, screen_center() + DR_SPINE_TOP_DY);
    lv_obj_set_style_pad_all(spine, 0, 0);
    lv_obj_set_style_pad_gap(spine, 0, 0);
    lv_obj_set_flex_flow(spine, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(spine, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    p->lbl_instance_name = dr_label(spine, DR_FONT_IDENT,
        app_config_apply_brightness(current_theme->label_color, gb), "N.I.N.A.");
    lv_obj_set_width(p->lbl_instance_name, LV_PCT(100));
    lv_obj_set_style_text_letter_space(p->lbl_instance_name, 1, 0);

    p->alt.lbl_target = dr_label(spine, DR_FONT_TARGET,
        app_config_apply_brightness(current_theme->target_name_color, gb), "----");
    lv_obj_set_width(p->alt.lbl_target, LV_PCT(100));
    lv_obj_set_style_margin_top(p->alt.lbl_target, DR_GAP_TARGET, 0);
    nina_dashboard_bind_tap(p->alt.lbl_target, NINA_TAP_CAPTURE);

    p->lbl_seq_step = dr_label(spine, DR_FONT_STEP,
        app_config_apply_brightness(current_theme->header_text_color, gb), "----");
    lv_obj_set_width(p->lbl_seq_step, LV_PCT(100));
    lv_obj_set_style_margin_top(p->lbl_seq_step, DR_GAP_STEP, 0);
    nina_dashboard_bind_tap(p->lbl_seq_step, NINA_TAP_SEQUENCE);

    /* Elapsed seconds: digits in the 96 px face, unit in a 28 px face beside
     * them. row_vals is this layout's handle for the pair, so the idle reset
     * hides both at once. */
    p->alt.row_vals = dr_row(spine, 4);
    lv_obj_set_style_margin_top(p->alt.row_vals, DR_GAP_ELAPSED, 0);
    nina_dashboard_bind_tap(p->alt.row_vals, NINA_TAP_EXPOSURE);
    p->alt.lbl_elapsed = dr_label(p->alt.row_vals, DR_FONT_ELAPSED,
        app_config_apply_brightness(current_theme->text_color, gb), "0");
    p->alt.lbl_unit = dr_label(p->alt.row_vals, DR_FONT_UNIT,
        app_config_apply_brightness(current_theme->label_color, gb), "s");
    lv_obj_set_style_translate_y(p->alt.lbl_unit,
        DR_FONT_UNIT->base_line - DR_FONT_ELAPSED->base_line, 0);
    lv_obj_add_flag(p->alt.row_vals, LV_OBJ_FLAG_HIDDEN);

    /* Filter name and the sub counter share one row: the ring is the count, so
     * the digits ride along instead of taking a line of their own. */
    lv_obj_t *row_filter = dr_row(spine, 16);
    lv_obj_set_style_margin_top(row_filter, DR_GAP_FILTER, 0);
    p->lbl_exposure_total = dr_label(row_filter, DR_FONT_FILTER,
        app_config_apply_brightness(current_theme->filter_text_color, gb), "");
    nina_dashboard_bind_tap(p->lbl_exposure_total, NINA_TAP_FILTER);
    p->lbl_loop_count = dr_label(row_filter, DR_FONT_COUNT,
        app_config_apply_brightness(current_theme->label_color, gb), "");
    nina_dashboard_bind_tap(p->lbl_loop_count, NINA_TAP_SESSION);

    /* 6: the two bullseyes and their figures. */
    p->rms_bull = dr_bullseye(parent, -DR_BULL_DX, DR_BULL_DY, DR_BULL_R,
        app_config_apply_brightness(current_theme->rms_color, gb), &p->rms_dot);
    nina_dashboard_bind_tap(p->rms_bull, NINA_TAP_RMS);
    p->hfr_bull = dr_bullseye(parent, DR_BULL_DX, DR_BULL_DY, DR_BULL_R,
        app_config_apply_brightness(current_theme->hfr_color, gb), &p->hfr_dot);
    nina_dashboard_bind_tap(p->hfr_bull, NINA_TAP_HFR);

    p->lbl_rms_value = dr_label(parent, DR_FONT_VALUE,
        app_config_apply_brightness(current_theme->rms_color, gb), "--");
    lv_obj_set_width(p->lbl_rms_value, 2 * DR_BULL_R + 60);
    lv_obj_align(p->lbl_rms_value, LV_ALIGN_CENTER, -DR_BULL_DX, DR_VALUE_DY);

    p->lbl_hfr_value = dr_label(parent, DR_FONT_VALUE,
        app_config_apply_brightness(current_theme->hfr_color, gb), "--");
    lv_obj_set_width(p->lbl_hfr_value, 2 * DR_BULL_R + 60);
    lv_obj_align(p->lbl_hfr_value, LV_ALIGN_CENTER, DR_BULL_DX, DR_VALUE_DY);

    /* 7: the flip figure under them. The tick on the rim is the shape; this is
     * the number the tick cannot say. */
    p->lbl_flip_value = dr_label(parent, DR_FONT_FLIP,
        app_config_apply_brightness(current_theme->text_color, gb), "--");
    lv_obj_set_width(p->lbl_flip_value, 2 * ui_chord_half(DR_FLIP_DY + 40));
    lv_obj_align(p->lbl_flip_value, LV_ALIGN_CENTER, 0, DR_FLIP_DY);
    nina_dashboard_bind_tap(p->lbl_flip_value, NINA_TAP_FLIP);

    /* ui_dial_place_dot() reads the bullseyes' resolved coordinates, so settle
     * the layout once here rather than showing both dots at the centre until
     * the first poll. */
    lv_obj_update_layout(parent);
    ui_dial_place_dot(p->rms_dot, p->rms_bull, 0.0f, DR_BEARING_RMS);
    ui_dial_place_dot(p->hfr_dot, p->hfr_bull, 0.0f, DR_BEARING_HFR);
}
