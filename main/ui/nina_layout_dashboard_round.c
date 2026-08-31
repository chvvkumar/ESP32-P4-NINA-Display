/**
 * @file nina_layout_dashboard_round.c
 * @brief NINA layout 0 (Dashboard) on a round panel: radial board 1.
 *
 * On round this board is one of the four view states of a picture page: the
 * spine builds the capture underneath it and nina_round_overlay.c draws the
 * safety shield crown, the rim exposure arc and the readings plate over the
 * picture. Everything this file builds therefore belongs to the readings-only
 * view alone, and set_view() hides the lot in the other three.
 *
 * The rim carries progress and time: an exposure ring flush with the glass, a
 * sub ring 28 px inside it built by nina_subbar, and a meridian-flip tick that
 * climbs the left rim and reaches twelve at flip. Both rings leave only the
 * small clearance the overlay's shield needs at twelve; the board's own safety
 * crown is gone, because the shield is the overlay's. The centre holds one
 * spine of identity, target, step, elapsed seconds and the filter plus counter
 * row; below it two miniature trend graphs plot guiding RMS and HFR against
 * their tolerances, one per half of the line, with the figure centred under
 * each and the flip figure under those.
 *
 * The page owns the data, the timers and the formatting: this file creates and
 * places widgets and writes the handles, nothing else. Every widget it does not
 * build stays NULL and nina_dashboard_update.c null-checks it, which is what
 * lets p->ring_crown simply stop being created. Runs with the LVGL display
 * lock held by the caller.
 *
 * Geometry is centre-relative and in absolute pixels, so the 800 panel gets the
 * same composition with more black margin and wider chords.
 */

#include "nina_layout_alt.h"

#include <math.h>
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

/* Crown clearance. The old 40 degree gap existed to hold this board's own
 * safety crown, which is now the shared overlay's shield glyph at twelve. All
 * the rings still owe that glyph is room: the same rule Halo and the overlay
 * use, an ABSOLUTE clearance each side of the 40 px shield rather than a fixed
 * angle, so the clear space is the same on both panel sizes. */
#define DR_SHIELD_HALF      20
#define DR_CROWN_CLEAR      14

/* Flip tick: 30 px long, centred on the exposure ring, 2 degrees wide. */
#define DR_R_TICK_OFF       15
#define DR_W_TICK           30

/* Centre spine and the block below it, as offsets from the panel centre. The
 * two RMS/HFR trend graphs split the line: each takes one half, mirrored about
 * the vertical centreline, with its figure centred to the graph width below. */
#define DR_SPINE_TOP_DY    (-268)
#define DR_GRAPH_DY         122
#define DR_GRAPH_DX         136
#define DR_GRAPH_W          250
#define DR_GRAPH_H          96
#define DR_GRAPH_POINTS     90
/* Figure box narrower than the graph so its corners stay off the sub ring at
 * y 218; it is centred on the graph either way. */
#define DR_VALUE_W          200
#define DR_VALUE_DY         218
#define DR_FLIP_DY          283

/* Spine gaps. The 96 px Hanken face reports line_height 69, not the 108 px box
 * the mockup draws, so the column runs about 95 px short of radial.html unless
 * the two gaps below the step absorb it (review C minor M-10, margin option).
 * With spine top at centre-268 = y 92 the boxes land at: identity 92..122,
 * target 132..176, step 182..212, elapsed 242..311, filter row 335..365. The
 * filter gap dropped from 65 to 24 to pull the filter row up and clear the two
 * trend graphs that replaced the bullseyes (top edge at centre+74 = y 434). */
#define DR_GAP_TARGET       10
#define DR_GAP_STEP          6
#define DR_GAP_ELAPSED      30
#define DR_GAP_FILTER       24

/* Graph scale: DR_GRAPH_RANGE lives in nina_dashboard_internal.h, single
 * owner shared with the feeder in nina_dashboard_update.c. */

#define DR_TRACK_COLOR      0x161616
#define DR_RING_BORDER      0x262a30

/* Fonts. The 108 px elapsed of the mockup maps to the existing 96 px Hanken
 * Black, whose glyph set is digits and colon only, so its unit "s" is a
 * separate 28 px Hanken Bold label in the same row. The 40 px graph figures
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

static int       dr_gap_half_deg(int r);
static lv_obj_t *dr_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, const char *text);
static lv_obj_t *dr_row(lv_obj_t *parent, int gap);
static lv_obj_t *dr_graph(lv_obj_t *parent, int dx, uint32_t color,
                          lv_chart_series_t **out_ser);
static void      dr_elapsed_cb(void *ud, int secs);

/* Half the angular gap the overlay's shield needs at radius @p r, in degrees.
 * An lv_arc wants this half angle on each of its two sides;
 * nina_subbar_create_ring() wants the WHOLE gap, so its caller doubles it. */
