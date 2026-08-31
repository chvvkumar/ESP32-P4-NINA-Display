/**
 * @file nina_layout_image.c
 * @brief Layout 1 — Image-forward NINA page.
 *
 * The last decoded capture fills the page; two transparent text groups sit
 * flush to the top and bottom screen edges over it: identity (top) and one
 * 64 px baseline value row over a 12 px block ledge (bottom). The value row
 * carries TOTAL RMS and the sub counter on the left, elapsed seconds and the
 * filter name on the right, with an empty centre column where the page dots
 * live. No tiles, no scrim, no stamp, no captions, no guiding underline:
 * legibility rests on the text alone, by the user's choice after on-device
 * review.
 *
 * The capture is the RGB565 buffer the existing FETCH_THUMBNAIL path already
 * decodes — no second fetch. One retained copy per instance, kept alive only
 * while that instance's page is visible (nina_layout_image_release_capture()).
 *
 * All entry points run with the LVGL display lock held by the caller, including
 * nina_layout_image_set_capture() (tasks.c calls it inside the same
 * bsp_display_lock() section as nina_dashboard_set_thumbnail()). The one
 * exception is nina_layout_image_needs_capture(), which only reads and touches
 * no LVGL object; the matching latch write lives in
 * nina_layout_image_note_capture_request(), which does take the lock's caller.
 *
 * Family split: the builder below is the SQUARE one and is compiled out of the
 * round binary, where nina_layout_image_round.c defines the same four entry
 * points (radial board 2). The retained-capture store and its handoff, at the
 * bottom of this file, are compiled on both families and serve all four capture
 * layouts (1 Image-forward, 2 Halo, 3 Meridian, 4 Orbit), including the one-off
 * COVER pre-scale layout 2 asks for.
 */

#include "nina_layout_alt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "app_config.h"
#include "image_red_remap.h"
#include "jpeg_utils.h"
#include "nina_connection.h"
#include "themes.h"
#include "ui_helpers.h"

/* Everything from here to the capture handoff is the SQUARE Image-forward
 * builder. On the round family nina_layout_image_round.c defines the same
 * three entry points and these bodies would be unreferenced, so the whole
 * region is compiled out (addendum section 6 rule 2). The retained-capture
 * store below the guard is shared by both families, and so are the two helpers
 * the square builder also calls: their prototypes stay here, above the guard,
 * so no square call site precedes a declaration. */

static bool      if_red_night(void);
static void      if_release(int instance);

#if !CONFIG_NINA_FAMILY_ROUND

LV_FONT_DECLARE(lv_font_material_safety);
LV_FONT_DECLARE(lv_font_hanken_bold_64);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ── design tokens ───────────────────────────────────────────────────────── */

/* Exact built-in Montserrat where one exists: 14, 24, 40 (enabled in sdkconfig +
 * sdkconfig.defaults, no generated font files). LVGL ships only even sizes, so
 * the design's 15 steps to 16. The safety shield is bundled at 40 px only.
 * Numbers use Hanken Grotesk Bold: every digit is 560 units wide, so ticking
 * values do not walk (Montserrat's "1" is narrower than its other digits). */
#define IF_FONT_STEP      (&lv_font_montserrat_28)
#define IF_STEP_COL_W     280   /* fixed step/safety column, fits "Smart Exposure" @28 */
#define IF_FONT_IDENT     (&lv_font_montserrat_16)  /* design 15 — no built-in */
#define IF_FONT_TARGET    (&lv_font_montserrat_40)
#define IF_FONT_ELAPSED   (&lv_font_hanken_bold_64)
#define IF_FONT_RMS       (&lv_font_hanken_bold_64)   /* match the exposure readout */
#define IF_FONT_COUNT     (&lv_font_hanken_bold_28)
#define IF_FONT_FILTER    (&lv_font_montserrat_24)

/* The page root IS full-bleed 720x720 for this layout: create_dashboard_page()
 * in nina_dashboard.c sizes it screen_size() square and offsets it by
 * -OUTER_PADDING to negate main_cont's padding (same trick as the six image
 * pages in nina_image_page.c). The two text groups hug the panel edges; only
 * their inner padding keeps the glyphs off the physical edge. */
#define IF_GROUP_PAD_V    8
#define IF_GROUP_PAD_H    12
#define IF_BLOCK_H        12    /* block ledge, flush on the bottom edge */
#define IF_BAND_H         86    /* value band above the ledge */
#define IF_GAP_LEFT       16    /* RMS -> counter */
#define IF_GAP_RIGHT      12    /* elapsed -> filter */

