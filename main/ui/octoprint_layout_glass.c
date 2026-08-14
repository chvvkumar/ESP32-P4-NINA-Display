/**
 * @file octoprint_layout_glass.c
 * @brief OctoPrint layout 2 — "Immersive image" (mockup v4, revision 2).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from an octo_color() token or
 * a shared style, so all nine themes work without a change here. This file never
 * reads octoprint_data_t.
 *
 * The print image IS the page: it runs full-bleed edge to edge with no caption,
 * source chip or corner marks, and everything else floats over it. Nothing sits
 * in a bordered box. Each overlay element paints a HORIZONTAL gradient fill that
 * is partly opaque behind its own text and reaches full transparency just past
 * it, so the picture is obstructed for the width of the words and nothing more:
 * left-anchored elements fade rightward, right-justified ones fade leftward.
 * LVGL carries per-stop opacity in bg_main_opa / bg_grad_opa, so one object with
 * LV_GRAD_DIR_HOR expresses this in two stops — which is all this build has
 * (CONFIG_LV_GRADIENT_MAX_STOPS=2).
 *
 * Structure (720 x 720 full-bleed page, absolute placement, LV_LAYOUT_NONE):
 *   0     image ground   full-bleed hero + flat dim + top/bottom scrims
 *   0     top bar    52  identity + connection left, dot + state right, no rule
 *   58    error slot 28  left-anchored, fades right ("No faults" at rest)
 *   b-103 percent        right-justified, fades left
 *   b-116 layer          left-anchored, fades right
 *   b-87  track      8   single progress bar, no markers
 *   b-78  layer bar  5   thin layer-fraction sub-bar
 *   b-0   metrics    70  five equal cells, flush to the bottom edge
 *
 * This layout sets octoprint_layout_ops_t::full_bleed, so nina_octoprint.c hands
 * it the whole 720 x 720 screen with the dashboard's 16 px outer padding negated:
 * "flush to the edge" means flush to the physical panel edge. Text keeps its
 * former distance from that edge via GL_EDGE_PAD / GL_PANE_PAD, which absorb the
 * 16 px the page no longer gets for free.
 */

#include "nina_octoprint_internal.h"

/* ── Geometry ─────────────────────────────────────────────────────────── */

#define GL_TOPBAR_H     52
#define GL_ERR_Y        58
#define GL_ERR_W       320
#define GL_ERR_H        28
#define GL_PCT_W       400
#define GL_PCT_Y      (-103)
#define GL_LAYER_W     300
#define GL_LAYER_Y    (-116)
#define GL_TRACK_Y     (-87)
#define GL_TRACK_H       8
#define GL_METRIC_H     70

/* Overlay panes are full width of their own text plus the fade tail; the track
 * insets symmetrically by 3 % on each side. */
#define GL_TRACK_PCT    94

/* Text insets from the panel edge. The page is full-bleed, so these carry the
 * 16 px the dashboard's outer padding used to contribute: 18 + 16 and 20 + 16,
 * which keeps every reading exactly where it sat before the canvas grew. */
#define GL_EDGE_PAD     34
#define GL_PANE_PAD     36

/* Directional fill: opaque-ish at the anchored edge, transparent past the text.
 * Stops are 0..255 across the object width; 204 = 80 %, matching the mockup. */
#define GL_PANE_OPA    LV_OPA_60
#define GL_FADE_STOP   204

/* ── Primitives ───────────────────────────────────────────────────────── */

/**
 * Paint the directional gradient fill on @p obj: the page ground colour at
 * GL_PANE_OPA on the anchored edge, fading to fully transparent past the text.
 * @p fade_right anchors left (fades rightward); false anchors right.
 */