static int dr_gap_half_deg(int r) {
    if (r <= 0) return 0;
    float sn = (float)(DR_SHIELD_HALF + DR_CROWN_CLEAR) / (float)r;
    if (sn > 1.0f) sn = 1.0f;
    return (int)lroundf(asinf(sn) * 180.0f / (float)M_PI);
}

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

/* Miniature trend graph: one line series scaled as ratio-vs-tolerance x100
 * (DR_GRAPH_RANGE tops out at twice the tolerance), SHIFT update mode so the
 * feeder in nina_dashboard_update.c pushes one point per poll cycle. Faint
 * horizontal div lines only; the middle one is the tolerance itself. */
static lv_obj_t *dr_graph(lv_obj_t *parent, int dx, uint32_t color,
                          lv_chart_series_t **out_ser) {
    lv_obj_t *ch = lv_chart_create(parent);
    lv_obj_remove_style_all(ch);
    lv_obj_remove_flag(ch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ch, DR_GRAPH_W, DR_GRAPH_H);
    lv_obj_align(ch, LV_ALIGN_CENTER, dx, DR_GRAPH_DY);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 1, 0);
    lv_obj_set_style_border_color(ch, lv_color_hex(DR_RING_BORDER), 0);
    lv_obj_set_style_border_opa(ch, LV_OPA_COVER, 0);
    lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(ch, lv_color_hex(DR_TRACK_COLOR), LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, DR_GRAPH_POINTS);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, DR_GRAPH_RANGE);
    *out_ser = lv_chart_add_series(ch, lv_color_hex(color),
                                   LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(ch, *out_ser, LV_CHART_POINT_NONE);
    return ch;
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
    const int g_exp  = dr_gap_half_deg(r_exp);
    const int g_sub  = dr_gap_half_deg(r_sub);

    /* 0: one full-panel transparent group holding EVERY object this board
     * builds. The board is the readings-only view of a picture page, so
     * set_view() hides it with a single flag; a hidden parent takes its whole
     * subtree with it, which matters because the classic updaters own the
     * HIDDEN flag of some children (the exposure arc on disconnect, the flip
     * tick) and would otherwise un-hide them behind the picture. */
    p->alt.grp_mid = lv_obj_create(parent);
    lv_obj_remove_style_all(p->alt.grp_mid);
    lv_obj_remove_flag(p->alt.grp_mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.grp_mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(p->alt.grp_mid, LV_LAYOUT_NONE);
    lv_obj_set_size(p->alt.grp_mid, screen_size(), screen_size());
    lv_obj_center(p->alt.grp_mid);
    lv_obj_set_style_pad_all(p->alt.grp_mid, 0, 0);
    lv_obj_set_style_pad_gap(p->alt.grp_mid, 0, 0);
    lv_obj_t *board = p->alt.grp_mid;

    /* 1: the exposure ring. This IS p->arc_exposure, so every line of the
     * shipped exposure model (seed, long linear anim, completion snap, gap
     * fade) drives it with no change. ring_exposure names the same object so
     * the stale cue knows there is a rim ring to dim. */
    p->arc_exposure = lv_arc_create(board);
    lv_obj_set_size(p->arc_exposure, 2 * r_exp + DR_W_EXPOSURE,
                                     2 * r_exp + DR_W_EXPOSURE);
    lv_obj_center(p->arc_exposure);
    lv_arc_set_rotation(p->arc_exposure, (270 + g_exp) % 360);
    lv_arc_set_bg_angles(p->arc_exposure, 0, 360 - 2 * g_exp);
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

    /* The ring's leading-edge cap. lv_arc only fills whole degrees, about 7 px
     * at this radius, so the cap rides the exact animated value and the
     * quantised band trails under it. It is a child of the ring, so it hides
     * and dims with it, and the descriptor hangs on the ring's user data so
     * every arc_exposure animation (arc_exec_cb in nina_dashboard_update.c)
     * places it from the value it just set. */
    p->alt.cap_progress_num.obj = ui_dial_cap_create(p->arc_exposure,
        DR_W_EXPOSURE,
        app_config_apply_brightness(current_theme->progress_color, gb));
    p->alt.cap_progress_num.r     = r_exp;
    p->alt.cap_progress_num.a0    = g_exp;
    p->alt.cap_progress_num.sweep = 360 - 2 * g_exp;
    lv_obj_set_user_data(p->arc_exposure, &p->alt.cap_progress_num);

    /* 2: no safety crown any more. The shield at twelve is the shared
     * overlay's, so p->ring_crown stays NULL and update_safety_icon() returns
     * early on this board, taking the crown's colour cues with it. */

    /* 3: the meridian-flip tick. Hidden until a countdown arrives. */
    p->ring_flip_tick = ui_dial_arc(board, r_tick, DR_W_TICK, 0, 2);
    lv_obj_set_style_arc_color(p->ring_flip_tick,
        lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
        LV_PART_MAIN);
    lv_obj_add_flag(p->ring_flip_tick, LV_OBJ_FLAG_HIDDEN);

    /* 4: the sub ring, same block rule as the square ledge. */
    nina_subbar_create_ring(&p->subbar, board, r_sub, DR_W_SUB, 2 * g_sub);
    p->subbar.hide_single = true;   /* one sub: the exposure ring is enough */
    nina_subbar_set_elapsed_cb(&p->subbar, dr_elapsed_cb, p);

    /* 5: the centre spine. One flex column on the widest chords it needs. */
    lv_obj_t *spine = lv_obj_create(board);
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

    /* 6: the two trend graphs, one per half of the line, each figure centred
     * to its graph's width directly below it. */
    p->rms_chart = dr_graph(board, -DR_GRAPH_DX,
        app_config_apply_brightness(current_theme->rms_color, gb), &p->rms_ser);
    nina_dashboard_bind_tap(p->rms_chart, NINA_TAP_RMS);
    p->hfr_chart = dr_graph(board, DR_GRAPH_DX,
        app_config_apply_brightness(current_theme->hfr_color, gb), &p->hfr_ser);
    nina_dashboard_bind_tap(p->hfr_chart, NINA_TAP_HFR);

    p->lbl_rms_value = dr_label(board, DR_FONT_VALUE,
        app_config_apply_brightness(current_theme->rms_color, gb), "--");
    lv_obj_set_width(p->lbl_rms_value, DR_VALUE_W);
    lv_obj_align(p->lbl_rms_value, LV_ALIGN_CENTER, -DR_GRAPH_DX, DR_VALUE_DY);

    p->lbl_hfr_value = dr_label(board, DR_FONT_VALUE,
        app_config_apply_brightness(current_theme->hfr_color, gb), "--");
    lv_obj_set_width(p->lbl_hfr_value, DR_VALUE_W);
    lv_obj_align(p->lbl_hfr_value, LV_ALIGN_CENTER, DR_GRAPH_DX, DR_VALUE_DY);

    /* 7: the flip figure under them. The tick on the rim is the shape; this is
     * the number the tick cannot say. */
    p->lbl_flip_value = dr_label(board, DR_FONT_FLIP,
        app_config_apply_brightness(current_theme->text_color, gb), "--");
    lv_obj_set_width(p->lbl_flip_value, 2 * ui_chord_half(DR_FLIP_DY + 40));
    lv_obj_align(p->lbl_flip_value, LV_ALIGN_CENTER, 0, DR_FLIP_DY);
    nina_dashboard_bind_tap(p->lbl_flip_value, NINA_TAP_FLIP);
}

/* The whole board is the readings-only view. FULL, ARC and PICTURE are the
 * capture with the shared overlay over it, and the overlay already draws the
 * shield, the rim arc and the readings there, so every object this file built
 * stands down in those three: one flag on the group that owns them all.
 *
 * The classic spine updaters keep writing these widgets while the group is
 * hidden. That is deliberate and cheap: they are LVGL writes into an
 * undrawn subtree, so the board is already correct the moment a tap brings it
 * back. Never touch p->alt.cap_img here: the spine sets its opacity at the
 * single choke point in nina_layout_alt_set_view(), and it is never hidden,
 * because it carries the tap. */
void nina_layout_dashboard_round_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    if (!p || !p->alt.grp_mid) return;
    if (mode == NINA_VIEW_NUMBERS) {
        lv_obj_remove_flag(p->alt.grp_mid, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(p->alt.grp_mid, LV_OBJ_FLAG_HIDDEN);
    }
}
