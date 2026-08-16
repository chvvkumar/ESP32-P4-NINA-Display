/**
 * @file octoprint_layout_bento.c
 * @brief OctoPrint layout 0 — "Grid dashboard" (reference layout, mockup v1 rev 2).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from octo_color(), so all
 * nine themes work without a change here. This file never reads octoprint_data_t.
 *
 * Structure (688 x 688 content area, 12 px gaps):
 *   header  36   slim UNPLATED strip: connection state | fault slot + state line
 *   heroes 390   completion arc (pct + COMPLETE + layer count on the face)
 *                | bare bordered image hero
 *   temps  140   one full-width cell, two halves: value line + heat bar below
 *   times  rest  three equal tiles — ELAPSED / REMAINING / FINISH AT
 *
 * Deliberate omissions vs the mockup, all covered in the notes: no job file name
 * anywhere (unreadable across a room, never actionable), no printer/nozzle/bed
 * glyphs (no icon font is wired into this page), no picture-in-picture webcam.
 */

#include "nina_octoprint_internal.h"

#define BENTO_GAP       12
#define BENTO_HDR_H     36
#define BENTO_HERO_H    390
#define BENTO_HERO_W    338
#define BENTO_TEMPS_H   140
#define BENTO_ARC_SIZE  296
#define BENTO_ARC_W     26
#define BENTO_LAYER_W   190

/* ── Header: slim unplated strip ───────────────────────────────────────── */

