/**
 * @file nina_octoprint.c
 * @brief OctoPrint 3D Printer page — shared widget library + update path.
 *
 * This file owns EVERYTHING that is not geometry: the shared styles, the widget
 * factories, the layout dispatch table, the update path, the image handoff, the
 * empty state and the theme application. The four octoprint_layout_*.c files
 * only arrange widgets (see nina_octoprint_internal.h for the seam contract).
 *
 * Locking: the caller holds octoprint_data_t::mutex AND the LVGL display lock
 * for the whole of octoprint_page_update() — the same contract nina_json.c has
 * with json_page_update() (see tasks.c: client lock OUTSIDE, display lock
 * INSIDE). This module never takes either lock itself.
 *
 * Image lifetime: octoprint_client frees the retired frame shortly after it
 * releases the mutex, so binding an lv_image straight at data->image_buf would
 * leave the LVGL flush task rendering from a freed pointer between two updates.
 * We therefore memcpy the frame into a UI-owned PSRAM buffer while the lock is
 * held and bind that. The copy happens only when new_image is set, not per poll.
 */

#include "nina_octoprint.h"
#include "nina_octoprint_internal.h"
#include "nina_empty_state.h"
#include "app_config.h"
#include "display_defs.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "octo_ui";

#define OCTO_PAD   16   /* outer padding, matches OUTER_PADDING */
#define OCTO_GAP   12   /* inter-card gap (mockup v1) */
#define OCTO_W     (SCREEN_SIZE - 2 * OCTO_PAD)

/* ── Shared styles ────────────────────────────────────────────────────── */

lv_style_t octo_style_card;
lv_style_t octo_style_label;
lv_style_t octo_style_value;
lv_style_t octo_style_accent;
static bool s_styles_ready = false;

/* ── Page state ───────────────────────────────────────────────────────── */

static lv_obj_t *s_root      = NULL;  /* survives refresh_config */
static lv_obj_t *s_content   = NULL;  /* deleted + rebuilt by refresh_config */
static lv_obj_t *s_backdrop  = NULL;  /* full-cover host for the empty state */
static lv_obj_t *s_empty     = NULL;
static octoprint_widgets_t s_w;

/* UI-owned copy of the decoded frame (see file header). */
static uint8_t       *s_img_copy    = NULL;
static uint32_t       s_img_copy_px = 0;
static lv_image_dsc_t s_img_dsc;

/* Cheap invalidation guards — avoid restyling unchanged widgets every poll. */
static int s_segs_on_cached = -1;

/* ── Small helpers ────────────────────────────────────────────────────── */

static inline int cfg_brightness(void)
{
    return app_config_get()->color_brightness;
}

/** Theme colour, dimmed by the configured colour brightness. */
static uint32_t tcol(uint32_t theme_color, uint32_t fallback)
{
    uint32_t c = current_theme ? theme_color : fallback;
    return app_config_apply_brightness(c, cfg_brightness());
}

static uint32_t col_text(void)   { return tcol(current_theme ? current_theme->text_color     : 0, 0xE5E7EB); }
static uint32_t col_label(void)  { return tcol(current_theme ? current_theme->label_color    : 0, 0x6B7280); }
static uint32_t col_accent(void) { return tcol(current_theme ? current_theme->progress_color : 0, 0x2563EB); }
static uint32_t col_border(void) { return tcol(current_theme ? current_theme->bento_border   : 0, 0x222222); }
static uint32_t col_cardbg(void) { return tcol(current_theme ? current_theme->bento_bg       : 0, 0x0A0A0A); }
static uint32_t col_bg(void)     { return tcol(current_theme ? current_theme->bg_main        : 0, 0x050505); }
static uint32_t col_hot(void)    { return tcol(current_theme ? current_theme->hfr_color      : 0, 0xD97706); }
static uint32_t col_alert(void)  { return tcol(current_theme ? current_theme->rms_color      : 0, 0xEF4444); }

uint32_t octo_color(octo_color_id_t id)
{
    switch (id) {
        case OCTO_COL_TEXT:   return col_text();
        case OCTO_COL_LABEL:  return col_label();
        case OCTO_COL_ACCENT: return col_accent();
        case OCTO_COL_BORDER: return col_border();
        case OCTO_COL_CARDBG: return col_cardbg();
        case OCTO_COL_BG:     return col_bg();
        case OCTO_COL_HOT:    return col_hot();
        case OCTO_COL_ALERT:  return col_alert();
        default:              return col_text();
    }
}

