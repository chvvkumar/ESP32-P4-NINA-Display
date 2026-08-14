/**
 * @file octoprint_layout_timeline.c
 * @brief OctoPrint layout 4 — "Layer timeline" (mockup v6).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour from octo_color()/octo_style_*, so
 * all nine themes work without a change here. This file never reads
 * octoprint_data_t.
 *
 * The page narrates the print as a physical build: the progress hero is a tall
 * vertical column of stacked layer bricks that fills bottom-up exactly like the
 * part grows, with the time axis read along the same edge — finish time at the
 * top, start at the bottom, and the lit/unlit boundary of the column standing in
 * for "now". The image hero sits beside it; the metrics dock runs along the
 * bottom edge.
 *
 * Structure (688 x 688 content area, 10 px gaps):
 *   header 58   file name + job caption | state line
 *   ribbon 28   connection chip + fault strip (fixed geometry, no reflow)
 *   body   350  layer timeline column 214 | image hero (grows)
 *   dock   222  temperature tracks 96 | elapsed / remaining / complete 116
 *
 * The whole left column is w->layer_cell, so when DisplayLayerProgress is absent
 * the update path hides it wholesale, the image hero grows into the freed width,
 * and the page still reads: progress is carried by lbl_pct + bar_progress in the
 * dock, which are not DLP-sourced.
 *
 * Deviation from the mockup: no G-code/Cam source toggle. The image area is
 * single-source (config choice); its tag label says which.
 */

#include "nina_octoprint_internal.h"

#define TL_GAP        10
#define TL_HDR_H      58
#define TL_RIBBON_H   28
#define TL_BODY_H     350
#define TL_COL_W      214
#define TL_LAYNUM_H   32
#define TL_GUTTER_W   100
#define TL_TEMPS_H    96
#define TL_STATS_H    116

/* ── Header ───────────────────────────────────────────────────────────── */

