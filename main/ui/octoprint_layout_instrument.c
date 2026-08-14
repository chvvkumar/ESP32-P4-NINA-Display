/**
 * @file octoprint_layout_instrument.c
 * @brief OctoPrint layout 1 — "Instrument cluster" (mockup opv3).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h, every colour from an octo_color() theme token or a
 * shared style, so all nine themes work without a change here. This file never
 * reads octoprint_data_t and never sets a value from live data.
 *
 * Structure (688 x 688 content area, 12 px gutters):
 *   header  52   state chip + JOB FILE
 *   row 1  350   progress dial (tick belt + completion arc + layer belt) | image scope
 *   row 2  186   nozzle gauge | bed gauge | system panel (link + fault strip)
 *   footer  64   elapsed | remaining | finish at | firmware M73
 *
 * The dial's fine tick belt is an lv_scale in ROUND_INNER mode (LVGL's own
 * radial tick renderer) rather than ~100 hand-placed rotated ticks. The inner
 * layer belt is OCTO_LAYER_SEGS dots placed on the same 135°..405° sweep, so
 * the core update path lights them through w->layer_segs[] exactly as it does
 * for the bento strip.
 *
 * Deviations from the mockup:
 *   1. No webcam picture-in-picture and no THUMB|CAM chip. The scope shows one
 *      image; which source it is comes from config and is named by the tag.
 *   2. Temperatures use the shared fill-to-target element (a linear lv_bar the
 *      update path drives with lv_bar_set_value), stood vertically, not the
 *      mockup's semicircle. Swapping in an lv_arc would break that handle.
 *   3. The mockup repeats STATE and LINK in the system panel; each handle
 *      exists once, so STATE lives in the header chip and LINK in the panel.
 *
 * Decorative accents are coloured at build time from octo_color() (chip border,
 * reticle, crosshair). That is safe because octoprint_page_apply_theme()
 * rebuilds the whole content tree rather than re-colouring known handles, so a
 * theme change re-runs build() and re-derives every one of them.
 */

#include "nina_octoprint_internal.h"

#include <math.h>

/* ── Geometry ─────────────────────────────────────────────────────────── */

#define INS_GAP          12
#define INS_HDR_H        52
#define INS_R1_H        350
#define INS_R2_H        186
#define INS_FOOT_H       64
#define INS_PANEL_R      10   /* instrument panels are squarer than the bento 24 */
#define INS_PAD          12

#define INS_GAUGE_W     348
#define INS_DIAL        300
#define INS_ARC_OUTER   258
#define INS_ARC_INNER   226
#define INS_LAYER_R      86   /* radius of the layer dot belt, from dial centre */
#define INS_LAYER_DOT     8

#define INS_SWEEP_START 135.0f
#define INS_SWEEP_DEG   270.0f
#define INS_DEG2RAD     0.017453292f

/* ── Small local helpers ──────────────────────────────────────────────── */

/** Bare unstyled container that positions its children absolutely. */
static lv_obj_t *ins_plain(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    return o;
}

/** Instrument panel: shared card style, squarer corners, uniform padding. */
static lv_obj_t *ins_panel(lv_obj_t *parent)
{
    lv_obj_t *p = octo_w_card(parent);
    lv_obj_set_style_radius(p, INS_PANEL_R, 0);
    lv_obj_set_style_pad_all(p, INS_PAD, 0);
    return p;
}

/** Caption strip at the top of a panel: label left, dim identity right. */
static lv_obj_t *ins_cap_row(lv_obj_t *panel, const char *left, const char *right)
{
    lv_obj_t *row = octo_w_row(panel, true, 0);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    octo_w_label(row, left, &lv_font_montserrat_12, &octo_style_label);
    if (right) {
        lv_obj_t *r = octo_w_label(row, right, &lv_font_montserrat_12,
                                   &octo_style_label);
        lv_obj_set_style_text_opa(r, LV_OPA_50, 0);
    }
    return row;
}

/** One L-shaped reticle mark in a corner of the scope frame. */
static void ins_reticle(lv_obj_t *parent, lv_align_t align, lv_border_side_t sides,
                        int dx, int dy)
{
    lv_obj_t *c = ins_plain(parent, 16, 16);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_side(c, sides, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(octo_color(OCTO_COL_ACCENT)), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_70, 0);
    lv_obj_align(c, align, dx, dy);
}

