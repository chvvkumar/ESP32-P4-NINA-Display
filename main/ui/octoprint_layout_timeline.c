/**
 * @file octoprint_layout_timeline.c
 * @brief OctoPrint layout 4 — "Layer timeline" (mockup opv6, revision 2).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour from octo_color()/octo_style_*, so
 * all nine themes work without a change here. This file never reads
 * octoprint_data_t.
 *
 * The page narrates the print as a physical build: a tall vertical column that
 * fills bottom-up exactly like the part grows, ruled into 20 bands so the
 * lit/unlit boundary reads as "now". The image hero is a bare bordered frame
 * beside it; a single row of equal tiles docks along the bottom edge.
 *
 * Structure (688 x 688 content area):
 *   header  40  wordmark | state dot + state (no fill, no file name)
 *   ribbon  24  connection + fault text, flat, fixed geometry (no reflow)
 *   body   ---  layer column 206 | image hero (grows), 12 px breathing room
 *   dock    90  ELAPSED | REMAINING | FINISH AT | NOZZLE | BED
 *
 * No percentage is shown anywhere: the column IS the progress read-out.
 *
 * The whole left column is w->layer_cell, so when DisplayLayerProgress is absent
 * the update path hides it wholesale and the image hero grows into the freed
 * width (LVGL flex skips hidden children), exactly as before.
 *
 * ponytail: the column fill is w->bar_progress (job completion), not the 12
 * w->layer_segs bricks — 12 handles cannot express the mockup's 20 bands, and
 * the mockup itself draws one continuous fill under 19 separators. If a
 * layer-exact fill is ever wanted, widen OCTO_LAYER_SEGS to 20 and swap the bar
 * for the segment loop; the geometry below does not change.
 */

#include "nina_octoprint_internal.h"

#define TL_HDR_H      40
#define TL_RIBBON_H   24
#define TL_DOCK_H     90
#define TL_GAP        10
#define TL_BODY_PAD   12
#define TL_COL_W      206
#define TL_LAYNUM_H   36
#define TL_STACK_W    120
#define TL_BANDS      20   /* 20 bands => 19 separator lines, one every 5 % */

/* ── Header ───────────────────────────────────────────────────────────── */

/** 1 px rule under a flat (unfilled) strip, in the card border colour. */
static void rule_under(lv_obj_t *obj)
{
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
}

static void build_header(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *hdr = octo_w_row(page, true, TL_GAP);
    lv_obj_set_size(hdr, LV_PCT(100), TL_HDR_H);
    lv_obj_set_style_pad_hor(hdr, 4, 0);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    rule_under(hdr);

    /* Wordmark only — the job file name is deliberately not on this page. */
    octo_w_caption(hdr, "OCTOPRINT");

    /* Dot + state, right-justified by the SPACE_BETWEEN above. */
    octo_w_state_line(hdr, w);
}

/* ── Ribbon: connection + fault, flat text ────────────────────────────── */

/** Strip a chip back to bare dot + text so the ribbon reads as one line. */
static void flatten_chip(lv_obj_t *chip)
{
    if (!chip) {
        return;
    }
    lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_hor(chip, 0, 0);
    lv_obj_set_style_pad_ver(chip, 0, 0);
    lv_obj_set_style_radius(chip, 0, 0);
}

