/**
 * @file octoprint_layout_glass.c
 * @brief OctoPrint layout 2 — "Immersive image" (mockup v4).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from an octo_color() token or
 * a shared style, so all nine themes work without a change here. This file never
 * reads octoprint_data_t.
 *
 * The page abandons the card grid: the print image is the ground, filling the
 * whole content area, darkened by flat and gradient scrims, with translucent
 * glass surfaces floating on top (same idiom as the Summary page: dark fill at
 * partial opacity plus a hairline border, no real blur — the P4 has no cheap
 * blur and a fake one would cost a full-screen software pass per redraw).
 *
 * Structure (688 x 688 content area, absolute placement, LV_LAYOUT_NONE):
 *   0    image ground  full-bleed hero + dim + top/bottom gradient scrims
 *   0    top bar   62  state pill | ellipsised file name | connection chip
 *   70   error strip 30 full-width fault chip (resting state reads "No faults")
 *   116  temp rail 158x296  right-edge vertical fill-to-target tubes
 *   424  hero slab 264  percentage, layer block, track + M73, three time tiles
 *
 * Deviations from the mockup are listed in the layout report; the load-bearing
 * one is that there is no source-switch chip — the image source is a config
 * choice, so the page shows one image area and tags it THUMBNAIL / WEBCAM.
 */

#include "nina_octoprint_internal.h"

/* ── Geometry ─────────────────────────────────────────────────────────── */

#define GL_TOPBAR_H    62
#define GL_ERR_Y       70
#define GL_ERR_H       30
#define GL_RAIL_W     158
#define GL_RAIL_Y     116
#define GL_RAIL_H     296
#define GL_TUBE_W      62
#define GL_SLAB_H     264
#define GL_LAYER_W    190
#define GL_PANEL_R     16
#define GL_SLAB_R      22

/* Glass: dark ground at partial opacity + hairline border (nina_summary idiom). */
#define GL_GLASS_OPA   LV_OPA_70
#define GL_BORDER_OPA  LV_OPA_40

/* ── Primitives ───────────────────────────────────────────────────────── */

/** Translucent panel floating over the image ground. */
static lv_obj_t *glass_panel(lv_obj_t *parent, int radius)
{
    lv_obj_t *p = octo_w_row(parent, true, 0);
    lv_obj_set_style_bg_opa(p, GL_GLASS_OPA, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(octo_color(OCTO_COL_BG)), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
    lv_obj_set_style_border_opa(p, GL_BORDER_OPA, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_clip_corner(p, true, 0);
    return p;
}

/**
 * Vertical darkening scrim in the page ground colour. @p top_opa / @p bot_opa
 * are the two gradient stop opacities, so a scrim can fade in either direction
 * (LVGL carries per-stop opacity in bg_main_opa / bg_grad_opa).
 */
static lv_obj_t *glass_scrim(lv_obj_t *parent, lv_opa_t top_opa, lv_opa_t bot_opa)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);

    uint32_t ground = octo_color(OCTO_COL_BG);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(ground), 0);
    lv_obj_set_style_bg_grad_color(s, lv_color_hex(ground), 0);
    lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(s, top_opa, 0);
    lv_obj_set_style_bg_grad_opa(s, bot_opa, 0);
    return s;
}

/** Small filled marker (M73 tick, layer segment). */
static lv_obj_t *glass_mark(lv_obj_t *parent, int w_px, int h_px, octo_color_id_t col)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(m, w_px, h_px);
    lv_obj_set_style_radius(m, 3, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(octo_color(col)), 0);
    return m;
}

/* ── Image ground ─────────────────────────────────────────────────────── */