static void set_txt(lv_obj_t *obj, const char *text)
{
    if (obj && text) {
        lv_label_set_text(obj, text);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * Release the UI-owned frame copy. Unbinds the hero first so the LVGL flush
 * task can never render from the pointer we are about to return to the heap.
 * Safe to call when nothing is held.
 */
static void free_img_copy(void)
{
    if (!s_img_copy) {
        return;   /* nothing bound; skip the invalidate an unbind would cost */
    }
    if (s_w.img_hero) {
        lv_image_set_src(s_w.img_hero, NULL);
    }
    heap_caps_free(s_img_copy);
    s_img_copy    = NULL;
    s_img_copy_px = 0;
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
}

/** "2h 08m" / "48m 12s" / "--" for a duration in seconds (-1 = unknown). */
static void fmt_duration(int secs, char *out, size_t out_len)
{
    if (secs < 0) {
        snprintf(out, out_len, "--");
        return;
    }
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    int s = secs % 60;
    if (h > 0) {
        snprintf(out, out_len, "%dh %02dm", h, m);
    } else if (m > 0) {
        snprintf(out, out_len, "%dm %02ds", m, s);
    } else {
        snprintf(out, out_len, "%ds", s);
    }
}

/* ── Style maintenance ────────────────────────────────────────────────── */

static void styles_update(void)
{
    if (!s_styles_ready) {
        lv_style_init(&octo_style_card);
        lv_style_init(&octo_style_label);
        lv_style_init(&octo_style_value);
        lv_style_init(&octo_style_accent);
        s_styles_ready = true;
    }

    lv_style_set_bg_color(&octo_style_card, lv_color_hex(col_cardbg()));
    lv_style_set_bg_opa(&octo_style_card, LV_OPA_COVER);
    lv_style_set_border_color(&octo_style_card, lv_color_hex(col_border()));
    lv_style_set_border_width(&octo_style_card, 1);
    lv_style_set_radius(&octo_style_card, OCTO_CARD_RADIUS);
    lv_style_set_pad_all(&octo_style_card, 0);
    lv_style_set_text_color(&octo_style_card, lv_color_hex(col_text()));

    lv_style_set_text_color(&octo_style_label, lv_color_hex(col_label()));
    lv_style_set_text_letter_space(&octo_style_label, 2);

    lv_style_set_text_color(&octo_style_value, lv_color_hex(col_text()));
    lv_style_set_text_color(&octo_style_accent, lv_color_hex(col_accent()));
}

/* ── Widget factories ─────────────────────────────────────────────────── */

lv_obj_t *octo_w_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &octo_style_card, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(card, true, 0);
    return card;
}

lv_obj_t *octo_w_row(lv_obj_t *parent, bool horizontal, int gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, horizontal ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    if (horizontal) {
        lv_obj_set_style_pad_column(row, gap, 0);
    } else {
        lv_obj_set_style_pad_row(row, gap, 0);
    }
    return row;
}

lv_obj_t *octo_w_label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, lv_style_t *style)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text ? text : "");
    if (font) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    if (style) {
        lv_obj_add_style(lbl, style, 0);
    }
    return lbl;
}

lv_obj_t *octo_w_caption(lv_obj_t *parent, const char *text)
{
    return octo_w_label(parent, text, &lv_font_montserrat_14, &octo_style_label);
}

/* -- temperature element ------------------------------------------------ */

/**
 * Paint the heat gradient on a bar indicator: cool accent at the cold baseline
 * running to alert-red (hot end) or amber (bed) at the top of the scale.
 *
 * ponytail: two stops, because CONFIG_LV_GRADIENT_MAX_STOPS is 2 in this build.
 * The mockup's blue-cyan-amber-red ramp needs 3+ stops; raise that Kconfig and
 * swap in an lv_grad_dsc_t here if the intermediate hues are ever worth it.
 */