static void build_ribbon(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *ribbon = octo_w_row(page, true, 14);
    lv_obj_set_size(ribbon, LV_PCT(100), TL_RIBBON_H);
    lv_obj_set_style_pad_hor(ribbon, 4, 0);
    lv_obj_set_flex_align(ribbon, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ribbon, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(ribbon, lv_color_hex(octo_color(OCTO_COL_CARDBG)), 0);
    rule_under(ribbon);

    octo_w_conn_chip(ribbon, w);
    flatten_chip(w->conn_chip);

    /* Fault text stretches across the rest of the ribbon so a fault appears in
     * place, in already-reserved geometry, without reflowing anything. */
    octo_w_status_strip(ribbon, w);
    flatten_chip(w->error_strip);
    lv_obj_set_flex_grow(w->error_strip, 1);
}

/* ── Layer column ─────────────────────────────────────────────────────── */

/**
 * The build column: a vertical bar that fills bottom-up (LVGL grows a vertical
 * bar's indicator from the bottom edge), ruled into TL_BANDS bands by thin
 * children positioned in percent so they land before the first layout pass.
 */
static void build_stack(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *bar = octo_w_progress_bar(parent, w);
    lv_bar_set_orientation(bar, LV_BAR_ORIENTATION_VERTICAL);
    lv_obj_set_width(bar, TL_STACK_W);
    lv_obj_set_height(bar, LV_PCT(100));
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_clip_corner(bar, true, 0);

    for (int i = 1; i < TL_BANDS; i++) {
        lv_obj_t *line = lv_obj_create(bar);
        lv_obj_remove_style_all(line);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_set_y(line, LV_PCT((i * 100) / TL_BANDS));
        lv_obj_set_style_bg_opa(line, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
    }
}

static void build_layer_column(lv_obj_t *parent, octoprint_widgets_t *w)
{
    /* The entire column is the layer cell: with no DisplayLayerProgress there is
     * no data for any of it, the update path hides it, and the image hero grows
     * into the freed width. */
    w->layer_cell = octo_w_row(parent, false, 10);
    lv_obj_set_size(w->layer_cell, TL_COL_W, LV_PCT(100));
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Layer count, at the top of the column. */
    lv_obj_t *num = octo_w_row(w->layer_cell, true, 6);
    lv_obj_set_size(num, LV_PCT(100), TL_LAYNUM_H);
    lv_obj_set_flex_align(num, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    octo_w_caption(num, "LAYER");

    lv_obj_t *cnt = octo_w_row(num, true, 4);
    lv_obj_set_size(cnt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(cnt, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(cnt, "--", &lv_font_montserrat_26,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(cnt, "/ --", &lv_font_montserrat_16,
                                      &octo_style_label);

    /* The column itself takes every pixel left between count and captions. */
    lv_obj_t *wrap = octo_w_row(w->layer_cell, true, 0);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_flex_grow(wrap, 1);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    build_stack(wrap, w);

    /* Bottom of the build: where the print started, on the bed. Static text —
     * the update path feeds no start-time handle, and the finish time now lives
     * in the dock, not on this axis. */
    lv_obj_t *foot = octo_w_row(w->layer_cell, true, 6);
    lv_obj_set_size(foot, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    octo_w_caption(foot, "START");
    octo_w_caption(foot, "BED");
}

/* ── Body: layer column + image hero ──────────────────────────────────── */

static void build_body(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, TL_GAP);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_pad_ver(row, TL_BODY_PAD, 0);

    build_layer_column(row, w);

    /* Bare bordered frame, no tag, no caption — it fills everything left. */
    lv_obj_t *img = octo_w_image_hero(row, w);
    lv_obj_set_height(img, LV_PCT(100));
    lv_obj_set_flex_grow(img, 1);
    lv_obj_set_style_radius(img, 10, 0);
}

/* ── Dock: one full-width row of five equal tiles ─────────────────────── */

/** Dock tile shell: equal share of the row, caption over value, left aligned. */
static lv_obj_t *dock_tile(lv_obj_t *dock)
{
    lv_obj_t *card = octo_w_card(dock);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_hor(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return card;
}

/** Temperature tile: TILE variant sized so "250.2/250 °C" cannot wrap. */
static void dock_temp(lv_obj_t *dock, const char *name, bool hot,
                      octo_temp_el_t *out, octo_color_id_t tint)
{
    lv_obj_t *card = dock_tile(dock);
    lv_obj_t *el = octo_w_temp(card, name, OCTO_TEMP_TILE, hot, out);
    lv_obj_set_size(el, LV_PCT(100), LV_SIZE_CONTENT);
    if (out->lbl_value) {
        lv_obj_set_style_text_font(out->lbl_value, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(out->lbl_value, lv_color_hex(octo_color(tint)), 0);
    }
}

static void build_dock(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *dock = octo_w_row(page, true, TL_GAP);
    lv_obj_set_size(dock, LV_PCT(100), TL_DOCK_H);

    lv_obj_t *el = dock_tile(dock);
    octo_w_caption(el, "ELAPSED");
    w->lbl_elapsed = octo_w_label(el, "--", &lv_font_montserrat_26,
                                  &octo_style_value);

    lv_obj_t *rem = dock_tile(dock);
    octo_w_caption(rem, "REMAINING");
    w->lbl_remaining = octo_w_label(rem, "--", &lv_font_montserrat_26,
                                    &octo_style_accent);

    /* Finish time is DLP-sourced: the whole tile is the cell the update path
     * hides, so the other four simply widen when the plugin is absent. */
    w->finish_cell = dock_tile(dock);
    octo_w_caption(w->finish_cell, "FINISH AT");
    w->lbl_finish = octo_w_label(w->finish_cell, "--", &lv_font_montserrat_26,
                                 &octo_style_value);

    dock_temp(dock, "NOZZLE", true, &w->nozzle, OCTO_COL_HOT);
    dock_temp(dock, "BED", false, &w->bed, OCTO_COL_TEXT);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void timeline_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 0, 0);

    build_header(page, w);
    build_ribbon(page, w);
    build_body(page, w);
    build_dock(page, w);
}

const octoprint_layout_ops_t octoprint_layout_timeline = {
    .name  = "Timeline",
    .build = timeline_build,
};