static void build_header(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *hdr = octo_w_row(page, true, 10);
    lv_obj_set_size(hdr, LV_PCT(100), BENTO_HDR_H);
    lv_obj_set_style_pad_hor(hdr, 8, 0);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Connection state, left. Plain text, not a chip: this strip carries no
     * plate and exactly one accent element (the state line, far right). */
    w->lbl_conn = octo_w_label(hdr, "--", &lv_font_montserrat_12, &octo_style_label);

    lv_obj_t *spacer = octo_w_row(hdr, true, 0);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    /* Fault slot: the shared strip, stripped back to bare type because this
     * header carries no plate. It is born empty and hidden; text, colour and
     * visibility all come from the update path. */
    octo_w_status_strip(hdr, w);
    if (w->error_strip) {
        lv_obj_set_style_bg_opa(w->error_strip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(w->error_strip, 0, 0);
        lv_obj_set_style_radius(w->error_strip, 0, 0);
        lv_obj_set_style_pad_hor(w->error_strip, 0, 0);
        lv_obj_set_style_pad_ver(w->error_strip, 0, 0);
        lv_obj_set_style_margin_right(w->error_strip, 6, 0);
    }
    /* No dot: the state line stays the strip's one accent element. */
    if (w->error_dot) {
        lv_obj_add_flag(w->error_dot, LV_OBJ_FLAG_HIDDEN);
    }

    octo_w_state_line(hdr, w);
}

/* ── Progress hero: one arc, everything on its face ───────────────────── */

static void build_arc_face(lv_obj_t *arc, octoprint_widgets_t *w)
{
    lv_obj_t *ctr = octo_w_row(arc, false, 0);
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
    w->lbl_pct_unit = octo_w_label(pct_row, "%", &lv_font_montserrat_28,
                                   &octo_style_label);

    w->lbl_pct_sub = octo_w_caption(ctr, "COMPLETE");
    lv_obj_set_style_margin_top(w->lbl_pct_sub, 6, 0);

    /* Layer count under a hairline rule — visible without spending a bento
     * cell on it. Hidden wholesale (rule included) when DLP is unavailable. */
    w->layer_cell = octo_w_row(ctr, false, 0);
    lv_obj_set_size(w->layer_cell, BENTO_LAYER_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(w->layer_cell, 16, 0);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *rule = lv_obj_create(w->layer_cell);
    lv_obj_remove_style_all(rule);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);

    lv_obj_t *lay = octo_w_row(w->layer_cell, true, 7);
    lv_obj_set_size(lay, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(lay, 12, 0);
    lv_obj_set_flex_align(lay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    octo_w_caption(lay, "LAYER");
    w->lbl_layer_cur = octo_w_label(lay, "--", &lv_font_montserrat_22,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(lay, "/ --", &lv_font_montserrat_16,
                                      &octo_style_label);
}

static void build_progress_hero(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *card = octo_w_card(parent);
    lv_obj_set_size(card, BENTO_HERO_W, BENTO_HERO_H);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(card, 16, 0);
    lv_obj_set_style_pad_hor(card, 20, 0);

    lv_obj_t *cap = octo_w_caption(card, "PROGRESS");
    lv_obj_set_width(cap, LV_PCT(100));

    lv_obj_t *arc = octo_w_progress_arc(card, BENTO_ARC_SIZE, BENTO_ARC_W, w);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_top(arc, 14, 0);

    /* Added after the arc so the face draws on top of it. */
    build_arc_face(arc, w);
}

/* ── Heroes row ───────────────────────────────────────────────────────── */

static void build_heroes(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, BENTO_GAP);
    lv_obj_set_size(row, LV_PCT(100), BENTO_HERO_H);

    build_progress_hero(row, w);

    /* Bare frame: the image and the widget border, nothing else. */
    lv_obj_t *img = octo_w_image_hero(row, w);
    lv_obj_set_size(img, BENTO_HERO_W, BENTO_HERO_H);
}

/* ── Temperatures: one cell, two halves ───────────────────────────────── */

static void build_temps(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *card = octo_w_card(page);
    lv_obj_set_size(card, LV_PCT(100), BENTO_TEMPS_H);
    lv_obj_set_style_pad_ver(card, 16, 0);
    lv_obj_set_style_pad_hor(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(card, 16, 0);

    for (int i = 0; i < 2; i++) {
        bool hot = (i == 0);
        octo_temp_el_t *t = hot ? &w->nozzle : &w->bed;

        lv_obj_t *half = octo_w_row(card, false, 0);
        lv_obj_set_height(half, LV_PCT(100));
        lv_obj_set_flex_grow(half, 1);
        lv_obj_set_flex_align(half, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        lv_obj_t *el = octo_w_temp(half, hot ? "NOZZLE" : "BED",
                                   OCTO_TEMP_BAR_GRADIENT, hot, t);
        lv_obj_set_width(el, LV_PCT(100));
        lv_obj_set_height(el, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_row(el, 12, 0);

        /* Value line big enough to read across a room, sized so the worst case
         * "250.3 / 250 °C" still fits one line in half a cell. */
        if (t->lbl_value) {
            lv_obj_set_style_text_font(t->lbl_value, &lv_font_montserrat_26, 0);
        }
        if (t->bar) {
            lv_obj_set_height(t->bar, 16);
            lv_obj_set_style_radius(t->bar, 8, 0);
            lv_obj_set_style_radius(t->bar, 8, LV_PART_INDICATOR);
        }
    }
}

/* ── Time row: three equal tiles ──────────────────────────────────────── */

static lv_obj_t *build_time_tile(lv_obj_t *row, const char *caption,
                                 lv_obj_t **out_value)
{
    lv_obj_t *cell = octo_w_card(row);
    lv_obj_set_height(cell, LV_PCT(100));
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_style_pad_hor(cell, 18, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *tile = octo_w_time_tile(cell, caption, &lv_font_montserrat_36,
                                      out_value);
    lv_obj_set_style_pad_row(tile, 8, 0);
    return cell;
}

static void build_times(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, BENTO_GAP);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);

    build_time_tile(row, "ELAPSED", &w->lbl_elapsed);
    build_time_tile(row, "REMAINING", &w->lbl_remaining);

    /* Finish time is DLP-sourced, so the whole tile hides when DLP is absent
     * and the other two grow to fill the row. */
    w->finish_cell = build_time_tile(row, "FINISH AT", &w->lbl_finish);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void bento_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, BENTO_GAP, 0);

    build_header(page, w);
    build_heroes(page, w);
    build_temps(page, w);
    build_times(page, w);
}

const octoprint_layout_ops_t octoprint_layout_bento = {
    .name  = "Grid",
    .build = bento_build,
};