/* ── Header ───────────────────────────────────────────────────────────── */

static void build_header(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *hdr = ins_panel(page);
    lv_obj_set_size(hdr, LV_PCT(100), INS_HDR_H);
    lv_obj_set_style_pad_ver(hdr, 0, 0);
    lv_obj_set_style_pad_column(hdr, 12, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    octo_w_header_wash(hdr);

    /* State chip: the shared state line inside an outlined instrument bezel. */
    lv_obj_t *chip = octo_w_state_line(hdr, w);
    lv_obj_set_style_pad_hor(chip, 10, 0);
    lv_obj_set_style_pad_ver(chip, 5, 0);
    lv_obj_set_style_radius(chip, 4, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(octo_color(OCTO_COL_ACCENT)), 0);
    lv_obj_set_style_border_opa(chip, LV_OPA_50, 0);

    /* Job file: micro caption over the ellipsised name. */
    lv_obj_t *file = octo_w_row(hdr, false, 1);
    lv_obj_set_height(file, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(file, 1);
    octo_w_label(file, "JOB FILE", &lv_font_montserrat_12, &octo_style_label);
    octo_w_file_label(file, w);
}

/* ── Progress dial ────────────────────────────────────────────────────── */

/** Fine index belt around the rim: 51 ticks, every 5th major, no labels. */
static void build_tick_belt(lv_obj_t *dial)
{
    lv_obj_t *sc = lv_scale_create(dial);
    lv_obj_remove_style_all(sc);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sc, INS_DIAL, INS_DIAL);
    lv_obj_center(sc);

    lv_scale_set_mode(sc, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(sc, 0, 100);
    lv_scale_set_total_tick_count(sc, 51);
    lv_scale_set_major_tick_every(sc, 5);
    lv_scale_set_label_show(sc, false);
    lv_scale_set_angle_range(sc, (uint32_t)INS_SWEEP_DEG);
    lv_scale_set_rotation(sc, (int32_t)INS_SWEEP_START);

    /* The rim line itself stays invisible; only the ticks read as an index. */
    lv_obj_set_style_arc_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_set_style_length(sc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(sc, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(sc, lv_color_hex(octo_color(OCTO_COL_LABEL)),
                                LV_PART_INDICATOR);

    lv_obj_set_style_length(sc, 8, LV_PART_ITEMS);
    lv_obj_set_style_line_width(sc, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(sc, lv_color_hex(octo_color(OCTO_COL_BORDER)),
                                LV_PART_ITEMS);
}

/** Static 0 / 50 / 100 index legend at the ends and apex of the sweep. */
static void build_dial_legend(lv_obj_t *dial)
{
    struct { const char *txt; int dx; int dy; } legend[3] = {
        { "0",   -104,  104 },
        { "50",     0, -128 },
        { "100",  104,  104 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = octo_w_label(dial, legend[i].txt, &lv_font_montserrat_12,
                                   &octo_style_label);
        lv_obj_set_style_text_opa(l, LV_OPA_60, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, legend[i].dx, legend[i].dy);
    }
}

/**
 * Layer belt + layer readout, both parented to layer_cell so the update path
 * hides the whole DisplayLayerProgress story with one flag when the plugin is
 * not installed.
 */
static void build_layer_cell(lv_obj_t *dial, octoprint_widgets_t *w)
{
    w->layer_cell = ins_plain(dial, INS_DIAL, INS_DIAL);
    lv_obj_center(w->layer_cell);

    for (int i = 0; i < OCTO_LAYER_SEGS; i++) {
        float a = (INS_SWEEP_START
                   + INS_SWEEP_DEG * (float)i / (float)(OCTO_LAYER_SEGS - 1))
                  * INS_DEG2RAD;
        lv_obj_t *seg = ins_plain(w->layer_cell, INS_LAYER_DOT, INS_LAYER_DOT);
        lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
        lv_obj_align(seg, LV_ALIGN_CENTER,
                     (int32_t)lroundf((float)INS_LAYER_R * cosf(a)),
                     (int32_t)lroundf((float)INS_LAYER_R * sinf(a)));
        w->layer_segs[i] = seg;
    }

    lv_obj_t *stack = octo_w_row(w->layer_cell, false, 2);
    lv_obj_set_size(stack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(stack, LV_ALIGN_CENTER, 0, 54);

    octo_w_label(stack, "LAYER", &lv_font_montserrat_12, &octo_style_label);

    lv_obj_t *row = octo_w_row(stack, true, 5);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(row, "--", &lv_font_montserrat_20,
                                    &octo_style_accent);
    w->lbl_layer_total = octo_w_label(row, "/ --", &lv_font_montserrat_12,
                                      &octo_style_label);
}

/** Digital readout on the dial face: COMPLETE over the big percentage. */
static void build_dial_readout(lv_obj_t *dial, octoprint_widgets_t *w)
{
    lv_obj_t *stack = octo_w_row(dial, false, 0);
    lv_obj_set_size(stack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(stack, LV_ALIGN_CENTER, 0, -30);

    w->lbl_pct_sub = octo_w_label(stack, "COMPLETE", &lv_font_montserrat_12,
                                  &octo_style_label);

    lv_obj_t *row = octo_w_row(stack, true, 3);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_margin_top(row, 2, 0);
    w->lbl_pct = octo_w_label(row, "--", &lv_font_montserrat_48, &octo_style_value);
    w->lbl_pct_unit = octo_w_label(row, "%", &lv_font_montserrat_18,
                                   &octo_style_accent);
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, 8, 0);
}

static void build_gauge(lv_obj_t *row, octoprint_widgets_t *w)
{
    lv_obj_t *panel = ins_panel(row);
    lv_obj_set_size(panel, INS_GAUGE_W, LV_PCT(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    ins_cap_row(panel, "JOB PROGRESS", "IDX 270\xC2\xB0");

    /* Free-positioning host so every ring shares one centre. */
    lv_obj_t *dial = ins_plain(panel, INS_DIAL, INS_DIAL);
    lv_obj_set_style_margin_top(dial, 4, 0);

    build_tick_belt(dial);
    build_dial_legend(dial);

    lv_obj_t *outer = octo_w_progress_arc(dial, INS_ARC_OUTER, 16, false, w);
    lv_obj_center(outer);

    lv_obj_t *inner = octo_w_progress_arc(dial, INS_ARC_INNER, 4, true, w);
    lv_obj_center(inner);

    build_layer_cell(dial, w);
    build_dial_readout(dial, w);
}

/* ── Image scope ──────────────────────────────────────────────────────── */

static void build_scope(lv_obj_t *row, octoprint_widgets_t *w)
{
    lv_obj_t *panel = ins_panel(row);
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Single image area: the source is a config choice, not a page control. */
    ins_cap_row(panel, "IMAGE", "SCOPE");

    lv_obj_t *frame = octo_w_image_hero(panel, w);
    lv_obj_set_width(frame, LV_PCT(100));
    lv_obj_set_flex_grow(frame, 1);
    lv_obj_set_style_margin_top(frame, 6, 0);
    lv_obj_set_style_radius(frame, 3, 0);

    /* Reticle: centre crosshair plus four corner marks, drawn over the frame. */
    lv_obj_t *cross_h = ins_plain(frame, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(cross_h, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(cross_h, lv_color_hex(octo_color(OCTO_COL_ACCENT)), 0);
    lv_obj_center(cross_h);

    lv_obj_t *cross_v = ins_plain(frame, 1, LV_PCT(100));
    lv_obj_set_style_bg_opa(cross_v, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(cross_v, lv_color_hex(octo_color(OCTO_COL_ACCENT)), 0);
    lv_obj_center(cross_v);

    ins_reticle(frame, LV_ALIGN_TOP_LEFT,
                LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 6, 6);
    ins_reticle(frame, LV_ALIGN_TOP_RIGHT,
                LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT, -6, 6);
    ins_reticle(frame, LV_ALIGN_BOTTOM_LEFT,
                LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT, 6, -6);
    ins_reticle(frame, LV_ALIGN_BOTTOM_RIGHT,
                LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT, -6, -6);

    /* Tag sits bottom-left over the reticle, as in the mockup. */
    if (w->lbl_img_tag) {
        lv_obj_align(w->lbl_img_tag, LV_ALIGN_BOTTOM_LEFT, 10, -10);
        lv_obj_move_foreground(w->lbl_img_tag);
    }
    if (w->img_placeholder) {
        lv_obj_move_foreground(w->img_placeholder);
    }
}

/* ── Temperature instruments ──────────────────────────────────────────── */

static void build_temp_panel(lv_obj_t *row, const char *name, bool hot,
                             octo_temp_el_t *out)
{
    lv_obj_t *panel = ins_panel(row);
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *el = octo_w_temp(panel, name, true, hot, out);
    lv_obj_set_width(el, LV_PCT(100));
    lv_obj_set_flex_grow(el, 1);
    lv_obj_set_style_pad_row(el, 6, 0);

    /* Column form: the shared element stacks name / value / bar / target. The
     * bar is widened into a gauge column and the value trimmed one step so a
     * "214.9 / 215 C" reading cannot overflow a third of the screen. */
    if (out->bar) {
        lv_obj_set_width(out->bar, 40);
    }
    if (out->lbl_value) {
        lv_obj_set_style_text_font(out->lbl_value, &lv_font_montserrat_20, 0);
    }
}

/* ── System panel ─────────────────────────────────────────────────────── */

static void build_system(lv_obj_t *row, octoprint_widgets_t *w)
{
    lv_obj_t *panel = ins_panel(row);
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    ins_cap_row(panel, "SYSTEM", "OCTOPRINT");

    /* Link block grows to take the slack so the fault strip stays pinned low. */
    lv_obj_t *body = octo_w_row(panel, false, 8);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    octo_w_label(body, "LINK", &lv_font_montserrat_12, &octo_style_label);
    octo_w_conn_chip(body, w);

    octo_w_status_strip(panel, w);
    if (w->error_strip) {
        lv_obj_set_width(w->error_strip, LV_PCT(100));
        lv_obj_set_style_radius(w->error_strip, 4, 0);
    }
}

/* ── Footer readouts ──────────────────────────────────────────────────── */

/** One footer cell: caption over a value, returned so it can be a hide target. */
static lv_obj_t *build_foot_cell(lv_obj_t *foot, const char *caption,
                                 lv_obj_t **out_value)
{
    lv_obj_t *cell = ins_panel(foot);
    lv_obj_set_height(cell, LV_PCT(100));
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_style_pad_ver(cell, 8, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *tile = octo_w_time_tile(cell, caption, &lv_font_montserrat_22,
                                      out_value);
    lv_obj_set_width(tile, LV_PCT(100));
    return cell;
}

static void build_footer(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *foot = octo_w_row(page, true, INS_GAP);
    lv_obj_set_size(foot, LV_PCT(100), INS_FOOT_H);

    build_foot_cell(foot, "ELAPSED", &w->lbl_elapsed);
    build_foot_cell(foot, "REMAINING", &w->lbl_remaining);

    /* Both DisplayLayerProgress-sourced cells are hide targets for the core. */
    w->finish_cell = build_foot_cell(foot, "FINISH AT", &w->lbl_finish);
    w->m73_row     = build_foot_cell(foot, "FIRMWARE", &w->lbl_m73);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void instrument_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, INS_GAP, 0);

    build_header(page, w);

    lv_obj_t *r1 = octo_w_row(page, true, INS_GAP);
    lv_obj_set_size(r1, LV_PCT(100), INS_R1_H);
    build_gauge(r1, w);
    build_scope(r1, w);

    lv_obj_t *r2 = octo_w_row(page, true, INS_GAP);
    lv_obj_set_size(r2, LV_PCT(100), INS_R2_H);
    build_temp_panel(r2, "NOZZLE", true, &w->nozzle);
    build_temp_panel(r2, "BED", false, &w->bed);
    build_system(r2, w);

    build_footer(page, w);
}

const octoprint_layout_ops_t octoprint_layout_instrument = {
    .name  = "Instrument",
    .build = instrument_build,
};
