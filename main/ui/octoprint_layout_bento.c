/**
 * @file octoprint_layout_bento.c
 * @brief OctoPrint layout 0 — "Bento dashboard" (reference layout, mockup v1).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from current_theme, so all
 * nine themes work without a change here. This file never reads octoprint_data_t.
 *
 * Structure (688 x 688 content area, 12 px gaps):
 *   header 56      state line + file name | connection chip + fault strip
 *   heroes 360     progress card (dual concentric arc) | image hero
 *   temps  120     one full-width cell, two fill-to-target rows
 *   stats  rest    layer count + 12-segment strip | finish time
 *
 * Deviation from the mockup: no webcam picture-in-picture. The image hero is a
 * single area showing whichever source the config selects (the tag label in the
 * hero says which), so there is no source-switch affordance to draw.
 */

#include "nina_octoprint_internal.h"

#define BENTO_GAP        12
#define BENTO_HDR_H      56
#define BENTO_HERO_H     360
#define BENTO_HERO_W     338
#define BENTO_TEMPS_H    120
#define BENTO_ARC_OUTER  262
#define BENTO_ARC_INNER  208

/* ── Header ───────────────────────────────────────────────────────────── */

static void build_header(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *hdr = octo_w_card(page);
    lv_obj_set_size(hdr, LV_PCT(100), BENTO_HDR_H);
    lv_obj_set_style_pad_hor(hdr, 18, 0);
    lv_obj_set_style_pad_column(hdr, 14, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Mockup's diagonal navy wash across the header strip. */
    octo_w_header_wash(hdr);

    /* Middle column: state line over the file name. */
    lv_obj_t *mid = octo_w_row(hdr, false, 2);
    lv_obj_set_height(mid, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    octo_w_state_line(mid, w);
    octo_w_file_label(mid, w);

    /* Right column: connection chip, then the fault strip in fixed geometry so
     * nothing reflows when a fault appears. */
    lv_obj_t *right = octo_w_row(hdr, false, 3);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    octo_w_conn_chip(right, w);
    octo_w_status_strip(right, w);
}

/* ── Progress hero ────────────────────────────────────────────────────── */

static void build_progress_hero(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *card = octo_w_card(parent);
    lv_obj_set_size(card, BENTO_HERO_W, BENTO_HERO_H);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(card, 16, 0);
    lv_obj_set_style_pad_hor(card, 20, 0);
    lv_obj_set_style_pad_bottom(card, 14, 0);

    lv_obj_t *cap = octo_w_caption(card, "PROGRESS");
    lv_obj_set_width(cap, LV_PCT(100));

    /* Dual concentric arcs: outer = OctoPrint completion, inner hairline = M73. */
    lv_obj_t *outer = octo_w_progress_arc(card, BENTO_ARC_OUTER, 18, false, w);
    lv_obj_set_style_margin_top(outer, 8, 0);

    lv_obj_t *inner = octo_w_progress_arc(outer, BENTO_ARC_INNER, 4, true, w);
    lv_obj_center(inner);

    /* Centred label stack, added after the inner arc so it draws on top. */
    lv_obj_t *ctr = octo_w_row(outer, false, 0);
    lv_obj_set_size(ctr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(ctr);
    lv_obj_set_flex_align(ctr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *pct_row = octo_w_row(ctr, true, 2);
    lv_obj_set_size(pct_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(pct_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_pct = octo_w_label(pct_row, "--", &lv_font_montserrat_64,
                              &octo_style_value);
    w->lbl_pct_unit = octo_w_label(pct_row, "%", &lv_font_montserrat_26,
                                   &octo_style_label);

    w->lbl_pct_sub = octo_w_caption(ctr, "COMPLETE");
    lv_obj_set_style_margin_top(w->lbl_pct_sub, 6, 0);

    /* M73 indicator: accent tick + text, hidden by the update path when absent. */
    w->m73_row = octo_w_row(ctr, true, 7);
    lv_obj_set_size(w->m73_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->m73_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(w->m73_row, 5, 0);
    lv_obj_t *m73_tick = lv_obj_create(w->m73_row);
    lv_obj_remove_style_all(m73_tick);
    lv_obj_remove_flag(m73_tick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(m73_tick, 14, 3);
    lv_obj_set_style_radius(m73_tick, 2, 0);
    lv_obj_set_style_bg_opa(m73_tick, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m73_tick, lv_color_hex(octo_color(OCTO_COL_ACCENT)), 0);
    w->lbl_m73 = octo_w_label(w->m73_row, "M73 --", &lv_font_montserrat_12,
                              &octo_style_label);

    /* Footer: elapsed left, remaining right — so the stats row need not repeat
     * them and can carry only what is not already on screen. */
    lv_obj_t *foot = octo_w_row(card, true, 0);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(foot, 10, 0);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    octo_w_time_tile(foot, "ELAPSED", &lv_font_montserrat_20, &w->lbl_elapsed);
    lv_obj_t *rem = octo_w_time_tile(foot, "REMAINING", &lv_font_montserrat_20,
                                     &w->lbl_remaining);
    lv_obj_set_style_text_align(rem, LV_TEXT_ALIGN_RIGHT, 0);
}

/* ── Heroes row ───────────────────────────────────────────────────────── */

static void build_heroes(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, BENTO_GAP);
    lv_obj_set_size(row, LV_PCT(100), BENTO_HERO_H);

    build_progress_hero(row, w);

    lv_obj_t *img = octo_w_image_hero(row, w);
    lv_obj_set_size(img, BENTO_HERO_W, BENTO_HERO_H);
}

/* ── Temperatures ─────────────────────────────────────────────────────── */

static void build_temps(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *card = octo_w_card(page);
    lv_obj_set_size(card, LV_PCT(100), BENTO_TEMPS_H);
    lv_obj_set_style_pad_ver(card, 14, 0);
    lv_obj_set_style_pad_hor(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    lv_obj_t *n = octo_w_temp(card, "NOZZLE", false, true, &w->nozzle);
    lv_obj_set_size(n, LV_PCT(100), 42);

    lv_obj_t *b = octo_w_temp(card, "BED", false, false, &w->bed);
    lv_obj_set_size(b, LV_PCT(100), 42);
}

/* ── Stats row ────────────────────────────────────────────────────────── */

static void build_stats(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, BENTO_GAP);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);

    /* Layer cell — hidden wholesale by the update path when DLP is unavailable. */
    w->layer_cell = octo_w_card(row);
    lv_obj_set_height(w->layer_cell, LV_PCT(100));
    lv_obj_set_flex_grow(w->layer_cell, 1);
    lv_obj_set_style_pad_ver(w->layer_cell, 14, 0);
    lv_obj_set_style_pad_hor(w->layer_cell, 18, 0);
    lv_obj_set_flex_flow(w->layer_cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    octo_w_caption(w->layer_cell, "LAYER");

    lv_obj_t *lay = octo_w_row(w->layer_cell, true, 6);
    lv_obj_set_size(lay, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(lay, 7, 0);
    lv_obj_set_flex_align(lay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(lay, "--", &lv_font_montserrat_36,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(lay, "/ --", &lv_font_montserrat_18,
                                      &octo_style_label);

    lv_obj_t *segs = octo_w_row(w->layer_cell, true, 2);
    lv_obj_set_width(segs, LV_PCT(100));
    lv_obj_set_height(segs, 6);
    lv_obj_set_style_margin_top(segs, 11, 0);
    for (int i = 0; i < OCTO_LAYER_SEGS; i++) {
        lv_obj_t *seg = lv_obj_create(segs);
        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(seg, 6);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_radius(seg, 2, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
        w->layer_segs[i] = seg;
    }

    /* Finish cell — also DLP-sourced, so it hides with the layer cell. */
    w->finish_cell = octo_w_card(row);
    lv_obj_set_height(w->finish_cell, LV_PCT(100));
    lv_obj_set_flex_grow(w->finish_cell, 1);
    lv_obj_set_style_pad_ver(w->finish_cell, 14, 0);
    lv_obj_set_style_pad_hor(w->finish_cell, 18, 0);
    lv_obj_set_flex_flow(w->finish_cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w->finish_cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    octo_w_caption(w->finish_cell, "FINISH AT");
    w->lbl_finish = octo_w_label(w->finish_cell, "--", &lv_font_montserrat_36,
                                 &octo_style_value);
    lv_obj_set_style_margin_top(w->lbl_finish, 7, 0);
    w->lbl_finish_sub = octo_w_label(w->finish_cell, "--", &lv_font_montserrat_12,
                                     &octo_style_label);
    lv_obj_set_style_margin_top(w->lbl_finish_sub, 6, 0);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void bento_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, BENTO_GAP, 0);

    build_header(page, w);
    build_heroes(page, w);
    build_temps(page, w);
    build_stats(page, w);
}

const octoprint_layout_ops_t octoprint_layout_bento = {
    .name  = "Bento",
    .build = bento_build,
};
