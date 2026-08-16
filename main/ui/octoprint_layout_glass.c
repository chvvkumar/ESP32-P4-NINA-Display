/**
 * @file octoprint_layout_glass.c
 * @brief OctoPrint layout 2 — "Immersive image" (mockup v4, revision 2).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from an octo_color() token or
 * a shared style, so all nine themes work without a change here. This file never
 * reads octoprint_data_t.
 *
 * The print image IS the page: its host runs full-bleed edge to edge with no
 * caption, source chip or corner marks, the frame is staged COVER-sized once by
 * the spine (octoprint_layout_ops_t::image_cover) and the host clips the middle
 * 720 px of it, and everything else floats over it. Nothing sits
 * in a bordered box. The layer and percent readouts float unfilled on the
 * image, caption above value. Only the error slot paints a HORIZONTAL gradient
 * fill: partly opaque behind its own text, fully transparent just past it. LVGL
 * carries per-stop opacity in bg_main_opa / bg_grad_opa, so one object with
 * LV_GRAD_DIR_HOR expresses this in two stops — which is all this build has
 * (CONFIG_LV_GRADIENT_MAX_STOPS=2).
 *
 * Structure (720 x 720 full-bleed page, absolute placement, LV_LAYOUT_NONE).
 * The hero host is the ONLY child of the page itself; EVERY other element below
 * hangs off one full-page transparent overlay layer (octo_w_overlay_layer() ->
 * w->overlay_layer), created straight after the hero so it draws over it. That
 * is what makes the "show readings" toggle a single HIDDEN flag on one object:
 * the picture stays, the readings go, and because every position here is
 * absolute nothing reflows either way.
 *
 *   page  image ground   full-bleed hero host, no scrim of its own
 *   0     flat dim + flat bottom strip (b-94..b-0), on the layer
 *   0     top bar    52  connection left, dot + state right, no fill
 *   58    error slot 28  left-anchored, fades right (hidden unless faulted)
 *   b-94  percent        left-anchored, unfilled, caption over value
 *   b-94  layer          right-justified, unfilled, caption over value
 *   b-78  track      8   single progress bar, 8 px clear above and below
 *   b-0   metrics    70  five equal cells, flush to the bottom edge, no rule
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
#define GL_ERR_W       240
#define GL_ERR_H        28
#define GL_PCT_W       280
#define GL_LAYER_W     220
#define GL_TRACK_H       8
#define GL_TRACK_GAP     8      /* equal clearance above and below the bar */
#define GL_METRIC_H     70
#define GL_METRIC_CAP_FONT  (&lv_font_montserrat_16)
#define GL_METRIC_VAL_FONT  (&lv_font_montserrat_28)
#define GL_TRACK_Y    (-(GL_METRIC_H + GL_TRACK_GAP))
/* Both panes share one bottom edge, sitting exactly one gap above the bar. */
#define GL_PCT_Y      (GL_TRACK_Y - GL_TRACK_H - GL_TRACK_GAP)
#define GL_LAYER_Y    GL_PCT_Y
/* Flat strip behind track + metrics: rises to the panes' shared bottom edge so
 * the bar sits on ground, not on bare image. */
#define GL_BOTTOM_H   (-GL_PCT_Y)

/* The track insets symmetrically by 3 % on each side. */
#define GL_TRACK_PCT    94

/* Text insets from the panel edge. The page is full-bleed, so these carry the
 * 16 px the dashboard's outer padding used to contribute: 18 + 16 and 20 + 16,
 * which keeps every reading exactly where it sat before the canvas grew. */
#define GL_EDGE_PAD     34
#define GL_PANE_PAD     36

/* Directional fill (error slot): opaque-ish at the anchored edge, transparent
 * past the text. Stops are 0..255 across the object width; 230 = 90 %, so the
 * ramp ends just past the widest reading rather than a screen-width later. */
#define GL_PANE_OPA    LV_OPA_60
#define GL_FADE_STOP   230

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

/**
 * FLAT darkening scrim in the page ground colour at a single opacity. No
 * gradient: a tall two-stop vertical ramp quantises in RGB565 into visible
 * horizontal steps across the image, so every full-width dim here is flat.
 */
static lv_obj_t *flat_scrim(lv_obj_t *parent, lv_opa_t opa)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_opa(s, opa, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(octo_color(OCTO_COL_BG)), 0);
    return s;
}

/* ── Image ground ─────────────────────────────────────────────────────── */