static void temp_bar_gradient(lv_obj_t *bar, bool hot)
{
    lv_obj_set_style_bg_color(bar, lv_color_hex(col_accent()), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(hot ? col_alert() : col_hot()),
                                   LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
}

lv_obj_t *octo_w_temp(lv_obj_t *parent, const char *name,
                      octo_temp_variant_t variant, bool hot,
                      octo_temp_el_t *out)
{
    if (!out) {
        return NULL;
    }
    memset(out, 0, sizeof(*out));
    out->variant = variant;
    out->hot     = hot;

    /* TILE stacks caption over value; the other two put them on one line. */
    bool stacked = (variant == OCTO_TEMP_TILE);

    lv_obj_t *root = octo_w_row(parent, false, stacked ? 2 : 6);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    out->root = root;

    if (stacked) {
        out->lbl_name  = octo_w_caption(root, name);
        out->lbl_value = octo_w_label(root, "--", &lv_font_montserrat_20,
                                      &octo_style_value);
        return root;
    }

    /* Value line: name left, reading right. Sized for the worst case
     * ("250.3 / 250 °C") so a high-temp filament cannot reflow the row. */
    lv_obj_t *line = octo_w_row(root, true, 12);
    lv_obj_set_width(line, LV_PCT(100));
    lv_obj_set_height(line, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    out->lbl_name  = octo_w_label(line, name, &lv_font_montserrat_14,
                                  &octo_style_label);
    out->lbl_value = octo_w_label(line, "--", &lv_font_montserrat_22,
                                  &octo_style_value);

    if (variant == OCTO_TEMP_TEXT_ONLY) {
        return root;
    }

    /* Heat bar, stacked under the value line. */
    out->bar = lv_bar_create(root);
    lv_bar_set_range(out->bar, 0, 1000);
    lv_bar_set_value(out->bar, 0, LV_ANIM_OFF);
    lv_obj_set_width(out->bar, LV_PCT(100));
    lv_obj_set_height(out->bar, 10);
    lv_obj_set_style_radius(out->bar, 5, 0);
    lv_obj_set_style_radius(out->bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(out->bar, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_border_color(out->bar, lv_color_hex(col_border()), 0);
    lv_obj_set_style_border_width(out->bar, 1, 0);
    temp_bar_gradient(out->bar, hot);

    /* Target tick: a thin bright line positioned in percent of the bar, so it
     * lands correctly before the first layout pass. */
    out->tick = lv_obj_create(out->bar);
    lv_obj_remove_style_all(out->tick);
    lv_obj_remove_flag(out->tick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(out->tick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(out->tick, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(out->tick, lv_color_hex(col_text()), 0);
    lv_obj_set_size(out->tick, 2, LV_PCT(100));
    lv_obj_add_flag(out->tick, LV_OBJ_FLAG_HIDDEN);
    return root;
}

/* -- image hero --------------------------------------------------------- */

lv_obj_t *octo_w_image_hero(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *host = octo_w_card(parent);

    w->img_hero = lv_image_create(host);
    lv_obj_set_size(w->img_hero, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(w->img_hero);
    lv_obj_add_flag(w->img_hero, LV_OBJ_FLAG_HIDDEN);

    w->img_placeholder = octo_w_label(host, "NO IMAGE",
                                      &lv_font_montserrat_16, &octo_style_label);
    lv_obj_center(w->img_placeholder);
    return host;
}

/* -- chips / strips ----------------------------------------------------- */

lv_obj_t *octo_w_chip(lv_obj_t *parent, const char *text,
                      lv_obj_t **out_dot, lv_obj_t **out_label)
{
    lv_obj_t *chip = octo_w_row(parent, true, 6);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(chip, 9, 0);
    lv_obj_set_style_pad_ver(chip, 3, 0);
    lv_obj_set_style_radius(chip, 11, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(col_cardbg()), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(col_border()), 0);

    lv_obj_t *dot = lv_obj_create(chip);
    lv_obj_remove_style_all(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(col_label()), 0);

    lv_obj_t *lbl = octo_w_label(chip, text, &lv_font_montserrat_12, &octo_style_label);

    if (out_dot) {
        *out_dot = dot;
    }
    if (out_label) {
        *out_label = lbl;
    }
    return chip;
}

lv_obj_t *octo_w_status_strip(lv_obj_t *parent, octoprint_widgets_t *w)
{
    w->error_strip = octo_w_chip(parent, "No faults", &w->error_dot, &w->lbl_error);
    return w->error_strip;
}

lv_obj_t *octo_w_conn_chip(lv_obj_t *parent, octoprint_widgets_t *w)
{
    w->conn_chip = octo_w_chip(parent, "--", &w->conn_dot, &w->lbl_conn);
    return w->conn_chip;
}

/* -- tiles / primitives -------------------------------------------------- */

lv_obj_t *octo_w_time_tile(lv_obj_t *parent, const char *caption,
                           const lv_font_t *font, lv_obj_t **out_value)
{
    lv_obj_t *tile = octo_w_row(parent, false, 2);
    lv_obj_set_size(tile, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    octo_w_caption(tile, caption);
    lv_obj_t *val = octo_w_label(tile, "--",
                                 font ? font : &lv_font_montserrat_20,
                                 &octo_style_value);
    if (out_value) {
        *out_value = val;
    }
    return tile;
}

lv_obj_t *octo_w_progress_bar(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_height(bar, 14);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_style_radius(bar, 7, 0);
    lv_obj_set_style_radius(bar, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(col_border()), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(col_accent()), LV_PART_INDICATOR);
    w->bar_progress = bar;
    return bar;
}

lv_obj_t *octo_w_progress_arc(lv_obj_t *parent, int size, int arc_width,
                              octoprint_widgets_t *w)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, arc_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, arc_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(col_border()), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(col_accent()), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    w->arc_completion = arc;
    return arc;
}

lv_obj_t *octo_w_state_line(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *line = octo_w_row(parent, true, 7);
    lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    w->state_dot = lv_obj_create(line);
    lv_obj_remove_style_all(w->state_dot);
    lv_obj_remove_flag(w->state_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(w->state_dot, 8, 8);
    lv_obj_set_style_radius(w->state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(w->state_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(w->state_dot, lv_color_hex(col_accent()), 0);

    w->lbl_state = octo_w_label(line, "--", &lv_font_montserrat_16, &octo_style_accent);
    lv_obj_set_style_text_letter_space(w->lbl_state, 2, 0);
    return line;
}

/* ── Layout dispatch ──────────────────────────────────────────────────── */

static const octoprint_layout_ops_t *const s_layouts[OCTO_LAYOUT_COUNT] = {
    &octoprint_layout_bento,
    &octoprint_layout_bento,   /* retired alias: slot 1 was "Instrument dial".
                                * The value stays legal in NVS and in
                                * validate_config; the web UI no longer offers
                                * it and it resolves to Bento. Reserved, so the
                                * later slots never renumber. */
    &octoprint_layout_glass,
    &octoprint_layout_typo,
    &octoprint_layout_timeline,
};

static void build_content(void)
{
    memset(&s_w, 0, sizeof(s_w));
    s_segs_on_cached = -1;

    uint8_t idx = app_config_get()->octoprint_layout;
    if (idx >= OCTO_LAYOUT_COUNT) {
        idx = 0;
    }
    const octoprint_layout_ops_t *ops = s_layouts[idx];

    /* Canvas first: a full-bleed layout gets the whole screen, shifted up and
     * left to negate main_cont's OUTER_PADDING (the same negation the image,
     * clock and Spotify pages use). Set on every build, so switching layouts at
     * runtime restores the inset one as well. */
    int size = ops->full_bleed ? SCREEN_SIZE : OCTO_W;
    int off  = ops->full_bleed ? -OCTO_PAD : 0;
    lv_obj_set_size(s_root, size, size);
    lv_obj_set_pos(s_root, off, off);

    s_content = octo_w_row(s_root, false, OCTO_GAP);
    lv_obj_set_size(s_content, LV_PCT(100), LV_PCT(100));

    ESP_LOGI(TAG, "Building OctoPrint page, layout %u (%s)",
             (unsigned)idx, ops->name ? ops->name : "?");
    ops->build(s_content, &s_w);
}

/* ── Empty state ──────────────────────────────────────────────────────── */

static void empty_show(const char *title)
{
    if (!s_backdrop || !s_empty) {
        return;
    }
    nina_empty_state_set_title(s_empty, title);
    lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_backdrop);
    nina_empty_state_show(s_empty);
}

static void empty_hide(void)
{
    if (!s_backdrop) {
        return;
    }
    nina_empty_state_hide(s_empty);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
}

/* ── Page create ──────────────────────────────────────────────────────── */

lv_obj_t *octoprint_page_create(lv_obj_t *parent)
{
    styles_update();
    /* A rebuild from scratch must not inherit the previous tree's frame copy:
     * s_img_copy != NULL with new_image false would leave image_update() showing
     * a hero it never re-bound. The widget table is cleared first because its
     * pointers belong to that previous tree (build_content() clears it again). */
    memset(&s_w, 0, sizeof(s_w));
    free_img_copy();

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    /* Size and position are set by build_content(): they depend on whether the
     * selected layout is full-bleed, and they must be re-applied on every
     * rebuild, not just here. */
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(col_bg()), 0);

    build_content();

    /* Full-coverage empty state: nina_empty_state renders transparent at 80 %
     * inline width, so a full-coverage consumer supplies its own opaque
     * backdrop (see the usage contract in nina_empty_state.h). FLOATING keeps
     * it out of the root's flex flow and lets it cover the whole page. */
    s_backdrop = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(col_bg()), 0);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);

    s_empty = nina_empty_state_create(s_backdrop, ICON_CLOUD_OFF,
                                      "OctoPrint Not Configured",
                                      "Check the 3D Printer settings.", 0);
    return s_root;
}

/* ── Update helpers ───────────────────────────────────────────────────── */

static void temp_update(octo_temp_el_t *t, float actual, float target)
{
    if (!t || !t->root) {
        return;
    }

    char buf[48];
    bool has_a = !isnan(actual);
    bool has_t = !isnan(target) && target > 0.0f;
    if (!has_a) {
        snprintf(buf, sizeof(buf), "--");
    } else if (has_t && t->variant == OCTO_TEMP_TILE) {
        /* The dock tile is narrow, so it drops the spaces: "214.9/215 °C". */
        snprintf(buf, sizeof(buf), "%.1f/%.0f \xC2\xB0" "C",
                 (double)actual, (double)target);
    } else if (has_t) {
        snprintf(buf, sizeof(buf), "%.1f / %.0f \xC2\xB0" "C",
                 (double)actual, (double)target);
    } else {
        snprintf(buf, sizeof(buf), "%.1f \xC2\xB0" "C", (double)actual);
    }
    set_txt(t->lbl_value, buf);

    if (!t->bar) {
        return;   /* TEXT_ONLY / TILE: the value line is the whole element */
    }

    /* Scale runs from a cold 25 C baseline to the target plus 10 % headroom, so
     * an overshoot is visible past the tick. With the heater off there is no
     * target to scale against, so fall back to a fixed 25..100 C span. */
    float top  = has_t ? (target * 1.1f) : 100.0f;
    float span = top - OCTO_TEMP_COLD_C;
    int   val  = 0;
    if (has_a && span > 1.0f) {
        float f = (actual - OCTO_TEMP_COLD_C) / span;
        if (f < 0.0f) {
            f = 0.0f;
        }
        if (f > 1.0f) {
            f = 1.0f;
        }
        val = (int)(f * 1000.0f);
    }
    lv_bar_set_value(t->bar, val, LV_ANIM_OFF);

    if (t->tick) {
        if (has_t && span > 1.0f) {
            int pct = (int)(((target - OCTO_TEMP_COLD_C) / span) * 100.0f);
            if (pct < 0) {
                pct = 0;
            }
            if (pct > 99) {
                pct = 99;
            }
            lv_obj_set_x(t->tick, LV_PCT(pct));
            lv_obj_remove_flag(t->tick, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(t->tick, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void image_update(const octoprint_data_t *data)
{
    if (!s_w.img_hero) {
        return;
    }

    if (!data->image_buf || data->image_w == 0 || data->image_h == 0) {
        /* Client dropped the frame (page left, source switched, or nothing
         * decoded yet). */
        free_img_copy();
        set_hidden(s_w.img_hero, true);
        set_hidden(s_w.img_placeholder, false);
        return;
    }

    if (data->new_image || !s_img_copy) {
        uint32_t px = (uint32_t)data->image_w * (uint32_t)data->image_h;
        if (px != s_img_copy_px) {
            lv_image_set_src(s_w.img_hero, NULL);
            heap_caps_free(s_img_copy);
            s_img_copy    = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
            s_img_copy_px = s_img_copy ? px : 0;
        }
        if (s_img_copy) {
            /* Straight top-down copy: this path needs NO orientation fix.
             * stb (jpeg_utils.c) emits row 0 = top, and binding that buffer to
             * an lv_image_dsc_t renders upright -- nina_image_display.c proves
             * it, since its Solar/Moon/Custom sources pass vflip=false and come
             * out the right way up through the identical decode -> RGB565 ->
             * bind path. The "net vertical flip" note in goes_client.c is stale:
             * vflip there is a per-SOURCE content correction (NESDIS), which is
             * why solar_band_vflip() had to be changed to return false. A
             * bottom-up row copy here is therefore a mirror, not a correction.
             * A camera mounting flip belongs in OctoPrint's webcam settings. */
            memcpy(s_img_copy, data->image_buf, px * 2);
            s_img_dsc.data          = s_img_copy;
            s_img_dsc.data_size     = px * 2;
            s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            s_img_dsc.header.w      = data->image_w;
            s_img_dsc.header.h      = data->image_h;
            s_img_dsc.header.stride = (uint32_t)data->image_w * 2;
            lv_image_set_src(s_w.img_hero, &s_img_dsc);
        } else {
            ESP_LOGW(TAG, "PSRAM alloc failed for %ux%u frame",
                     (unsigned)data->image_w, (unsigned)data->image_h);
        }
        /* The client sets new_image on swap and documents that the UI clears it
         * under the lock once the src is re-bound (octoprint_client.h). The
         * caller holds that lock for us; const is cast away for this one flag. */
        ((octoprint_data_t *)data)->new_image = false;
    }

    bool have = (s_img_copy != NULL);
    set_hidden(s_w.img_hero, !have);
    set_hidden(s_w.img_placeholder, have);
}

void octoprint_page_free_image(void)
{
    if (!s_img_copy) {
        return;
    }
    free_img_copy();
    /* The hero has nothing to show now; leave the page in its placeholder state
     * so a re-entry before the first poll does not flash an empty image slot. */
    set_hidden(s_w.img_hero, true);
    set_hidden(s_w.img_placeholder, false);
}

/** Human state text for the header line. */
static const char *state_text(const octoprint_data_t *data)
{
    if (data->error) {
        return "ERROR";
    }
    if (data->paused) {
        return "PAUSED";
    }
    if (data->printing) {
        return "PRINTING";
    }
    if (data->printer_state[0] != '\0') {
        return data->printer_state;
    }
    if (data->job_state[0] != '\0') {
        return data->job_state;
    }
    return "--";
}

/* ── Page update ──────────────────────────────────────────────────────── */

void octoprint_page_update(const octoprint_data_t *data)
{
    if (!s_root || !data) {
        return;
    }

    const app_config_t *cfg = app_config_get();

    /* Overlay states, mirroring json_page_update: page-specific because they
     * read the configured URL and the client's connectivity flags. */
    if (cfg->octoprint_url[0] == '\0') {
        empty_show("OctoPrint Not Configured");
        return;
    }
    if (!data->data_valid || !data->connected) {
        empty_show("Cannot Reach OctoPrint");
        return;
    }
    empty_hide();

    char buf[48];

    /* -- progress ------------------------------------------------------- */
    float pct = data->completion;   /* already percent 0..100 (live-verified) */
    if (pct >= 0.0f) {
        if (pct > 100.0f) {
            pct = 100.0f;
        }
        snprintf(buf, sizeof(buf), "%.1f", (double)pct);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    set_txt(s_w.lbl_pct, buf);

    int arc_val = (pct >= 0.0f) ? (int)(pct * 10.0f) : 0;
    if (s_w.arc_completion) {
        lv_arc_set_value(s_w.arc_completion, arc_val);
    }
    if (s_w.bar_progress) {
        lv_bar_set_value(s_w.bar_progress, arc_val, LV_ANIM_OFF);
    }

    /* -- layer (DisplayLayerProgress) ------------------------------------ */
    set_hidden(s_w.layer_cell, !data->dlp_available);
    if (data->dlp_available) {
        if (data->layer_current >= 0) {
            snprintf(buf, sizeof(buf), "%d", data->layer_current);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        set_txt(s_w.lbl_layer_cur, buf);

        if (data->layer_total >= 0) {
            snprintf(buf, sizeof(buf), "/ %d", data->layer_total);
        } else {
            snprintf(buf, sizeof(buf), "/ --");
        }
        set_txt(s_w.lbl_layer_total, buf);

        int on = 0;
        if (data->layer_total > 0 && data->layer_current >= 0) {
            on = (int)(((float)data->layer_current * OCTO_LAYER_SEGS)
                       / (float)data->layer_total + 0.5f);
            if (on > OCTO_LAYER_SEGS) {
                on = OCTO_LAYER_SEGS;
            }
        }
        if (on != s_segs_on_cached) {
            uint32_t on_c  = col_accent();
            uint32_t off_c = col_border();
            for (int i = 0; i < OCTO_LAYER_SEGS; i++) {
                if (s_w.layer_segs[i]) {
                    lv_obj_set_style_bg_color(s_w.layer_segs[i],
                                              lv_color_hex(i < on ? on_c : off_c), 0);
                }
            }
            s_segs_on_cached = on;
        }
    }

    /* -- temperatures ----------------------------------------------------- */
    temp_update(&s_w.nozzle, data->nozzle_actual, data->nozzle_target);
    temp_update(&s_w.bed, data->bed_actual, data->bed_target);

    /* -- times ------------------------------------------------------------ */
    fmt_duration(data->print_time_s, buf, sizeof(buf));
    set_txt(s_w.lbl_elapsed, buf);
    fmt_duration(data->print_time_left_s, buf, sizeof(buf));
    set_txt(s_w.lbl_remaining, buf);

    /* Finish time comes from the DLP plugin only. */
    set_hidden(s_w.finish_cell, !data->dlp_available);
    if (data->dlp_available) {
        set_txt(s_w.lbl_finish, data->eta[0] ? data->eta : "--");
    }

    /* -- identity / state -------------------------------------------------- */
    set_txt(s_w.lbl_state, state_text(data));

    bool closed = (strcasecmp(data->conn_state, "Closed") == 0);
    if (s_w.state_dot) {
        uint32_t c = data->error ? col_alert() : (data->printing ? col_accent() : col_label());
        lv_obj_set_style_bg_color(s_w.state_dot, lv_color_hex(c), 0);
    }

    /* /api/connection mirrors the printer state ("Printing"/"Operational") while
     * things are healthy, which just duplicates the job-state chip. Surface the
     * connection element only when it says something the state chip cannot: the
     * link to the printer is down. Unknown/empty state stays hidden. */
    bool conn_error = (strncasecmp(data->conn_state, "Error", 5) == 0);
    bool conn_show  = closed || conn_error;
    if (conn_show) {
        set_txt(s_w.lbl_conn, conn_error ? data->conn_state : "PRINTER OFFLINE");
        if (s_w.lbl_conn) {
            lv_obj_set_style_text_color(s_w.lbl_conn, lv_color_hex(col_alert()), 0);
        }
        if (s_w.conn_dot) {
            lv_obj_set_style_bg_color(s_w.conn_dot, lv_color_hex(col_alert()), 0);
        }
    }
    set_hidden(s_w.conn_chip, !conn_show);
    set_hidden(s_w.lbl_conn, !conn_show);

    /* Fault strip: real error wins, then a closed serial connection (OctoPrint
     * is up but the printer is off), else a muted "no faults" resting state. */
    bool fault = (data->error_text[0] != '\0') || data->error || closed;
    if (data->error_text[0] != '\0') {
        set_txt(s_w.lbl_error, data->error_text);
    } else if (closed) {
        set_txt(s_w.lbl_error, "Printer disconnected");
    } else if (data->error) {
        set_txt(s_w.lbl_error, "Printer error");
    } else {
        set_txt(s_w.lbl_error, "No faults");
    }
    if (s_w.lbl_error) {
        lv_obj_set_style_text_color(s_w.lbl_error,
                                    lv_color_hex(fault ? col_alert() : col_label()), 0);
    }
    if (s_w.error_dot) {
        lv_obj_set_style_bg_color(s_w.error_dot,
                                  lv_color_hex(fault ? col_alert() : col_label()), 0);
    }

    /* -- image ------------------------------------------------------------- */
    image_update(data);
}

/* ── Theme ────────────────────────────────────────────────────────────── */

/**
 * Re-resolve every colour the live widget tree can be re-styled in place.
 * Does NOT cover colours a layout baked in at build time (gradients, per-widget
 * one-off tints) -- octoprint_page_apply_theme() rebuilds for those.
 */
static void apply_styles(void)
{
    styles_update();
    lv_obj_report_style_change(&octo_style_card);
    lv_obj_report_style_change(&octo_style_label);
    lv_obj_report_style_change(&octo_style_value);
    lv_obj_report_style_change(&octo_style_accent);

    lv_obj_set_style_bg_color(s_root, lv_color_hex(col_bg()), 0);
    if (s_backdrop) {
        lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(col_bg()), 0);
    }
    nina_empty_state_apply_theme(s_empty, current_theme, cfg_brightness());

    /* Widgets whose colours are not carried by a shared style. */
    if (s_w.arc_completion) {
        lv_obj_set_style_arc_color(s_w.arc_completion, lv_color_hex(col_border()),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_w.arc_completion, lv_color_hex(col_accent()),
                                   LV_PART_INDICATOR);
    }
    if (s_w.bar_progress) {
        lv_obj_set_style_bg_color(s_w.bar_progress, lv_color_hex(col_bg()), 0);
        lv_obj_set_style_border_color(s_w.bar_progress, lv_color_hex(col_border()), 0);
        lv_obj_set_style_bg_color(s_w.bar_progress, lv_color_hex(col_accent()),
                                  LV_PART_INDICATOR);
    }

    octo_temp_el_t *temps[2] = { &s_w.nozzle, &s_w.bed };
    for (int i = 0; i < 2; i++) {
        octo_temp_el_t *t = temps[i];
        if (t->bar) {
            lv_obj_set_style_bg_color(t->bar, lv_color_hex(col_bg()), 0);
            lv_obj_set_style_border_color(t->bar, lv_color_hex(col_border()), 0);
            temp_bar_gradient(t->bar, t->hot);
        }
        if (t->tick) {
            lv_obj_set_style_bg_color(t->tick, lv_color_hex(col_text()), 0);
        }
    }

    /* conn_chip / error_strip are deliberately NOT restyled here. octo_w_chip()
     * already paints cardbg + border at build time, and this function only ever
     * runs immediately after build_content(), so the rewrite was redundant --
     * and it clobbered layouts that restyle the chip themselves (the Immersive
     * image layout turns the fault chip into a gradient fade pane in the page
     * ground colour, which a bg_color rewrite half-undid). */

    /* Segment strip is repainted on the next update; drop the cache. */
    s_segs_on_cached = -1;
}

/* ── Refresh (layout / theme change) ──────────────────────────────────── */

/**
 * Tear the content down and build it again under the current config and theme.
 *
 * The root object stays alive so nina_dashboard's stored page pointer remains
 * valid (same approach as nina_tile_grid_refresh_config). The empty-state
 * backdrop is a separate FLOATING child of the root, survives the teardown and
 * is re-raised afterwards.
 */
static void rebuild_in_place(void)
{
    free_img_copy();

    if (s_content) {
        lv_obj_delete(s_content);
        s_content = NULL;
    }
    build_content();   /* clears s_w first: its pointers named the deleted tree */

    if (s_backdrop) {
        lv_obj_move_foreground(s_backdrop);
    }
    apply_styles();
}

void octoprint_page_refresh_config(void)
{
    if (!s_root) {
        return;
    }
    rebuild_in_place();
}

void octoprint_page_apply_theme(void)
{
    /* Every layout resolves its decoration colours (gradients, dial rings, tint
     * washes) once at build time, so a style pass alone leaves them on the old
     * palette. Rebuilding is the only way to re-derive all of them -- cheap
     * enough for a theme change, and skipped entirely when the page was never
     * built. */
    if (!s_root) {
        return;
    }
    rebuild_in_place();
}

lv_obj_t *octoprint_page_get_obj(void)
{
    return s_root;
}