#define IF_TARGET_FG      0xf2f2f4
#define IF_OK_FG          0x10b981

/* Material Symbols codepoints (UTF-8), same glyphs the arc layout uses. */
#define IF_ICON_SAFE      "\xee\xa3\xa8"  /* U+E8E8 verified_user */
#define IF_ICON_UNSAFE    "\xef\x80\x92"  /* U+F012 gpp_bad       */
#define IF_ICON_UNKNOWN   "\xef\x80\x94"  /* U+F014 gpp_maybe     */

/* ── forward declarations ────────────────────────────────────────────────── */

static uint32_t  if_dim(uint32_t color, int gb);
static lv_obj_t *if_group(lv_obj_t *parent);
static lv_obj_t *if_hrow(lv_obj_t *parent, int gap);
static lv_obj_t *if_label(lv_obj_t *parent, const lv_font_t *font, const char *text);
static lv_obj_t *if_fixed_label(lv_obj_t *parent, const lv_font_t *font,
                                const char *sample, lv_text_align_t align);
static void      if_set_text(lv_obj_t *lbl, const char *text);
static uint32_t  if_filter_color(const char *filter, int inst, int gb);
static void      if_elapsed_cb(void *ud, int secs);
static void      if_theme_page(dashboard_page_t *p, int gb);

/* ── small helpers ───────────────────────────────────────────────────────── */

static uint32_t if_dim(uint32_t color, int gb) {
    return app_config_apply_brightness(color, gb);
}