static void build_ground(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* The hero host fills the page and is the only thing that stays parented to
     * the page: everything else lives on the overlay layer. The host is a card,
     * so the no-image state is a deliberate dark ground with a centred label
     * rather than a hole in the UI; radius and border are dropped so it truly
     * bleeds to the edges. */
    lv_obj_t *host = octo_w_image_hero(page, w);
    lv_obj_set_size(host, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(host, 0, 0);
    lv_obj_set_style_radius(host, 0, 0);
    lv_obj_set_style_border_width(host, 0, 0);

    if (w->img_hero) {
        /* CENTER, not CONTAIN: this layout sets image_cover, so stage_image()
         * already scales the frame ONCE to cover this 720x720 host — the staged
         * copy is at least as large as the box in both axes and the host clips
         * the middle of it. CONTAIN would take that oversized copy and DOWNscale
         * it back into the box on every redraw (a software transform per frame,
         * plus the bands the cover sizing exists to avoid). CENTER pins it 1:1
         * and lets the host do the cropping. */
        lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_CENTER);
    }
    if (w->img_placeholder) {
        lv_obj_set_style_text_font(w->img_placeholder, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_letter_space(w->img_placeholder, 3, 0);
        lv_obj_align(w->img_placeholder, LV_ALIGN_CENTER, 0, -40);
    }

}

/**
 * The two scrims, on the overlay LAYER rather than the page: they exist only to
 * make the readings legible, so they belong to the set the "show readings"
 * toggle hides. With the readings gone the picture is seen undimmed, which is
 * the point of turning them off.
 *
 * One flat page-wide dim, and one flat strip behind the track and metrics row,
 * reaching up to the percent/layer panes' bottom edge. No top scrim at all: the
 * top bar sits directly on the image and stays legible on its own weight and
 * colour.
 */
static void build_scrims(lv_obj_t *layer)
{
    lv_obj_t *dim = flat_scrim(layer, LV_OPA_20);
    lv_obj_set_size(dim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(dim, 0, 0);

    lv_obj_t *bottom = flat_scrim(layer, LV_OPA_70);
    lv_obj_set_size(bottom, LV_PCT(100), GL_BOTTOM_H);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ── Top bar ──────────────────────────────────────────────────────────── */

static void build_topbar(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Flush to the top edge, borderless and unfilled — no band of any kind: the
     * text sits directly over the image. */
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
    /* The chip itself becomes the fading pane: one object, no wrapper. The strip
     * starts hidden and the update path shows it on a fault; placement here is
     * absolute, so nothing reflows either way. */
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
    /* Left-anchored, unfilled: caption above value, floating on the image.
     * The layer pane takes the right side; the two never overlap and the percent
     * geometry stands alone when DisplayLayerProgress is absent. */
    lv_obj_t *pane = octo_w_row(page, false, 0);
    lv_obj_set_size(pane, GL_PCT_W, LV_SIZE_CONTENT);
    lv_obj_align(pane, LV_ALIGN_BOTTOM_LEFT, 0, GL_PCT_Y);
    lv_obj_set_style_pad_left(pane, GL_PANE_PAD, 0);
    lv_obj_set_flex_align(pane, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    w->lbl_pct_sub = octo_w_caption(pane, "COMPLETE");
    lv_obj_set_width(w->lbl_pct_sub, LV_PCT(100));
    lv_obj_set_style_text_align(w->lbl_pct_sub, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *row = octo_w_row(pane, true, 4);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* The mockup sets the percentage in Playfair; that face carries only digits
     * and ':', and the value is rendered "61.8", so Montserrat 64 is used. */
    w->lbl_pct = octo_w_label(row, "--", &lv_font_montserrat_64, &octo_style_value);
    w->lbl_pct_unit = octo_w_label(row, "%", &lv_font_montserrat_28, &octo_style_accent);
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, 8, 0);
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
    /* The right-justified readout lives inside layer_cell, so the update path
     * hides the whole layer story in one go when DisplayLayerProgress is not
     * installed. The cell itself is an invisible full-page absolute frame; only
     * its child pane paints. */
    w->layer_cell = octo_w_row(page, false, 0);
    lv_obj_set_layout(w->layer_cell, LV_LAYOUT_NONE);
    lv_obj_set_size(w->layer_cell, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(w->layer_cell, 0, 0);

    /* Unfilled: caption above value, floating on the image. */
    lv_obj_t *pane = octo_w_row(w->layer_cell, false, 0);
    lv_obj_set_size(pane, GL_LAYER_W, LV_SIZE_CONTENT);
    lv_obj_align(pane, LV_ALIGN_BOTTOM_RIGHT, 0, GL_LAYER_Y);
    lv_obj_set_style_pad_right(pane, GL_PANE_PAD, 0);
    lv_obj_set_flex_align(pane, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *cap = octo_w_caption(pane, "LAYER");
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *nums = octo_w_row(pane, true, 6);
    lv_obj_set_size(nums, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(nums, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(nums, "--", &lv_font_montserrat_36,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(nums, "/ --", &lv_font_montserrat_22,
                                      &octo_style_label);
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
        /* Child 0 is the caption, child 1 the value, for time and temp tiles
         * alike; both are bumped so the strip reads at arm's length. */
        lv_obj_set_style_text_font(c, i == 0 ? GL_METRIC_CAP_FONT
                                             : GL_METRIC_VAL_FONT, 0);
    }
}

static void build_metrics(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Flush to the bottom edge, no rule: the strip and the track above share
     * one continuous ground. */
    lv_obj_t *row = octo_w_row(page, true, 0);
    lv_obj_set_size(row, LV_PCT(100), GL_METRIC_H);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

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

    /* One transparent full-page container over the hero carries every reading,
     * scrims included, so the show/hide toggle is a single HIDDEN flag on one
     * object. Created after the hero, so it draws over it; it is absolute and
     * unpadded like the page, so every position below is unchanged. */
    lv_obj_t *layer = octo_w_overlay_layer(page, w);

    build_scrims(layer);
    build_topbar(layer, w);
    build_error_slot(layer, w);
    build_percent(layer, w);
    build_track(layer, w);
    build_layer(layer, w);
    build_metrics(layer, w);
}

const octoprint_layout_ops_t octoprint_layout_glass = {
    .name       = "Immersive image",
    .full_bleed = true,   /* the image IS the page: 720x720, no outer inset */
    .image_cover = true,  /* staged once cover-sized in the spine, host crops; no per-redraw scale */
    .build      = glass_build,
};