static void build_header(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *hdr = octo_w_card(page);
    lv_obj_set_size(hdr, LV_PCT(100), TL_HDR_H);
    lv_obj_set_style_radius(hdr, 10, 0);
    lv_obj_set_style_pad_hor(hdr, 16, 0);
    lv_obj_set_style_pad_column(hdr, 14, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    octo_w_header_wash(hdr);

    lv_obj_t *left = octo_w_row(hdr, false, 2);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    octo_w_file_label(left, w);
    octo_w_caption(left, "JOB FILE");

    lv_obj_t *state = octo_w_state_line(hdr, w);
    lv_obj_set_style_pad_hor(state, 12, 0);
    lv_obj_set_style_pad_ver(state, 5, 0);
    lv_obj_set_style_radius(state, 14, 0);
    lv_obj_set_style_bg_opa(state, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(state, lv_color_hex(octo_color(OCTO_COL_ACCENT)), 0);
}

/* ── Ribbon: connection + fault slot ──────────────────────────────────── */

static void build_ribbon(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *ribbon = octo_w_row(page, true, TL_GAP);
    lv_obj_set_size(ribbon, LV_PCT(100), TL_RIBBON_H);
    lv_obj_set_flex_align(ribbon, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    octo_w_conn_chip(ribbon, w);

    /* Fault strip stretches across the rest of the ribbon so a fault appears in
     * place, in already-reserved geometry, without reflowing anything. */
    octo_w_status_strip(ribbon, w);
    lv_obj_set_flex_grow(w->error_strip, 1);
}

/* ── Layer timeline column ────────────────────────────────────────────── */

static void build_gutter(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *gutter = octo_w_row(parent, false, 0);
    lv_obj_set_size(gutter, TL_GUTTER_W, LV_PCT(100));
    lv_obj_set_flex_align(gutter, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    /* Top of the axis = finish time. DLP-sourced, hidden by the update path
     * when the plugin is absent. */
    w->finish_cell = octo_w_row(gutter, false, 2);
    lv_obj_set_size(w->finish_cell, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->finish_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    w->lbl_finish = octo_w_label(w->finish_cell, "--", &lv_font_montserrat_28,
                                 &octo_style_value);
    lv_obj_t *fin_cap = octo_w_caption(w->finish_cell, "FINISH");
    lv_obj_set_style_text_align(fin_cap, LV_TEXT_ALIGN_RIGHT, 0);

    w->lbl_finish_sub = octo_w_label(w->finish_cell, "--", &lv_font_montserrat_12,
                                     &octo_style_label);
    lv_obj_set_width(w->lbl_finish_sub, LV_PCT(100));
    lv_label_set_long_mode(w->lbl_finish_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(w->lbl_finish_sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_margin_top(w->lbl_finish_sub, 4, 0);

    /* Bottom of the axis = where the build started. Static: the update path
     * feeds no start-time handle, so nothing is computed here. */
    lv_obj_t *start = octo_w_row(gutter, false, 2);
    lv_obj_set_size(start, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(start, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_t *bed = octo_w_label(start, "BED", &lv_font_montserrat_16,
                                 &octo_style_label);
    lv_obj_set_style_text_align(bed, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *start_cap = octo_w_caption(start, "STARTED");
    lv_obj_set_style_text_align(start_cap, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_stack(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *stack = octo_w_card(parent);
    lv_obj_set_height(stack, LV_PCT(100));
    lv_obj_set_flex_grow(stack, 1);
    lv_obj_set_style_radius(stack, 10, 0);
    lv_obj_set_style_pad_all(stack, 5, 0);
    lv_obj_set_style_pad_row(stack, 3, 0);
    /* COLUMN_REVERSE: segment 0 is the bottom brick, so the strip lights
     * bottom-up as the print grows — the update path lights indices 0..n. */
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN_REVERSE);

    for (int i = 0; i < OCTO_LAYER_SEGS; i++) {
        lv_obj_t *seg = lv_obj_create(stack);
        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(seg, LV_PCT(100));
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_radius(seg, 3, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
        w->layer_segs[i] = seg;
    }
}

static void build_timeline_column(lv_obj_t *parent, octoprint_widgets_t *w)
{
    /* The entire column is the layer cell: with no DisplayLayerProgress there is
     * no data for any of it, the update path hides it, and the image hero grows
     * into the freed width. */
    w->layer_cell = octo_w_row(parent, false, 8);
    lv_obj_set_size(w->layer_cell, TL_COL_W, LV_PCT(100));

    lv_obj_t *num = octo_w_row(w->layer_cell, true, 6);
    lv_obj_set_size(num, LV_PCT(100), TL_LAYNUM_H);
    lv_obj_set_flex_align(num, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    octo_w_caption(num, "LAYER");

    lv_obj_t *cnt = octo_w_row(num, true, 5);
    lv_obj_set_size(cnt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(cnt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(cnt, "--", &lv_font_montserrat_28,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(cnt, "/ --", &lv_font_montserrat_16,
                                      &octo_style_label);

    lv_obj_t *wrap = octo_w_row(w->layer_cell, true, 8);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_flex_grow(wrap, 1);

    build_gutter(wrap, w);
    build_stack(wrap, w);
}

/* ── Body: timeline column + image hero ───────────────────────────────── */

static void build_body(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, TL_GAP);
    lv_obj_set_size(row, LV_PCT(100), TL_BODY_H);

    build_timeline_column(row, w);

    lv_obj_t *img = octo_w_image_hero(row, w);
    lv_obj_set_height(img, LV_PCT(100));
    lv_obj_set_flex_grow(img, 1);
    lv_obj_set_style_radius(img, 10, 0);
}

/* ── Dock ─────────────────────────────────────────────────────────────── */

static void build_temps(lv_obj_t *dock, octoprint_widgets_t *w)
{
    lv_obj_t *card = octo_w_card(dock);
    lv_obj_set_size(card, LV_PCT(100), TL_TEMPS_H);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_ver(card, 12, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    lv_obj_t *n = octo_w_temp(card, "NOZZLE", false, true, &w->nozzle);
    lv_obj_set_size(n, LV_PCT(100), 32);

    lv_obj_t *b = octo_w_temp(card, "BED", false, false, &w->bed);
    lv_obj_set_size(b, LV_PCT(100), 32);
}

/** Stats tile shell: card, centred column, caption at the top. */
static lv_obj_t *stat_card(lv_obj_t *parent, int grow)
{
    lv_obj_t *card = octo_w_card(parent);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, grow);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return card;
}

static void build_stats(lv_obj_t *dock, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(dock, true, TL_GAP);
    lv_obj_set_size(row, LV_PCT(100), TL_STATS_H);

    lv_obj_t *el = stat_card(row, 3);
    octo_w_caption(el, "ELAPSED");
    w->lbl_elapsed = octo_w_label(el, "--", &lv_font_montserrat_28,
                                  &octo_style_value);

    lv_obj_t *rem = stat_card(row, 3);
    octo_w_caption(rem, "REMAINING");
    w->lbl_remaining = octo_w_label(rem, "--", &lv_font_montserrat_28,
                                    &octo_style_value);

    /* Progress tile: not DLP-sourced, so this is what keeps the page readable
     * when the layer column is hidden. */
    lv_obj_t *pct = stat_card(row, 4);
    w->lbl_pct_sub = octo_w_caption(pct, "COMPLETE");

    lv_obj_t *val = octo_w_row(pct, true, 3);
    lv_obj_set_size(val, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(val, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_pct = octo_w_label(val, "--", &lv_font_montserrat_36, &octo_style_accent);
    w->lbl_pct_unit = octo_w_label(val, "%", &lv_font_montserrat_20,
                                   &octo_style_label);

    lv_obj_t *bar = octo_w_progress_bar(pct, w);
    lv_obj_set_height(bar, 8);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_margin_top(bar, 3, 0);

    w->m73_row = octo_w_row(pct, true, 0);
    lv_obj_set_size(w->m73_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(w->m73_row, 3, 0);
    w->lbl_m73 = octo_w_label(w->m73_row, "M73 --", &lv_font_montserrat_12,
                              &octo_style_label);
}

static void build_dock(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *dock = octo_w_row(page, false, TL_GAP);
    lv_obj_set_width(dock, LV_PCT(100));
    lv_obj_set_flex_grow(dock, 1);

    build_temps(dock, w);
    build_stats(dock, w);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void timeline_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, TL_GAP, 0);

    build_header(page, w);
    build_ribbon(page, w);
    build_body(page, w);
    build_dock(page, w);
}

const octoprint_layout_ops_t octoprint_layout_timeline = {
    .name  = "Timeline",
    .build = timeline_build,
};