static void fade_fill(lv_obj_t *obj, bool fade_right)
{
    if (!obj) {
        return;
    }
    uint32_t ground = octo_color(OCTO_COL_BG);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(ground), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(ground), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);

    if (fade_right) {
        lv_obj_set_style_bg_main_opa(obj, GL_PANE_OPA, 0);
        lv_obj_set_style_bg_grad_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_main_stop(obj, 0, 0);
        lv_obj_set_style_bg_grad_stop(obj, GL_FADE_STOP, 0);
    } else {
        lv_obj_set_style_bg_main_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(obj, GL_PANE_OPA, 0);
        lv_obj_set_style_bg_main_stop(obj, 255 - GL_FADE_STOP, 0);
        lv_obj_set_style_bg_grad_stop(obj, 255, 0);
    }
}

/** Column container carrying the directional fill. No border, no radius. */
static lv_obj_t *fade_pane(lv_obj_t *parent, bool fade_right)
{
    lv_obj_t *p = octo_w_row(parent, false, 0);
    fade_fill(p, fade_right);
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
        /* COVER, not CONTAIN: as the page ground the frame should fill it. */
        lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_COVER);
    }
    if (w->img_placeholder) {
        lv_obj_set_style_text_font(w->img_placeholder, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_letter_space(w->img_placeholder, 3, 0);
        lv_obj_align(w->img_placeholder, LV_ALIGN_CENTER, 0, -40);
    }

    /* The mockup's radial vignette needs complex gradients, which this build
     * does not draw; a flat dim plus the two linear scrims is the stand-in. The
     * scrims are what keep the top bar and the metrics row legible over a bright
     * frame now that neither has a fill of its own. */
    lv_obj_t *dim = glass_scrim(page, LV_OPA_20, LV_OPA_20);
    lv_obj_set_size(dim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(dim, 0, 0);

    lv_obj_t *top = glass_scrim(page, LV_OPA_80, LV_OPA_TRANSP);
    lv_obj_set_size(top, LV_PCT(100), 150);
    lv_obj_set_pos(top, 0, 0);

    lv_obj_t *bottom = glass_scrim(page, LV_OPA_TRANSP, LV_OPA_90);
    lv_obj_set_size(bottom, LV_PCT(100), 330);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ── Top bar ──────────────────────────────────────────────────────────── */

static void build_topbar(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Flush to the top edge, borderless and unfilled — the top scrim is the
     * only thing separating it from the image. */
    lv_obj_t *bar = octo_w_row(page, true, 12);
    lv_obj_set_size(bar, LV_PCT(100), GL_TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, GL_EDGE_PAD, 0);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = octo_w_row(bar, true, 12);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Printer identity. Static text: the job file name is not shown anywhere. */
    lv_obj_t *brand = octo_w_label(left, "OCTOPRINT", &lv_font_montserrat_14,
                                   &octo_style_value);
    lv_obj_set_style_text_letter_space(brand, 3, 0);

    /* Connection reads as plain text, not a chip: strip the fill and border the
     * factory gives it and keep only the live-recoloured dot. */
    octo_w_conn_chip(left, w);
    if (w->conn_chip) {
        lv_obj_set_style_bg_opa(w->conn_chip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(w->conn_chip, 0, 0);
        lv_obj_set_style_pad_hor(w->conn_chip, 0, 0);
        lv_obj_set_style_pad_ver(w->conn_chip, 0, 0);
    }

    /* State right-justified. Dot and text are recoloured live by the update
     * path, so only the type scale is set here. */
    lv_obj_t *state = octo_w_state_line(bar, w);
    lv_obj_set_style_pad_all(state, 0, 0);
    if (w->state_dot) {
        lv_obj_set_size(w->state_dot, 9, 9);
    }
    if (w->lbl_state) {
        lv_obj_set_style_text_font(w->lbl_state, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(w->lbl_state, 3, 0);
    }
}

/* ── Error slot ───────────────────────────────────────────────────────── */

static void build_error_slot(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* The chip itself becomes the fading pane: one object, no wrapper, and the
     * resting "No faults" state occupies its designed place so a fault never
     * reflows anything. */
    octo_w_status_strip(page, w);
    if (!w->error_strip) {
        return;
    }
    fade_fill(w->error_strip, true);
    lv_obj_set_size(w->error_strip, GL_ERR_W, GL_ERR_H);
    lv_obj_set_pos(w->error_strip, 0, GL_ERR_Y);
    lv_obj_set_style_pad_left(w->error_strip, GL_EDGE_PAD, 0);
    lv_obj_set_style_pad_right(w->error_strip, 0, 0);
    lv_obj_set_flex_align(w->error_strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
}

/* ── Completion ───────────────────────────────────────────────────────── */

static void build_percent(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Right-justified, so the fill fades leftward per the page's direction rule.
     * The layer pane takes the left side; the two never overlap and the percent
     * geometry stands alone when DisplayLayerProgress is absent. */
    lv_obj_t *pane = fade_pane(page, false);
    lv_obj_set_size(pane, GL_PCT_W, LV_SIZE_CONTENT);
    lv_obj_align(pane, LV_ALIGN_BOTTOM_RIGHT, 0, GL_PCT_Y);
    lv_obj_set_style_pad_right(pane, GL_PANE_PAD, 0);
    lv_obj_set_style_pad_top(pane, 12, 0);
    lv_obj_set_style_pad_bottom(pane, 14, 0);
    lv_obj_set_flex_align(pane, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *row = octo_w_row(pane, true, 4);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* The mockup sets the percentage in Playfair; that face carries only digits
     * and ':', and the value is rendered "61.8", so Montserrat 64 is used. */
    w->lbl_pct = octo_w_label(row, "--", &lv_font_montserrat_64, &octo_style_value);
    w->lbl_pct_unit = octo_w_label(row, "%", &lv_font_montserrat_28, &octo_style_accent);
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, 8, 0);

    w->lbl_pct_sub = octo_w_caption(pane, "COMPLETE");
    lv_obj_set_width(w->lbl_pct_sub, LV_PCT(100));
    lv_obj_set_style_text_align(w->lbl_pct_sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_margin_top(w->lbl_pct_sub, 8, 0);
}

/* ── Progress ─────────────────────────────────────────────────────────── */

static void build_track(lv_obj_t *page, octoprint_widgets_t *w)
{
    octo_w_progress_bar(page, w);
    if (!w->bar_progress) {
        return;
    }
    lv_obj_set_width(w->bar_progress, LV_PCT(GL_TRACK_PCT));
    lv_obj_set_height(w->bar_progress, GL_TRACK_H);
    lv_obj_align(w->bar_progress, LV_ALIGN_BOTTOM_MID, 0, GL_TRACK_Y);
    lv_obj_set_style_radius(w->bar_progress, GL_TRACK_H / 2, 0);
    lv_obj_set_style_radius(w->bar_progress, GL_TRACK_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(w->bar_progress, LV_OPA_40, 0);
    lv_obj_set_style_border_opa(w->bar_progress, LV_OPA_50, 0);

    /* Fill runs dim-to-bright in the accent hue: one token, two opacity stops,
     * so it survives every theme and stays inside the 2-stop gradient budget. */
    lv_obj_set_style_bg_grad_color(w->bar_progress,
                                   lv_color_hex(octo_color(OCTO_COL_ACCENT)),
                                   LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(w->bar_progress, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_main_opa(w->bar_progress, LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_opa(w->bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
}

/* ── Layer ────────────────────────────────────────────────────────────── */

static void build_layer(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* The left-anchored readout lives inside layer_cell, so the update path
     * hides the whole layer story in one go when DisplayLayerProgress is not
     * installed. The cell itself is an invisible full-page absolute frame; only
     * its child pane paints. w->layer_segs stays NULL: the 12-segment sub-bar
     * that used to sit under the track read as a second progress bar, and the
     * update path null-checks every handle. */
    w->layer_cell = octo_w_row(page, false, 0);
    lv_obj_set_layout(w->layer_cell, LV_LAYOUT_NONE);
    lv_obj_set_size(w->layer_cell, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(w->layer_cell, 0, 0);

    lv_obj_t *pane = fade_pane(w->layer_cell, true);
    lv_obj_set_size(pane, GL_LAYER_W, LV_SIZE_CONTENT);
    lv_obj_align(pane, LV_ALIGN_BOTTOM_LEFT, 0, GL_LAYER_Y);
    lv_obj_set_style_pad_left(pane, GL_PANE_PAD, 0);
    lv_obj_set_style_pad_top(pane, 10, 0);
    lv_obj_set_style_pad_bottom(pane, 12, 0);
    lv_obj_set_flex_align(pane, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *nums = octo_w_row(pane, true, 6);
    lv_obj_set_size(nums, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(nums, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(nums, "--", &lv_font_montserrat_36,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(nums, "/ --", &lv_font_montserrat_22,
                                      &octo_style_label);

    lv_obj_t *cap = octo_w_caption(pane, "LAYER");
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_margin_top(cap, 2, 0);
}

/* ── Metrics row ──────────────────────────────────────────────────────── */

/**
 * Dress one bottom-row cell. Every cell gets identical treatment — no fill, no
 * border, no accent — so the row reads as one continuous strip of readings.
 * Both the time tiles and the TILE temperature elements are a caption over a
 * value, so one shaping pass covers all five.
 */
static void metric_cell(lv_obj_t *cell)
{
    if (!cell) {
        return;
    }
    lv_obj_set_height(cell, LV_PCT(100));
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_style_pad_hor(cell, 4, 0);
    lv_obj_set_style_pad_row(cell, 4, 0);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    uint32_t n = lv_obj_get_child_count(cell);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(cell, i);
        if (!c || !lv_obj_check_type(c, &lv_label_class)) {
            continue;
        }
        /* Percent width against the grown cell, clipped rather than wrapped, so
         * a long "250.3/250 °C" can never make one cell taller than its
         * neighbours or push the five-way split off balance. */
        lv_obj_set_width(c, LV_PCT(100));
        lv_label_set_long_mode(c, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void build_metrics(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Flush to the bottom edge: zero padding below it, a hairline top rule and
     * nothing else, so the reclaimed space goes to the image. */
    lv_obj_t *row = octo_w_row(page, true, 0);
    lv_obj_set_size(row, LV_PCT(100), GL_METRIC_H);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(octo_color(OCTO_COL_LABEL)), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_40, 0);

    metric_cell(octo_w_time_tile(row, "ELAPSED", &lv_font_montserrat_20,
                                 &w->lbl_elapsed));
    metric_cell(octo_w_time_tile(row, "REMAINING", &lv_font_montserrat_20,
                                 &w->lbl_remaining));

    /* Finish time is DisplayLayerProgress-only, so its cell is the handle the
     * update path hides wholesale. It carries no accent: every cell alike. */
    w->finish_cell = octo_w_time_tile(row, "FINISH AT", &lv_font_montserrat_20,
                                      &w->lbl_finish);
    metric_cell(w->finish_cell);

    /* TILE temperature elements: caption over "214.9/215 °C", same shape as a
     * time tile, so the row stays uniform. No heat rail anywhere on this page. */
    metric_cell(octo_w_temp(row, "NOZZLE", OCTO_TEMP_TILE, true, &w->nozzle));
    metric_cell(octo_w_temp(row, "BED", OCTO_TEMP_TILE, false, &w->bed));
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void glass_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Absolute placement: the overlays float over the image ground, which no
     * flex or grid flow can express. */
    lv_obj_set_layout(page, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(page, 0, 0);

    build_ground(page, w);
    build_topbar(page, w);
    build_error_slot(page, w);
    build_percent(page, w);
    build_track(page, w);
    build_layer(page, w);
    build_metrics(page, w);
}

const octoprint_layout_ops_t octoprint_layout_glass = {
    .name       = "Immersive image",
    .full_bleed = true,   /* the image IS the page: 720x720, no outer inset */
    .build      = glass_build,
};
