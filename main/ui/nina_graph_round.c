/**
 * @file nina_graph_round.c
 * @brief Round composition of the RMS/HFR history overlay, inscribed board 10.
 *
 * A fit pass over what nina_graph_overlay_create() built: the chart becomes a
 * rim-sized disc (2 * ui_rim_radius(), 684 at 720 and 760 at 800) with
 * clip_corner on, so the dashed threshold lines, which are chart CHILDREN,
 * become chords; the chart's OWN division lines are dropped instead, because
 * clip_corner wraps the children layer only and lv_chart draws them (and the
 * series) in LV_EVENT_DRAW_MAIN on the parent layer. The y-axis labels leave
 * their fixed left column and step inward with the chord, each left-anchored on
 * its own knock-out bed; the title goes to the top cap; the summary, the x-axis
 * title and the legend are dropped; and the two control rows collapse to one
 * row of point pills on the lower chord with BACK on the bottom cardinal. The
 * Y-scale choice becomes a tap-cycle on the y-axis layer (y_scale_cycle_cb in
 * nina_graph_controls.c), installed through the graph_controls_builder hook so
 * this page keeps exactly one family conditional.
 *
 * clip_corner renders in software: CONFIG_LV_USE_PPA is n because LVGL's PPA
 * fill unit drops clip_corner sub-layers that are not 128 B size aligned. Do
 * not re-enable it.
 *
 * Display lock held by the caller.
 */

#include "nina_graph_internal.h"
#include "ui_round.h"
#include "screen_geom.h"
#include "lvgl.h"
#include <stdio.h>

#define GRR_TITLE_DY     36     /* from the chart's top edge */
#define GRR_PILL_ROW_DY 216
#define GRR_PILL_ROW_H   48
#define GRR_BACK_W      140
#define GRR_BACK_H       56
#define GRR_BACK_DY      48     /* pixels from the overlay's bottom edge */

/* The chart is the rim circle, not a fixed 684: the label x offsets below come
 * from ui_chord_half(), which uses Rs, so a chart smaller than 2 * Rs at 800
 * would clip every label away. 684 at 720, 760 at 800. */
static inline int grr_chart_sz(void) { return 2 * ui_rim_radius(); }

/* The five y-label offsets from the chart centre. update_y_labels() writes top,
 * +half, 0, -half, bottom, so the labels mark 0, 1/4, 1/2, 3/4 and 1 of the
 * range: quarters of the data band, NOT the chart's division-line thirds. The
 * two extremes are pulled in from +-334 to +-320 so a 27 px label still fits
 * the chord there (half chord at 320 is 120). */
static const int s_y_dy[5] = { -320, -167, 0, 167, 320 };

/* Left-anchored: the text is still "" at this point (the values arrive later
 * from update_y_labels), so a placement computed from the measured width would
 * walk the label out of the circle once it fills. Pin the LEFT edge just inside
 * the chord and let the label grow rightward into the plot. */