static void build_ground(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* The hero fills the page. Its host is a card, so the no-image state is a
     * deliberate dark ground with a centred label rather than a hole in the UI;
     * radius and border are dropped so it truly bleeds to the edges. */
    lv_obj_t *host = octo_w_image_hero(page, w);
    lv_obj_set_size(host, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(host, 0, 0);
    lv_obj_set_style_radius(host, 0, 0);
    lv_obj_set_style_border_width(host, 0, 0);

    if (w->img_hero) {
        /* COVER, not CONTAIN: as a background the frame should fill the page. */
        lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_COVER);
    }
    if (w->img_placeholder) {
        lv_obj_set_style_text_font(w->img_placeholder, &lv_font_montserrat_20, 0);
        lv_obj_align(w->img_placeholder, LV_ALIGN_CENTER, 0, -60);
    }
    if (w->lbl_img_tag) {
        /* Clear of the top bar and the error strip, left of the temp rail. */
        lv_obj_align(w->lbl_img_tag, LV_ALIGN_TOP_LEFT, 6, GL_ERR_Y + GL_ERR_H + 12);
    }

    /* Ambient darkening, then a fade under the top bar and under the slab so the
     * glass edges never sit on a bright patch of the image. */
    lv_obj_t *dim = glass_scrim(page, LV_OPA_30, LV_OPA_30);
    lv_obj_set_size(dim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(dim, 0, 0);

    lv_obj_t *top = glass_scrim(page, LV_OPA_80, LV_OPA_TRANSP);
    lv_obj_set_size(top, LV_PCT(100), 200);
    lv_obj_set_pos(top, 0, 0);

    lv_obj_t *bottom = glass_scrim(page, LV_OPA_TRANSP, LV_OPA_90);
    lv_obj_set_size(bottom, LV_PCT(100), 340);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ── Top bar ──────────────────────────────────────────────────────────── */

static void build_topbar(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *bar = glass_panel(page, GL_PANEL_R);
    lv_obj_set_size(bar, LV_PCT(100), GL_TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_style_pad_column(bar, 12, 0);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* State line dressed as a pill. Its dot and text are recoloured live by the
     * update path, so only the pill shell is styled here. */
    lv_obj_t *pill = octo_w_state_line(bar, w);
    lv_obj_set_style_pad_hor(pill, 12, 0);
    lv_obj_set_style_pad_ver(pill, 6, 0);
    lv_obj_set_style_radius(pill, 17, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
    lv_obj_set_style_border_opa(pill, GL_BORDER_OPA, 0);

    /* File name takes the slack; flex_grow overrides the factory's 100 % width. */
    octo_w_file_label(bar, w);
    if (w->lbl_file) {
        lv_obj_set_flex_grow(w->lbl_file, 1);
    }

    octo_w_conn_chip(bar, w);
    if (w->conn_chip) {
        lv_obj_set_style_bg_opa(w->conn_chip, LV_OPA_40, 0);
        lv_obj_set_style_border_opa(w->conn_chip, GL_BORDER_OPA, 0);
    }
}

/* ── Error strip ──────────────────────────────────────────────────────── */

static void build_error_strip(lv_obj_t *page, octoprint_widgets_t *w)
{
    octo_w_status_strip(page, w);
    if (!w->error_strip) {
        return;
    }
    lv_obj_set_size(w->error_strip, LV_PCT(100), GL_ERR_H);
    lv_obj_set_pos(w->error_strip, 0, GL_ERR_Y);
    lv_obj_set_style_radius(w->error_strip, 11, 0);
    lv_obj_set_style_pad_hor(w->error_strip, 14, 0);
    lv_obj_set_style_bg_opa(w->error_strip, LV_OPA_40, 0);
    lv_obj_set_style_border_opa(w->error_strip, LV_OPA_30, 0);
    lv_obj_set_flex_align(w->error_strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
}

/* ── Temperature rail ─────────────────────────────────────────────────── */

/** Shape one vertical fill-to-target element into a tube column. */
static void tube_shape(octo_temp_el_t *t)
{
    if (!t || !t->root) {
        return;
    }
    lv_obj_set_size(t->root, GL_TUBE_W, LV_PCT(100));
    lv_obj_set_style_pad_row(t->root, 6, 0);

    if (t->bar) {
        lv_obj_set_width(t->bar, 30);
        lv_obj_set_style_radius(t->bar, 14, 0);
        lv_obj_set_style_radius(t->bar, 14, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(t->bar, LV_OPA_40, 0);
        /* Tube on top, value under it, name at the foot (mockup order). The
         * factory builds name/value/bar/scale, so two moves reorder it. */
        lv_obj_move_to_index(t->bar, 0);
    }
    if (t->lbl_value) {
        lv_obj_set_style_text_font(t->lbl_value, &lv_font_montserrat_14, 0);
        lv_obj_set_width(t->lbl_value, LV_PCT(100));
        lv_label_set_long_mode(t->lbl_value, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(t->lbl_value, LV_TEXT_ALIGN_CENTER, 0);
        /* Fixed height so an uneven wrap between the two tubes cannot make the
         * bars end at different heights. */
        lv_obj_set_height(t->lbl_value, 38);
    }
    if (t->lbl_name) {
        lv_obj_set_width(t->lbl_name, LV_PCT(100));
        lv_obj_set_style_text_align(t->lbl_name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_move_to_index(t->lbl_name, 2);
    }
    if (t->lbl_scale) {
        /* Redundant here: the target already appears in the value string. */
        lv_obj_add_flag(t->lbl_scale, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_rail(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *rail = glass_panel(page, 18);
    lv_obj_set_size(rail, GL_RAIL_W, GL_RAIL_H);
    lv_obj_align(rail, LV_ALIGN_TOP_RIGHT, 0, GL_RAIL_Y);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(rail, 12, 0);
    lv_obj_set_style_pad_hor(rail, 10, 0);
    lv_obj_set_style_pad_row(rail, 10, 0);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = octo_w_caption(rail, "HEAT");
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *tubes = octo_w_row(rail, true, 14);
    lv_obj_set_width(tubes, LV_PCT(100));
    lv_obj_set_flex_grow(tubes, 1);
    lv_obj_set_flex_align(tubes, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);

    octo_w_temp(tubes, "NOZ", true, true, &w->nozzle);
    tube_shape(&w->nozzle);

    octo_w_temp(tubes, "BED", true, false, &w->bed);
    tube_shape(&w->bed);
}

/* ── Progress slab ────────────────────────────────────────────────────── */

static void build_percent(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *col = octo_w_row(parent, false, 0);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    lv_obj_t *row = octo_w_row(col, true, 4);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* The mockup sets the percentage in Playfair; that face carries only digits
     * and ':', and the value is rendered "61.8", so Montserrat 64 is used. */
    w->lbl_pct = octo_w_label(row, "--", &lv_font_montserrat_64, &octo_style_value);
    w->lbl_pct_unit = octo_w_label(row, "%", &lv_font_montserrat_28, &octo_style_accent);
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, 8, 0);

    w->lbl_pct_sub = octo_w_caption(col, "COMPLETE");
    lv_obj_set_style_margin_top(w->lbl_pct_sub, 4, 0);
}

static void build_layer_block(lv_obj_t *parent, octoprint_widgets_t *w)
{
    /* Layer number, caption and segment strip all live inside layer_cell so the
     * update path hides the whole block in one go when DisplayLayerProgress is
     * not installed — nothing half-lit is left behind. */
    w->layer_cell = octo_w_row(parent, false, 0);
    lv_obj_set_size(w->layer_cell, GL_LAYER_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *nums = octo_w_row(w->layer_cell, true, 6);
    lv_obj_set_size(nums, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(nums, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(nums, "--", &lv_font_montserrat_36,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(nums, "/ --", &lv_font_montserrat_22,
                                      &octo_style_label);

    lv_obj_t *cap = octo_w_caption(w->layer_cell, "LAYER");
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_margin_top(cap, 2, 0);

    lv_obj_t *segs = octo_w_row(w->layer_cell, true, 3);
    lv_obj_set_width(segs, LV_PCT(100));
    lv_obj_set_height(segs, 6);
    lv_obj_set_style_margin_top(segs, 10, 0);
    for (int i = 0; i < OCTO_LAYER_SEGS; i++) {
        lv_obj_t *seg = glass_mark(segs, 0, 6, OCTO_COL_BORDER);
        lv_obj_set_flex_grow(seg, 1);
        w->layer_segs[i] = seg;
    }
}

static void build_track(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *host = octo_w_row(parent, false, 6);
    lv_obj_set_width(host, LV_PCT(100));
    lv_obj_set_height(host, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* Firmware M73 read-out above the track. The update path hides this row when
     * M73 is absent; it cannot move it, so it sits at the end of the track
     * rather than at the live M73 position. */
    w->m73_row = octo_w_row(host, true, 7);
    lv_obj_set_size(w->m73_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->m73_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    w->lbl_m73 = octo_w_label(w->m73_row, "M73 --", &lv_font_montserrat_12,
                              &octo_style_label);
    glass_mark(w->m73_row, 2, 15, OCTO_COL_TEXT);

    octo_w_progress_bar(host, w);
    if (w->bar_progress) {
        lv_obj_set_height(w->bar_progress, 22);
        lv_obj_set_style_radius(w->bar_progress, 11, 0);
        lv_obj_set_style_radius(w->bar_progress, 11, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(w->bar_progress, LV_OPA_40, 0);
        lv_obj_set_style_border_opa(w->bar_progress, GL_BORDER_OPA, 0);
    }
}

/** Dress a time tile as a glass cell. @p accent marks the finish tile. */
static void time_tile_shape(lv_obj_t *tile, bool accent)
{
    if (!tile) {
        return;
    }
    lv_obj_set_height(tile, LV_PCT(100));
    lv_obj_set_flex_grow(tile, 1);
    lv_obj_set_style_radius(tile, 14, 0);
    lv_obj_set_style_pad_hor(tile, 12, 0);
    lv_obj_set_style_pad_ver(tile, 9, 0);
    lv_obj_set_style_pad_row(tile, 4, 0);
    lv_obj_set_style_bg_opa(tile, accent ? LV_OPA_20 : LV_OPA_10, 0);
    lv_obj_set_style_bg_color(
        tile, lv_color_hex(octo_color(accent ? OCTO_COL_ACCENT : OCTO_COL_BORDER)), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(
        tile, lv_color_hex(octo_color(accent ? OCTO_COL_ACCENT : OCTO_COL_BORDER)), 0);
    lv_obj_set_style_border_opa(tile, GL_BORDER_OPA, 0);
}

static void build_times(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *times = octo_w_row(parent, true, 10);
    lv_obj_set_width(times, LV_PCT(100));
    lv_obj_set_height(times, 72);

    time_tile_shape(octo_w_time_tile(times, "ELAPSED", &lv_font_montserrat_26,
                                     &w->lbl_elapsed), false);
    time_tile_shape(octo_w_time_tile(times, "REMAINING", &lv_font_montserrat_26,
                                     &w->lbl_remaining), false);

    /* Finish time is DisplayLayerProgress-only, so its tile is the handle the
     * update path hides wholesale. */
    w->finish_cell = octo_w_time_tile(times, "FINISH AT", &lv_font_montserrat_26,
                                      &w->lbl_finish);
    time_tile_shape(w->finish_cell, true);
    if (w->lbl_finish) {
        /* Added after the factory's value style, so accent wins — and it stays
         * correct across a theme change because it is a shared style. */
        lv_obj_add_style(w->lbl_finish, &octo_style_accent, 0);
    }
}

static void build_slab(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *slab = glass_panel(page, GL_SLAB_R);
    lv_obj_set_size(slab, LV_PCT(100), GL_SLAB_H);
    lv_obj_align(slab, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(slab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(slab, 16, 0);
    lv_obj_set_style_pad_bottom(slab, 14, 0);
    lv_obj_set_style_pad_hor(slab, 20, 0);
    lv_obj_set_style_pad_row(slab, 12, 0);

    lv_obj_t *top = octo_w_row(slab, true, 18);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    build_percent(top, w);
    build_layer_block(top, w);
    build_track(slab, w);
    build_times(slab, w);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void glass_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Absolute placement: the surfaces overlap the image ground, which no flex
     * or grid flow can express. */
    lv_obj_set_layout(page, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(page, 0, 0);

    build_ground(page, w);
    build_topbar(page, w);
    build_error_strip(page, w);
    build_rail(page, w);
    build_slab(page, w);
}

const octoprint_layout_ops_t octoprint_layout_glass = {
    .name  = "Immersive image",
    .build = glass_build,
};