static lv_obj_t *if_label(lv_obj_t *parent, const lv_font_t *font, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* Value-row label: fixed width sized from @p sample, one line tall, aligned
 * inside its box so a pinned glyph (closing quote, trailing "s") never walks.
 * Baseline alignment: LVGL has no baseline, so every label is bottom-aligned
 * (flex cross END) and nudged up by the difference between its font's
 * base_line and the 64 px font's, which puts all baselines on one line. */
static lv_obj_t *if_fixed_label(lv_obj_t *parent, const lv_font_t *font,
                                const char *sample, lv_text_align_t align) {
    lv_obj_t *l = if_label(parent, font, "--");
    lv_point_t sz = { 0, 0 };
    lv_text_get_size(&sz, sample, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(l, sz.x + 2);
    lv_obj_set_height(l, lv_font_get_line_height(font));
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_style_translate_y(l, font->base_line - IF_FONT_ELAPSED->base_line, 0);
    return l;
}

static void if_set_text(lv_obj_t *lbl, const char *text) {
    if (lbl && text && strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

/* Filter name tone: the configured filter colour, theme text on Red Night,
 * label tone when no filter is known. Already brightness-applied. */
static uint32_t if_filter_color(const char *filter, int inst, int gb) {
    if (!current_theme) return if_dim(0x808080, gb);
    if (if_red_night()) return if_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return if_dim(current_theme->label_color, gb);
}

/* Text group: a fully transparent flex column (no bg, no border) that only
 * lays its rows out; the 8/12 px inner padding keeps text off the panel edge. */
static lv_obj_t *if_group(lv_obj_t *parent) {
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(t, IF_GROUP_PAD_V, 0);
    lv_obj_set_style_pad_hor(t, IF_GROUP_PAD_H, 0);
    lv_obj_set_style_pad_gap(t, 0, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    return t;
}

/* Transparent content-sized row, children bottom-aligned (cross END). */
static lv_obj_t *if_hrow(lv_obj_t *parent, int gap) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_gap(r, gap, 0);
    return r;
}

/* Sole writer of the elapsed label: the 200 ms tick via the sub bar
 * (nina_subbar_set_elapsed_cb) and the idle reset below, which passes -1. */
static void if_elapsed_cb(void *ud, int secs) {
    dashboard_page_t *p = (dashboard_page_t *)ud;
    if (!p || !p->alt.lbl_elapsed) return;
    char buf[16];
    if (secs < 0) {
        snprintf(buf, sizeof(buf), "--s");
    } else {
        if (secs > 9999) secs = 9999;
        snprintf(buf, sizeof(buf), "%ds", secs);
    }
    if_set_text(p->alt.lbl_elapsed, buf);
}

/* ── create ──────────────────────────────────────────────────────────────── */

void nina_layout_image_create(dashboard_page_t *p, lv_obj_t *parent, int page_index) {
    if (!p || !parent) return;

    p->alt.inst = page_index;

    /* The root carries no layout of its own: the capture is absolutely placed
     * and one content layer holds the two text groups. */
    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 1 — the capture. Sized to the full panel and centred. Kept alive (with a
     * NULL source) even when there is no capture so the tap target for the
     * full-screen preview is always present. */
    p->alt.cap_img = lv_image_create(parent);
    lv_obj_set_size(p->alt.cap_img, screen_size(), screen_size());
    lv_obj_center(p->alt.cap_img);
    /* ponytail: LVGL software-scales the cover fit on redraw (PPA only helps at
     * 1.0x). Fine for a background that changes once per sub; if it ever shows
     * up in frame time, pre-scale once with ppa_scale_rgb565() in set_capture. */
    lv_image_set_inner_align(p->alt.cap_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(p->alt.cap_img, NULL);
    nina_dashboard_bind_tap(p->alt.cap_img, NINA_TAP_CAPTURE);

    /* A rebuild that kept the retained frame (theme/URL edit rather than a page
     * leave) re-attaches it instead of showing an empty background. */
    nina_layout_image_reattach_capture(page_index);

    /* Content layer: the two groups pushed flush to the top and bottom screen
     * edges (no inset); everything between them is the picture. */
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
    lv_obj_align(layer, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_set_flex_flow(layer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(layer, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(layer, GRID_GAP, 0);

    /* ── Group 1 — top tile: a row. Target name fills the left space; the
     * sequence step (large) sits over the safety shield on the right. ── */
    p->alt.tile_ident = if_group(layer);
    lv_obj_set_flex_flow(p->alt.tile_ident, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p->alt.tile_ident, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(p->alt.tile_ident, 14, 0);

    /* Target name: fills the space left of the step column, LEFT justified, and
     * wraps to a second line for a long name rather than pushing into the step
     * (flex separates the two, so a wrap can never overlap the step column). */
    p->alt.lbl_target = if_label(p->alt.tile_ident, IF_FONT_TARGET, "--");
    lv_obj_set_flex_grow(p->alt.lbl_target, 1);
    lv_label_set_long_mode(p->alt.lbl_target, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(p->alt.lbl_target, LV_TEXT_ALIGN_LEFT, 0);

    /* Right column: sequence step on top, safety shield below, right-aligned.
     * Fixed width (never flex-grow) so the step has a bounded box LONG_DOT can
     * ellipsise into; content-width here collapses to "..." because the step
     * label wants 100 % of a parent that wants the label's content. */
    p->alt.row_seq = lv_obj_create(p->alt.tile_ident);
    lv_obj_remove_style_all(p->alt.row_seq);
    lv_obj_remove_flag(p->alt.row_seq, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(p->alt.row_seq, IF_STEP_COL_W);
    lv_obj_set_height(p->alt.row_seq, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->alt.row_seq, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p->alt.row_seq, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(p->alt.row_seq, 6, 0);
    nina_dashboard_bind_tap(p->alt.row_seq, NINA_TAP_SEQUENCE);

    p->alt.lbl_seq_step = if_label(p->alt.row_seq, IF_FONT_STEP, "--");
    lv_obj_set_width(p->alt.lbl_seq_step, LV_PCT(100));
    lv_obj_set_height(p->alt.lbl_seq_step, lv_font_get_line_height(IF_FONT_STEP));
    lv_label_set_long_mode(p->alt.lbl_seq_step, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(p->alt.lbl_seq_step, LV_TEXT_ALIGN_RIGHT, 0);

    p->alt.lbl_safety = if_label(p->alt.row_seq, &lv_font_material_safety, IF_ICON_UNKNOWN);

    /* ── Group 2 — value band (86 px) over the 12 px block ledge, flush to the
     * bottom edge. No padding of its own: the row carries the side padding and
     * the ledge runs the full width. ── */
    p->alt.tile_hero = lv_obj_create(layer);
    lv_obj_remove_style_all(p->alt.tile_hero);
    lv_obj_remove_flag(p->alt.tile_hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.tile_hero, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(p->alt.tile_hero, LV_PCT(100), IF_BAND_H + IF_BLOCK_H);
    lv_obj_set_style_pad_all(p->alt.tile_hero, 0, 0);
    lv_obj_set_style_pad_gap(p->alt.tile_hero, 0, 0);
    lv_obj_set_flex_flow(p->alt.tile_hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p->alt.tile_hero, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Value row: one 64 px line, left and right groups pushed apart so the
     * centre column (where the page dots sit) stays empty. */
    int row_h = lv_font_get_line_height(IF_FONT_ELAPSED);
    p->alt.row_vals = lv_obj_create(p->alt.tile_hero);
    lv_obj_remove_style_all(p->alt.row_vals);
    lv_obj_remove_flag(p->alt.row_vals, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.row_vals, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(p->alt.row_vals, LV_PCT(100), row_h);
    lv_obj_set_style_pad_hor(p->alt.row_vals, IF_GROUP_PAD_H, 0);
    lv_obj_set_style_pad_ver(p->alt.row_vals, 0, 0);
    lv_obj_set_style_pad_gap(p->alt.row_vals, 0, 0);
    lv_obj_set_style_margin_bottom(p->alt.row_vals,
        (IF_BAND_H > row_h) ? (IF_BAND_H - row_h) : 0, 0);
    lv_obj_set_flex_flow(p->alt.row_vals, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p->alt.row_vals, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    /* Left: TOTAL RMS, bottom-aligned on the elapsed baseline. */
    p->alt.grp_left = if_hrow(p->alt.row_vals, IF_GAP_LEFT);
    nina_dashboard_bind_tap(p->alt.grp_left, NINA_TAP_RMS);
    p->alt.lbl_rms   = if_fixed_label(p->alt.grp_left, IF_FONT_RMS, "99.99\"", LV_TEXT_ALIGN_RIGHT);

    /* Centre: a two-line stack, counter over filter name, centred both ways in
     * the row. Full row height so bottom-alignment fills the row and the column
     * centres its contents vertically; the page dots are hidden on this layout
     * so nothing collides here. */
    lv_obj_t *grp_center = lv_obj_create(p->alt.row_vals);
    lv_obj_remove_style_all(grp_center);
    lv_obj_remove_flag(grp_center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(grp_center, row_h);
    lv_obj_set_width(grp_center, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grp_center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grp_center, 2, 0);
    nina_dashboard_bind_tap(grp_center, NINA_TAP_SEQUENCE);

    p->alt.lbl_count = if_fixed_label(grp_center, IF_FONT_COUNT, "9999 / 9999", LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_translate_y(p->alt.lbl_count, 0, 0);   /* stack is self-centred */
    p->alt.lbl_filter = if_label(grp_center, IF_FONT_FILTER, "");
    lv_obj_set_style_text_align(p->alt.lbl_filter, LV_TEXT_ALIGN_CENTER, 0);
    nina_dashboard_bind_tap(p->alt.lbl_filter, NINA_TAP_FILTER);

    /* Right: elapsed, "s" pinned by right alignment, bottom-aligned. */
    lv_obj_t *grp_right = if_hrow(p->alt.row_vals, IF_GAP_RIGHT);
    p->alt.lbl_elapsed = if_fixed_label(grp_right, IF_FONT_ELAPSED, "9999s", LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(p->alt.lbl_elapsed, "--s");

    /* The ledge. The spine drives its progress and its theme. */
    nina_subbar_create(&p->subbar, p->alt.tile_hero, IF_BLOCK_H);
    nina_subbar_set_elapsed_cb(&p->subbar, if_elapsed_cb, p);

    if_theme_page(p, app_config_get()->color_brightness);
}

/* ── theme ───────────────────────────────────────────────────────────────── */

static void if_theme_page(dashboard_page_t *p, int gb) {
    if (!p || !p->alt.cap_img || !current_theme) return;

    bool red = if_red_night();
    uint32_t target_fg = red ? current_theme->text_color : IF_TARGET_FG;
    uint32_t text      = if_dim(current_theme->text_color, gb);

    /* The groups are transparent: no bg or border is ever painted on them. */
    lv_obj_set_style_text_color(p->alt.lbl_target, lv_color_hex(if_dim(target_fg, gb)), 0);
    lv_obj_set_style_text_color(p->alt.lbl_seq_step,
        lv_color_hex(if_dim(current_theme->header_text_color, gb)), 0);

    lv_obj_set_style_text_color(p->alt.lbl_count, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(p->alt.lbl_elapsed, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(p->alt.lbl_filter,
        lv_color_hex(if_filter_color(p->subbar.cached_filter, p->alt.inst, gb)), 0);
    /* The RMS tone depends on the live value; the next update() repaints it. */
    lv_obj_set_style_text_color(p->alt.lbl_rms,
        lv_color_hex(if_dim(red ? current_theme->rms_color : current_theme->label_color, gb)), 0);
}

void nina_layout_image_apply_theme(dashboard_page_t *p) {
    if (!p) return;
    nina_layout_image_note_theme_switch(p->alt.inst);
    if_theme_page(p, app_config_get()->color_brightness);
}

/* ── view mode ───────────────────────────────────────────────────────────── */

/* The square Image-forward page has one composition: the text groups always sit
 * over whatever the background is, with or without a picture, which is the
 * shipped behaviour and the one this family keeps. The view cycle is a round
 * board feature, so this is deliberately empty rather than absent: the spine
 * dispatches to it on both families. */
void nina_layout_image_set_view(dashboard_page_t *p, nina_view_mode_t mode) {
    LV_UNUSED(p);
    LV_UNUSED(mode);
}

/* ── update ──────────────────────────────────────────────────────────────── */

void nina_layout_image_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb) {
    if (!p || !d || !p->alt.cap_img || !current_theme) return;

    p->alt.inst = instance_idx;

    bool red = if_red_night();

    /* Shield and current step. */
    {
        const char *icon;
        uint32_t icon_color;
        if (!d->safety_connected) {
            icon = IF_ICON_UNKNOWN;
            icon_color = red ? current_theme->label_color : 0x999999;
        } else if (d->safety_is_safe) {
            icon = IF_ICON_SAFE;
            icon_color = red ? 0x7f1d1d : 0x4CAF50;
        } else {
            icon = IF_ICON_UNSAFE;
            icon_color = red ? 0xff0000 : 0xF44336;
        }
        if_set_text(p->alt.lbl_safety, icon);
        lv_obj_set_style_text_color(p->alt.lbl_safety, lv_color_hex(if_dim(icon_color, gb)), 0);
        if_set_text(p->alt.lbl_seq_step,
                    (d->container_step[0] != '\0') ? d->container_step : "--");
    }

    if_set_text(p->alt.lbl_target, (d->target_name[0] != '\0') ? d->target_name : "--");

    /* Ledge — the spine owns set_progress() and apply_theme(). */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* TOTAL RMS, threshold tone. */
    {
        char buf[24];
        uint32_t rms_c;
        if (d->guider.rms_total > 0.0f) {
            snprintf(buf, sizeof(buf), "%.2f\"", (double)d->guider.rms_total);
            rms_c = red ? current_theme->rms_color
                        : app_config_get_rms_color(d->guider.rms_total, instance_idx);
        } else {
            snprintf(buf, sizeof(buf), "--");
            rms_c = current_theme->label_color;
        }
        if_set_text(p->alt.lbl_rms, buf);
        lv_obj_set_style_text_color(p->alt.lbl_rms, lv_color_hex(if_dim(rms_c, gb)), 0);
    }

    /* Counter "done / target". */
    {
        char buf[32];
        int target = d->exposure_iterations;
        int done   = d->exposure_count;
        if (done < 0) done = 0;
        if (target > 0 && done > target) done = target;
        if (target > 0) {
            snprintf(buf, sizeof(buf), "%d / %d", done, target);
        } else {
            snprintf(buf, sizeof(buf), "%d", done);
        }
        if_set_text(p->alt.lbl_count, buf);
    }

    /* Filter name in its configured colour. */
    {
        const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
        if_set_text(p->alt.lbl_filter, filter);
        lv_obj_set_style_text_color(p->alt.lbl_filter,
            lv_color_hex(if_filter_color(filter, instance_idx, gb)), 0);
    }

    /* Elapsed: the tick writes the digits; only the idle reset lives here,
     * routed through the same writer. */
    if (d->exposure_total <= 0.0f) if_elapsed_cb(p, -1);
}

#endif  /* !CONFIG_NINA_FAMILY_ROUND */

/* ── capture handoff, compiled on both families ──────────────────────────── */

typedef struct {
    uint8_t        *buf;      /* PSRAM RGB565, owned here, freed with free() */
    lv_image_dsc_t  dsc;
    bool            asked;    /* a fetch was already requested for this gap */
    bool            red;      /* Red Night state the buffer was remapped for  */
    lv_image_align_t align;   /* inner align this buffer was prepared for */
} if_capture_t;

static if_capture_t s_cap[MAX_NINA_INSTANCES];

static bool if_red_night(void) {
    return current_theme && theme_is_red_night(current_theme);
}

bool nina_layout_uses_capture(uint8_t layout) {
    /* 1 Image-forward, 2 Halo, 4 Orbit. Id 3 is retired.
     *
     * On a ROUND panel the Dashboard (0) draws a picture too, so it joins the
     * list there and only there: the square bento grid has no picture and must
     * never be handed one, or the poll loop would fetch a capture it cannot
     * show and the page would try to release a buffer it never took. */
#if CONFIG_NINA_FAMILY_ROUND
    if (layout == 0) return true;
#endif
    return (layout == 1 || layout == 2 || layout == 4);
}

bool nina_layout_image_has_capture(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return false;
    return s_cap[instance].buf != NULL;
}

nina_capture_fit_t nina_layout_capture_fit(uint8_t layout) {
    /* Every round board fills the disc: a letterboxed picture inside a circle
     * wastes the two lens shaped caps and reads as a mistake. The uncropped
     * frame is one long press away, on the full-screen preview, which fetches
     * its own copy. Round layout 0 is a picture layout as well, so it takes the
     * same fit; on square, layout 0 has no picture at all. */
#if CONFIG_NINA_FAMILY_ROUND
    if (layout == 0) return NINA_CAPTURE_FIT_COVER;
#endif
    return (layout == 2 || layout == 4) ? NINA_CAPTURE_FIT_COVER
                                        : NINA_CAPTURE_FIT_CONTAIN;
}

/* Crop the centre square out of the decoded frame and scale it to the panel in
 * ONE PPA SRM pass, replacing the caller's buffer. This is what makes the COVER
 * fit affordable: LV_IMAGE_ALIGN_COVER rescales in software on every redraw,
 * and every redraw on this panel is a full refresh.
 *
 * The scale is a truncated n/16, so the result is at least the panel size and
 * usually a few pixels larger; the widget is told to CENTER it and the extra
 * is clipped by the object, which is exactly the cover crop.
 *
 * Returns false and leaves the caller's buffer untouched when the frame is
 * unusable or the hardware refuses the job, so the caller can fall back to
 * CONTAIN. */
static bool if_cover_prescale(uint8_t **pbuf, uint32_t *pw, uint32_t *ph,
                              uint32_t *psize) {
    const uint32_t panel = (uint32_t)screen_size();
    const uint32_t w = *pw, h = *ph;
    const uint32_t block = (w < h) ? w : h;
    if (block == 0 || panel == 0) return false;

    uint32_t n16 = (panel * 16 + block - 1) / block;   /* ceil, 1/16 steps */
    if (n16 < 1)   n16 = 1;
    if (n16 > 255) n16 = 255;                          /* the field is a uint8_t */
    const uint32_t out = block * n16 / 16;
    /* PPA hangs on a 0 px or over-8191 px output axis; jpeg_utils guards it too,
     * but a bad frame should never get that far. */
    if (out == 0 || out > 8191) return false;

    /* 128 B aligned address AND size: the PPA rejects anything else. */
    const size_t dst_size = ((size_t)out * out * 2 + 127) & ~(size_t)127;
    uint8_t *dst = heap_caps_aligned_alloc(128, dst_size, MALLOC_CAP_SPIRAM);
    if (!dst) return false;

    ppa_srm_job_t job = {
        .src           = *pbuf,
        .src_stride_px = w,
        .src_h         = h,
        .block_x       = (w - block) / 2,
        .block_y       = (h - block) / 2,
        .block_w       = block,
        .block_h       = block,
        .rotate_cw     = 0,
        .hflip         = false,
        .vflip         = false,
        .dst           = dst,
        .dst_buf_size  = dst_size,
        .dst_w         = out,
        .dst_h         = out,
        .dst_x         = 0,
        .dst_y         = 0,
        .scale_n16     = (uint8_t)n16,
        .clear_dst     = false,
    };
    if (ppa_srm_rgb565(&job) != ESP_OK) {
        free(dst);
        return false;
    }

    free(*pbuf);
    *pbuf  = dst;
    *pw    = out;
    *ph    = out;
    *psize = (uint32_t)dst_size;
    return true;
}

/* Point the widget at the retained buffer with the fit it was prepared for. */
static void if_attach(dashboard_page_t *p, const if_capture_t *c) {
    if (!p->alt.cap_img || !c->buf) return;
    lv_image_set_src(p->alt.cap_img, &c->dsc);
    lv_image_set_inner_align(p->alt.cap_img, c->align);
}

/* Drop the retained buffer for one instance and detach it from the widget.
 * Callers hold the LVGL display lock. With no capture the page falls back to
 * the theme bg_main behind the text groups — no placeholder card. */
static void if_release(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;

    dashboard_page_t *p = &pages[instance];
    if (nina_layout_uses_capture(p->layout) && p->alt.cap_img) {
        lv_image_set_src(p->alt.cap_img, NULL);
    }
    if (s_cap[instance].buf) {
        free(s_cap[instance].buf);
        s_cap[instance].buf = NULL;
    }
    memset(&s_cap[instance].dsc, 0, sizeof(s_cap[instance].dsc));
    s_cap[instance].asked = false;
    s_cap[instance].align = LV_IMAGE_ALIGN_CONTAIN;

    /* With no picture the page falls back to its readings composition. */
    nina_dashboard_refresh_view(instance);
}

void nina_layout_image_note_theme_switch(int instance) {
    /* The retained capture was red-remapped in place at set_capture() time and
     * cannot be un-remapped. A Red Night switch either way would otherwise
     * leave a full-screen background in the old palette, so drop it and let the
     * next capture arrive correctly remapped (needs_capture() re-arms). */
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    if (s_cap[instance].buf && s_cap[instance].red != if_red_night()) {
        if_release(instance);
    }
}

void nina_layout_image_reattach_capture(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    dashboard_page_t *p = &pages[instance];
    if (!nina_layout_uses_capture(p->layout) || !p->alt.cap_img
        || !s_cap[instance].buf) {
        return;
    }
    if_attach(p, &s_cap[instance]);
}

void nina_layout_image_set_capture(int instance, uint8_t *rgb565,
                                   uint32_t w, uint32_t h, uint32_t size) {
    if (!rgb565) return;

    if (instance < 0 || instance >= MAX_NINA_INSTANCES
        || w == 0 || h == 0 || size == 0
        || !nina_layout_uses_capture(pages[instance].layout)
        || !pages[instance].alt.cap_img) {
        free(rgb565);
        return;
    }

    dashboard_page_t *p = &pages[instance];
    if_capture_t *c = &s_cap[instance];

    /* COVER layouts pay for the crop and the scale ONCE, here, on the PPA. A
     * refusal degrades to CONTAIN with the original frame rather than asking
     * LVGL to cover-scale the picture on every full-panel redraw. */
    lv_image_align_t align = LV_IMAGE_ALIGN_CONTAIN;
    if (nina_layout_capture_fit(p->layout) == NINA_CAPTURE_FIT_COVER
        && if_cover_prescale(&rgb565, &w, &h, &size)) {
        align = LV_IMAGE_ALIGN_CENTER;
    }

    /* Detach before freeing the buffer LVGL is still pointing at. */
    lv_image_set_src(p->alt.cap_img, NULL);
    if (c->buf) free(c->buf);
    c->buf = rgb565;
    c->asked = false;
    c->align = align;

    /* Red Night: remap the frame in place, same rule as the thumbnail overlay.
     * Self-gating — a no-op unless Red Night is the active theme. */
    image_red_remap_rgb565((uint16_t *)c->buf, (size_t)w * h);
    c->red = if_red_night();

    memset(&c->dsc, 0, sizeof(c->dsc));
    c->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    c->dsc.header.w      = w;
    c->dsc.header.h      = h;
    c->dsc.header.stride = w * 2;
    c->dsc.data          = c->buf;
    c->dsc.data_size     = size;

    if_attach(p, c);

    /* The page may have been showing its readings composition while it had no
     * picture; the stored mode takes over now that one exists. */
    nina_dashboard_refresh_view(instance);
}

void nina_layout_image_release_capture(int instance) {
    if_release(instance);
}

bool nina_layout_image_needs_capture(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return false;
    dashboard_page_t *p = &pages[instance];
    if (!nina_layout_uses_capture(p->layout) || !p->alt.cap_img) return false;
    return !s_cap[instance].buf && !s_cap[instance].asked;
}

void nina_layout_image_note_capture_request(int instance, bool asked) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    s_cap[instance].asked = asked;
}