static void fit_y_label(lv_obj_t *lbl, int dy)
{
    if (!lbl) return;
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    /* Knock-out bed: the labels now sit over the series, which the square
     * column never did. */
    lv_obj_set_style_bg_color(lbl, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(lbl, 6, 0);
    lv_obj_set_style_pad_ver(lbl, 2, 0);
    lv_obj_set_style_radius(lbl, 6, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID,
                 grr_chart_sz() / 2 - ui_chord_half(dy) + 12, dy);
}

void graph_round_fit(void)
{
    if (!overlay || !chart) return;
    const int chart_sz = grr_chart_sz();

    lv_obj_set_layout(overlay, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    if (chart_area) {
        lv_obj_set_size(chart_area, chart_sz, chart_sz);
        lv_obj_align(chart_area, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_set_size(chart, chart_sz, chart_sz);
    lv_obj_set_style_radius(chart, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(chart, true, 0);
    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);

    /* clip_corner wraps the CHILDREN layer only: LV_EVENT_DRAW_MAIN fires on
     * the parent layer before it, and lv_chart draws its division lines and its
     * series there. Full-width division lines would therefore show in the strip
     * between the rim and the physical edge, so drop them. The dashed threshold
     * lines are real lv_line children and ARE clipped, so they stay. */
    lv_chart_set_div_line_count(chart, 0, 0);

    /* The y-axis column becomes a full-plot placement layer and the tap target
     * for the scale cycle. A tapered column is not a reachable target on a
     * circle, so the whole plot answers the tap. */
    if (y_label_col) {
        lv_obj_set_layout(y_label_col, LV_LAYOUT_NONE);
        lv_obj_set_size(y_label_col, chart_sz, chart_sz);
        lv_obj_set_style_pad_all(y_label_col, 0, 0);
        lv_obj_align(y_label_col, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(y_label_col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(y_label_col, y_scale_cycle_cb, LV_EVENT_CLICKED, NULL);
    }
    fit_y_label(lbl_y_top, s_y_dy[0]);
    fit_y_label(lbl_y_q1,  s_y_dy[1]);
    fit_y_label(lbl_y_mid, s_y_dy[2]);
    fit_y_label(lbl_y_q3,  s_y_dy[3]);
    fit_y_label(lbl_y_bot, s_y_dy[4]);

    if (lbl_title) {
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_28, 0);
        lv_obj_set_style_bg_opa(lbl_title, LV_OPA_TRANSP, 0);   /* C1 */
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, GRR_TITLE_DY);
        lv_obj_move_foreground(lbl_title);
    }
    /* Ships at lv_font_montserrat_20, under the 27 px round floor. */
    if (loading_lbl) lv_obj_set_style_text_font(loading_lbl, &lv_font_montserrat_28, 0);

    /* Dropped on round. Every use site is NULL guarded, so deleting is the
     * cheapest way to keep them from being un-hidden by the data setters. */
    if (lbl_summary) { lv_obj_delete(lbl_summary); lbl_summary = NULL; }
    if (lbl_x_title) { lv_obj_delete(lbl_x_title); lbl_x_title = NULL; }
    if (legend_cont) { lv_obj_delete(legend_cont); legend_cont = NULL; }

    if (btn_back) {
        lv_obj_set_size(btn_back, GRR_BACK_W, GRR_BACK_H);
        lv_obj_set_style_radius(btn_back, LV_RADIUS_CIRCLE, 0);
        /* Centre relative, so the bottom cap means the same thing at 800. */
        lv_obj_align(btn_back, LV_ALIGN_CENTER, 0,
                     screen_center() - GRR_BACK_DY - GRR_BACK_H / 2);
        lv_obj_move_foreground(btn_back);
    }
    if (btn_back_lbl) {
        lv_obj_set_style_text_font(btn_back_lbl, &lv_font_montserrat_28, 0);
    }

    /* Install the controls hook LAST: rebuild_controls() delegates to it from
     * here on, which is what keeps the second family conditional out of
     * nina_graph_controls.c (addendum section 6, rule 1). */
    graph_controls_builder = graph_round_rebuild_controls;
}

void graph_round_rebuild_controls(void)
{
    if (!overlay) return;

    if (controls_cont) {
        lv_obj_delete(controls_cont);
        controls_cont = NULL;
    }

    /* One row on the lower chord. Width comes from the chord at the row's own
     * lower edge so the pills never leave the circle at either panel size. */
    int row_w = ui_chord_at_y(screen_center() + GRR_PILL_ROW_DY + GRR_PILL_ROW_H / 2)
                - 2 * UI_SQUARE_INSET;

    controls_cont = lv_obj_create(overlay);
    lv_obj_remove_style_all(controls_cont);
    lv_obj_set_size(controls_cont, row_w, GRR_PILL_ROW_H);
    lv_obj_set_flex_flow(controls_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(controls_cont, 6, 0);
    lv_obj_clear_flag(controls_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(controls_cont, LV_ALIGN_CENTER, 0, GRR_PILL_ROW_DY);

    for (int i = 0; i < POINT_OPT_COUNT; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", point_options[i]);
        btn_points[i] = make_pill_btn(controls_cont, buf, (i == selected_points_idx),
                                      point_btn_cb, i);
        /* make_pill_btn labels at 18 px; the round floor is 27. */
        lv_obj_t *lbl = lv_obj_get_child(btn_points[i], 0);
        if (lbl) lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    }

    /* The scale pills do not exist on round (the y-axis tap-cycle replaces
     * them), but scale_btn_count still bounds selected_scale_idx. */
    scale_btn_count = (current_type == GRAPH_TYPE_RMS) ? RMS_SCALE_COUNT : HFR_SCALE_COUNT;
    for (int i = 0; i < RMS_SCALE_COUNT; i++) btn_scale[i] = NULL;
    if (selected_scale_idx >= scale_btn_count) selected_scale_idx = 0;

    if (btn_back) lv_obj_move_foreground(btn_back);
}
