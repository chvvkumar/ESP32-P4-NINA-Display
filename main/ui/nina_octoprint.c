/**
 * @file nina_octoprint.c
 * @brief OctoPrint 3D Printer page — shared widget library + update path.
 *
 * This file owns EVERYTHING that is not geometry: the shared styles, the widget
 * factories, the layout dispatch table, the update path, the image handoff, the
 * empty state and the theme application. The four octoprint_layout_*.c files
 * only arrange widgets (see nina_octoprint_internal.h for the seam contract).
 *
 * Locking: the caller of octoprint_page_update() holds octoprint_data_t::mutex
 * ONLY; this module takes the LVGL display lock itself, inside that (lock
 * order: client lock OUTSIDE, display lock INSIDE, never reversed). The split
 * exists so the bilinear resample (~300-500k px per webcam frame) runs under
 * the client lock alone and never stalls the flush task. Every other entry
 * point (create / refresh / theme / free_image) still runs with the display
 * lock held by the CALLER, unchanged.
 *
 * Image lifetime: octoprint_client frees the retired frame shortly after it
 * releases the mutex, so binding an lv_image straight at data->image_buf would
 * leave the LVGL flush task rendering from a freed pointer between two updates.
 * We therefore stage a UI-owned PSRAM copy under the client lock (stage_image),
 * then commit it — descriptor rewrite + lv_image_set_src rebind + pointer swap,
 * the cheap part — under the display lock, and free the retired copy after
 * unlocking. The copy is produced by the bilinear scaler at the hero's exact
 * pixel box -- fitted inside it, or sized to cover it when the layout sets
 * octoprint_layout_ops_t::image_cover -- so the descriptor binds 1:1 and LVGL
 * never runs its software transform (which is what put the horizontal seams on
 * the hero); a cover-sized copy is cropped by the hero's own clip. It is re-made
 * only when the frame or the box changes, not per update. The target box is the
 * one cached by the previous commit pass (reading LVGL layout needs the display
 * lock); a first frame or a moved box costs one skipped update cycle, not a
 * scale under the display lock.
 */

#include "nina_octoprint.h"
#include "nina_octoprint_internal.h"
#include "nina_empty_state.h"
#include "app_config.h"
#include "display_defs.h"
#include "page_conn.h"
#include "jpeg_utils.h"
#include "bsp/esp-bsp.h"   /* bsp_display_lock / bsp_display_unlock */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "octo_ui";

#define OCTO_PAD   16   /* outer padding, matches OUTER_PADDING */
#define OCTO_GAP   12   /* inter-card gap (mockup v1) */
#define OCTO_W     (SCREEN_SIZE - 2 * OCTO_PAD)

/* ── Shared styles ────────────────────────────────────────────────────── */

static lv_style_t octo_style_card;   /* file-local: layouts get cards via octo_w_card() */
lv_style_t octo_style_label;
lv_style_t octo_style_value;
lv_style_t octo_style_accent;
static bool s_styles_ready = false;

/* Readings-over-picture visibility. RUNTIME state, deliberately not config: a
 * tap on the page flips it for this session only, while a config save calls
 * octoprint_page_set_overlay_visible() and re-asserts the stored value. Seeded
 * from app_config at page create; a rebuild (theme or layout change) keeps
 * whatever the user last tapped. */
static bool s_readings_shown = true;

/* ── Page state ───────────────────────────────────────────────────────── */

static lv_obj_t *s_root      = NULL;  /* survives refresh_config */
static lv_obj_t *s_content   = NULL;  /* deleted + rebuilt by refresh_config */
static lv_obj_t *s_backdrop  = NULL;  /* full-cover host for the empty state */
static lv_obj_t *s_empty     = NULL;
static octoprint_widgets_t s_w;

/* Current opacity of s_content. Dimmed while the source is STALE (last values
 * kept on screen), full while it is OK. Tracked so the update path only
 * restyles on a transition -- a per-cycle lv_obj_set_style_opa() would
 * invalidate the whole 720x720 frame under FULL_REFRESH. */
static lv_opa_t s_content_opa = LV_OPA_COVER;

/* UI-owned, bilinear-scaled copy of the decoded frame (see file header).
 * s_img_copy is a 128-byte-aligned PSRAM buffer; s_fit_w/h are the pixel
 * dimensions currently held in it. All four are display-lock domain. */
static uint8_t       *s_img_copy = NULL;
static uint16_t       s_fit_w    = 0;
static uint16_t       s_fit_h    = 0;
static lv_image_dsc_t s_img_dsc;

/* Which octoprint_image_source produced s_img_copy (-1 = none). Display-lock
 * domain. Needed because stage_image() consumes (frees) the client's webcam
 * original after scaling, so a NULL data->image_buf is ambiguous: "frame
 * dropped" vs "frame already shown". */
static int8_t s_copy_src = -1;

/* Hero content box cached by the last commit pass, and the rescale request
 * flag. Written under the display lock; stage_image() reads them WITHOUT it
 * (deliberate, benign race: a stale box stages a wrong-sized copy that the
 * commit pass detects and re-flags, costing one extra update cycle). */
static uint16_t s_box_w        = 0;
static uint16_t s_box_h        = 0;
static bool     s_need_rescale = true;

/* Scaling mode of the active layout (octoprint_layout_ops_t::image_cover):
 * true = scale to COVER the hero box and let the host crop the excess, false =
 * aspect-FIT. Written by build_content() ONLY -- it is a property of the
 * layout, not of the frame, so free_img_copy() must not touch it (that call
 * also happens on page-leave, where the layout is unchanged). Every path that
 * can change the layout (create, refresh_config, apply_theme) goes through
 * build_content() after a free_img_copy(), so the mode and the held copy are
 * always re-derived together. Read by stage_image() without the display lock,
 * same benign race as s_box_w/h: one wrong-mode copy that the commit pass
 * detects and re-flags. */
static bool s_img_cover = false;

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
    if (!obj || !text) {
        return;
    }
    /* Dirty guard (precedent: nina_empty_state.c): with FULL_REFRESH every
     * label invalidation repaints the whole 720x720 frame, so skip the set
     * when the text is unchanged. */
    const char *cur = lv_label_get_text(obj);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(obj, text);
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

static void set_content_opa(lv_opa_t opa)
{
    if (!s_content || s_content_opa == opa) {
        return;
    }
    s_content_opa = opa;
    lv_obj_set_style_opa(s_content, opa, 0);
}

/**
 * What the image slot says while it holds no frame. One line per reason the
 * client could not give us a picture, so the user is told which of the four it
 * is instead of a bare "NO IMAGE". PENDING (and OK, whose placeholder is
 * hidden anyway) reads as "loading", worded for the source that is actually
 * being fetched.
 */
static const char *placeholder_text(uint8_t status)
{
    switch (status) {
        case OCTO_IMG_WEBCAM_FAILED: return "Webcam unavailable";
        case OCTO_IMG_NO_THUMBNAIL:  return "No preview for this file";
        case OCTO_IMG_NO_JOB:        return "No job loaded";
        default:                     break;
    }
    return (app_config_get()->octoprint_image_source == 1)
           ? "Loading webcam..."
           : "Loading G-code preview...";
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
    s_img_copy = NULL;
    s_fit_w    = 0;
    s_fit_h    = 0;
    s_copy_src = -1;
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    /* The cached box may belong to a tree this call is tearing down
     * (rebuild_in_place), so invalidate it; the next commit pass re-measures.
     * Costs one update cycle of staging latency after a page re-entry. */
    s_box_w        = 0;
    s_box_h        = 0;
    s_need_rescale = true;
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

/** Push s_readings_shown onto the live layer. No-op for a layout without one. */
static void apply_readings_visibility(void)
{
    if (!s_w.overlay_layer) {
        return;
    }
    if (s_readings_shown) {
        lv_obj_remove_flag(s_w.overlay_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_w.overlay_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *octo_w_overlay_layer(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *layer = lv_obj_create(page);
    lv_obj_remove_style_all(layer);
    /* FLOATING: the layout root is a flex column, and the layer must sit on top
     * of it at (0,0) rather than take a slot in the flow (same trick as the
     * empty-state backdrop). */
    lv_obj_add_flag(layer, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    /* NOT clickable, so a tap over the readings hit-tests through to the page
     * root that owns the toggle handler. GESTURE_BUBBLE stays at its default so
     * page swipes still reach the dashboard. */
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(layer, LV_LAYOUT_NONE);
    lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(layer, 0, 0);

    w->overlay_layer = layer;
    return layer;
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

void octo_label_shadow(lv_obj_t *lbl)
{
    if (!lbl) {
        return;
    }
    /* Radius 0 = hard offset copy, no blur (the blur task is skipped outright
     * at lv_draw_blur() when the radius is 0). Setting the same values twice is
     * a no-op, which is what makes this idempotent. */
    lv_obj_set_style_drop_shadow_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_drop_shadow_opa(lbl, OCTO_SHADOW_OPA, 0);
    lv_obj_set_style_drop_shadow_radius(lbl, 0, 0);
    lv_obj_set_style_drop_shadow_offset_x(lbl, OCTO_SHADOW_DX, 0);
    lv_obj_set_style_drop_shadow_offset_y(lbl, OCTO_SHADOW_DY, 0);
    /* Make room for the offset copy inside the label's OWN coords. A
     * LV_LABEL_LONG_CLIP label clips its draw to the text area, and a tight
     * SIZE_CONTENT parent clips its children to bare coords, so without this
     * padding the shadow is trimmed off the right and bottom edges. */
    lv_obj_set_style_pad_right(lbl, OCTO_SHADOW_DX, 0);
    lv_obj_set_style_pad_bottom(lbl, OCTO_SHADOW_DY, 0);
}

int32_t octo_text_width(const lv_font_t *font, const char *sample)
{
    lv_point_t sz = { 0, 0 };
    if (!font || !sample) {
        return 0;
    }
    lv_text_get_size(&sz, sample, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
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

    /* TILE and COMPACT stack caption over value; BAR_GRADIENT puts them on one
     * line. COMPACT differs from TILE only in the value font and format (the
     * current reading alone, coloured by heat stage in the update path). */
    bool stacked = (variant == OCTO_TEMP_TILE || variant == OCTO_TEMP_COMPACT);

    lv_obj_t *root = octo_w_row(parent, false, stacked ? 2 : 6);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    out->root = root;

    if (variant == OCTO_TEMP_COMPACT) {
        lv_obj_set_size(root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        out->lbl_name  = octo_w_label(root, name, &lv_font_montserrat_14,
                                      &octo_style_label);
        out->lbl_value = octo_w_label(root, "--", &lv_font_montserrat_bold_22,
                                      &octo_style_value);
        return root;
    }

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
    /* CENTER, not CONTAIN: stage_image() bilinear-scales the frame to this
     * widget's exact box before binding, so the aspect fit is already done and
     * any LVGL zoom here would only re-introduce the software transform (and its
     * horizontal seams). A FIT layout binds a frame no larger than the box
     * (except transiently for one cycle after the box shrinks); a COVER layout
     * (octoprint_layout_ops_t::image_cover) deliberately binds a LARGER one and
     * relies on CENTER to show its middle. Either way the scale stays
     * LV_SCALE_NONE and the overhang is clipped to the widget's own coords
     * before the draw (lv_refr.c lv_obj_redraw(), which truncates the layer clip
     * to obj->coords + ext_draw_size, and ext_draw_size is 0 without a
     * transform), so nothing bleeds outside the hero. */
    lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(w->img_hero);
    lv_obj_add_flag(w->img_hero, LV_OBJ_FLAG_HIDDEN);

    /* Born reading "Loading ...": a freshly built page has never had a frame,
     * and the poller is on its way with one. "NO IMAGE" read as a verdict. */
    w->img_placeholder = octo_w_label(host, placeholder_text(OCTO_IMG_PENDING),
                                      &lv_font_montserrat_16, &octo_style_label);
    lv_obj_center(w->img_placeholder);
    return host;
}

/* -- chips / strips ----------------------------------------------------- */

/**
 * Dot + text chip (file-local). Used for the connection indicator and the fault
 * strip; the geometry is identical so nothing reflows when a fault appears.
 */
static lv_obj_t *octo_w_chip(lv_obj_t *parent, const char *text,
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
    /* Born empty and hidden. A healthy printer has nothing to report, so the
     * strip earns its space only when the update path has a fault or an offline
     * printer to show. Layouts still lay it out normally -- they need no changes
     * for this; visibility is driven solely from octoprint_page_update(). */
    w->error_strip = octo_w_chip(parent, "", &w->error_dot, &w->lbl_error);
    lv_obj_add_flag(w->error_strip, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);   /* let taps on the track reach the page root (readings toggle) */
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
                                * it and it resolves to Grid. Reserved, so the
                                * later slots never renumber. */
    &octoprint_layout_glass,
    &octoprint_layout_bento,   /* retired alias: slot 3 was "Large numerals".
                                * Renders Grid; slot reserved, NVS value legal. */
    &octoprint_layout_bento,   /* retired alias: slot 4 was "Layer timeline".
                                * Renders Grid; slot reserved, NVS value legal. */
    &octoprint_layout_overlay,   /* slot 5: "Floating overlay" */
    &octoprint_layout_letterbox, /* slot 6: "Letterbox"        */
};

static void build_content(void)
{
    memset(&s_w, 0, sizeof(s_w));

    uint8_t idx = app_config_get()->octoprint_layout;
    if (idx >= OCTO_LAYOUT_COUNT) {
        idx = 0;
    }
    const octoprint_layout_ops_t *ops = s_layouts[idx];

    /* Canvas first: a full-bleed layout gets the whole screen, shifted up and
     * left to negate main_cont's OUTER_PADDING (the same negation the image,
     * clock and Spotify pages use). Set on every build, so switching layouts at
     * runtime restores the inset one as well. */
    /* Scaling mode for the image path. Set on every build, so switching layouts
     * at runtime re-stages with the other mode (rebuild_in_place() drops the
     * held copy and re-flags a rescale just before this runs). */
    s_img_cover = ops->image_cover;

    int size = ops->full_bleed ? SCREEN_SIZE : OCTO_W;
    int off  = ops->full_bleed ? -OCTO_PAD : 0;
    lv_obj_set_size(s_root, size, size);
    lv_obj_set_pos(s_root, off, off);

    s_content = octo_w_row(s_root, false, OCTO_GAP);
    lv_obj_set_size(s_content, LV_PCT(100), LV_PCT(100));
    s_content_opa = LV_OPA_COVER;   /* a fresh tree carries no dim from a stale pass */

    ESP_LOGI(TAG, "Building OctoPrint page, layout %u (%s)",
             (unsigned)idx, ops->name ? ops->name : "?");
    ops->build(s_content, &s_w);

    /* The fresh tree carries no visibility state; re-assert the runtime one, so
     * a theme or layout change does not undo a tap toggle. */
    apply_readings_visibility();
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

/**
 * Tap anywhere on the page toggles the readings layer. No-op on a layout that
 * built no layer (Grid).
 *
 * LVGL still emits LV_EVENT_CLICKED on release after a swipe (indev_proc_release
 * suppresses it only when a SCROLL claimed the press, and this page is not
 * scrollable), so the gesture direction latched by indev_gesture() -- it is
 * cleared on the NEXT press, not on release -- is what tells a page swipe from a
 * tap. Without this guard every page swipe would also toggle the readings.
 */
static void page_tap_cb(lv_event_t *e)
{
    (void)e;
    if (!s_w.overlay_layer) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) {
        return;   /* that was a page swipe, not a tap */
    }
    s_readings_shown = !s_readings_shown;
    apply_readings_visibility();
}

void octoprint_page_set_overlay_visible(bool visible)
{
    /* Takes the display lock itself: called from the config side-effect path,
     * which does not hold it. Remembered even before the page exists -- the
     * next build_content() applies it. */
    s_readings_shown = visible;
    if (!s_root) {
        return;
    }
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        apply_readings_visibility();
        bsp_display_unlock();
    }
}

lv_obj_t *octoprint_page_create(lv_obj_t *parent)
{
    styles_update();
    /* Seed the runtime readings state from config; taps flip it afterwards
     * without persisting. */
    s_readings_shown = app_config_get()->octoprint_overlay_visible;
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
    /* Readings toggle. Registered on the ROOT (clickable by default) and not on
     * the layer: the layer and every library-built container are non-clickable,
     * so taps over them hit-test straight through to here. Registered once --
     * the root survives rebuild_in_place(). */
    lv_obj_add_event_cb(s_root, page_tap_cb, LV_EVENT_CLICKED, NULL);

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

/**
 * Heating stage derived from one tool's actual/target pair. Single precision
 * only (the P4 FPU has no double unit). @p has_t is false when the heater is
 * off; an overshoot above a live target still reads as HOLDING.
 */
static octo_heat_stage_t octo_heat_stage(float actual, float target,
                                         bool has_a, bool has_t)
{
    if (!has_a) {
        return OCTO_HEAT_IDLE;
    }
    if (has_t) {
        return (actual < target - OCTO_TEMP_HOLD_BAND_C) ? OCTO_HEAT_HEATING
                                                         : OCTO_HEAT_HOLDING;
    }
    return (actual > OCTO_TEMP_COLD_LINE_C) ? OCTO_HEAT_COOLING : OCTO_HEAT_IDLE;
}

static void temp_update(octo_temp_el_t *t, float actual, float target)
{
    if (!t || !t->root) {
        return;
    }

    char buf[48];
    bool has_a = !isnan(actual);
    bool has_t = !isnan(target) && target > 0.0f;
    if (t->variant == OCTO_TEMP_COMPACT) {
        /* Current reading alone: "23.8°" — no target, no unit letter. */
        if (has_a) {
            snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", (double)actual);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        set_txt(t->lbl_value, buf);

        /* Heat stage recolours the value with a LOCAL style, which outranks the
         * shared octo_style_value it also carries. */
        uint32_t col;
        switch (octo_heat_stage(actual, target, has_a, has_t)) {
        case OCTO_HEAT_HEATING:
            col = octo_color(t->hot ? OCTO_COL_ALERT : OCTO_COL_HOT);
            break;
        case OCTO_HEAT_HOLDING:
            col = octo_color(OCTO_COL_TEXT);
            break;
        case OCTO_HEAT_COOLING:
            col = octo_color(OCTO_COL_ACCENT);
            break;
        case OCTO_HEAT_IDLE:
        default:
            col = octo_color(OCTO_COL_LABEL);
            break;
        }
        if (t->lbl_value) {
            lv_obj_set_style_text_color(t->lbl_value, lv_color_hex(col), 0);
        }
        return;   /* no bar, no tick */
    }
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
        return;   /* TILE: the value line is the whole element */
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

/**
 * Scale @p src_w x @p src_h to the @p bw x @p bh box, keeping the aspect: FIT
 * (whole frame inside the box, bands) when @p cover is false, COVER (box filled
 * in both axes, excess cropped by the host) when it is true. Pure arithmetic,
 * no LVGL reads, so it is callable with or without the display lock. Returns
 * false for a degenerate box (< 8 px: unlaid) or source.
 *
 * Idempotent in BOTH modes: re-running it on its own output for the same box
 * returns that output unchanged. The commit pass relies on that to detect a
 * moved box without knowing the source dimensions. It holds because the
 * limiting axis is assigned the box edge exactly rather than computed, so the
 * second pass scales by exactly 1.
 *
 * COVER guard: a very lopsided source (a tall thumbnail) would cover a 720
 * square with an enormous copy, so anything past 2 * SCREEN_SIZE on either axis
 * falls back to FIT for that frame. The decision depends only on the aspect and
 * the box, never on the size, so the fallback is idempotent too: re-running
 * COVER on a fitted copy of the same aspect trips the same guard and returns
 * the fit size unchanged.
 */
static bool fit_into_box(uint16_t src_w, uint16_t src_h, int32_t bw, int32_t bh,
                         bool cover, uint16_t *out_w, uint16_t *out_h)
{
    if (bw < 8 || bh < 8 || src_w == 0 || src_h == 0) {
        return false;
    }
    if (bw > SCREEN_SIZE) {
        bw = SCREEN_SIZE;
    }
    if (bh > SCREEN_SIZE) {
        bh = SCREEN_SIZE;
    }

    if (cover) {
        /* Scale by max(bw/src_w, bh/src_h): the axis that needs the LARGER
         * factor lands exactly on the box edge, the other overhangs. */
        int32_t cw, ch;
        if ((int32_t)bw * (int32_t)src_h >= (int32_t)bh * (int32_t)src_w) {
            cw = bw;
            ch = ((int32_t)src_h * bw + src_w / 2) / src_w;
            if (ch < bh) {
                ch = bh;   /* rounding must never leave a gap */
            }
        } else {
            ch = bh;
            cw = ((int32_t)src_w * bh + src_h / 2) / src_h;
            if (cw < bw) {
                cw = bw;
            }
        }
        if (cw <= 2 * SCREEN_SIZE && ch <= 2 * SCREEN_SIZE) {
            *out_w = (uint16_t)cw;
            *out_h = (uint16_t)ch;
            return true;
        }
        static bool cover_clamped = false;
        if (!cover_clamped) {
            cover_clamped = true;
            ESP_LOGW(TAG, "hero cover %ux%u -> %dx%d too large, using fit",
                     (unsigned)src_w, (unsigned)src_h, (int)cw, (int)ch);
        }
        /* fall through to FIT */
    }

    /* Width-limited when the source is relatively wider than the box. */
    int32_t w, h;
    if ((int32_t)src_w * bh >= (int32_t)src_h * bw) {
        w = bw;
        h = ((int32_t)src_h * bw + src_w / 2) / src_w;
    } else {
        h = bh;
        w = ((int32_t)src_w * bh + src_h / 2) / src_h;
    }
    /* Clamp both ways: never past the box (CENTER would clip), never degenerate. */
    if (w > bw) {
        w = bw;
    }
    if (h > bh) {
        h = bh;
    }
    if (w < 2) {
        w = 2;
    }
    if (h < 2) {
        h = 2;
    }
    *out_w = (uint16_t)w;
    *out_h = (uint16_t)h;
    return true;
}

/**
 * Frame staged by stage_image() under the client lock, committed (or
 * discarded) by image_update() under the display lock. A local of
 * octoprint_page_update(); never outlives one call.
 */
typedef struct {
    uint8_t *buf;       /* 128-aligned PSRAM RGB565, w*h; NULL = nothing staged */
    uint16_t w, h;
    int8_t   src;       /* octoprint_image_source that produced it */
    bool     consumed;  /* set on commit: the caller must not free buf */
} octo_staged_t;

/**
 * Phase A of the image path: produce the bilinear-scaled copy WITHOUT the
 * display lock, so the resample never stalls the flush task. Runs under the
 * client lock only. The target size comes from the hero box cached by the
 * previous commit pass; image_update() verifies that box under the display
 * lock and re-flags a rescale if it moved. A first frame (box not yet cached)
 * stages nothing and is scaled on the NEXT update cycle -- deliberately the
 * "skip a frame" option, chosen over a one-off scale under the display lock
 * because it keeps a single code path (~2 s of placeholder at worst).
 *
 * Bilinear resample, once, into the hero's exact box, so the descriptor binds
 * 1:1 and LVGL never runs its software transform per draw-unit band (which is
 * what showed as repeating horizontal seams -- PPA only accelerates 1.0x, so a
 * zoomed lv_image is always the software path).
 *
 * Software, not ppa_scale_rgb565_into(): on device the SRM path both mirrors
 * the frame vertically and resamples by pixel drop/duplicate, which banded the
 * hero worse than LVGL's own zoom. The GOES/moon callers are calibrated around
 * that behaviour, so it is left alone.
 *
 * Row order: jpeg_sw_decode_rgb565 converts stb output TOP-DOWN, so a straight
 * copy renders upright. NO row reversal belongs on this path. Every past
 * "mirrored with a straight copy" sighting was stb's flip-on-load flag reading
 * uninitialized TLS garbage on this task (proven on-device 2026-08-14; see
 * STBI_NO_THREAD_LOCALS in stb_image.c), which vertically mirrored decodes per
 * task, per build. With that fixed, decode polarity is deterministic. Before
 * changing orientation handling in ANY direction, re-run the mock test-pattern
 * + /api/screenshot procedure on a fullclean build (see tests/octoprint_mock,
 * fixture .orig backups).
 */
static void stage_image(octoprint_data_t *data, octo_staged_t *st)
{
    memset(st, 0, sizeof(*st));
    st->src = -1;

    if (!data->image_buf || data->image_w == 0 || data->image_h == 0) {
        return;
    }
    if (data->image_src < 0) {
        /* A frame with no source label is a frame we cannot honestly draw:
         * staging it would have to guess the label from live config, which is
         * exactly what mislabels an old frame during a source toggle. */
        return;
    }
    if (data->image_src != ((app_config_get()->octoprint_image_source == 1) ? 1 : 0)) {
        /* A frame labelled for the other source is never drawn, even if a poll
         * pass outran the switch. */
        return;
    }
    page_conn_t conn = page_conn_eval(data->data_valid, data->connected,
                                      (int)data->fail_count);
    if (conn != PAGE_CONN_OK && conn != PAGE_CONN_STALE) {
        /* The page is showing the empty overlay and the commit pass will not
         * run, so a scale now would be redone every cycle until reconnect.
         * STALE still renders (dimmed last values), so it still stages. */
        return;
    }
    if (!data->new_image && !s_need_rescale) {
        return;   /* current copy still matches this frame and box */
    }

    uint16_t fit_w = 0, fit_h = 0;
    if (!fit_into_box(data->image_w, data->image_h,
                      (int32_t)s_box_w, (int32_t)s_box_h, s_img_cover,
                      &fit_w, &fit_h)) {
        return;   /* hero box not cached yet -- scale on the next cycle */
    }

    /* 128-byte-aligned address AND size: not required by the software scale,
     * but it is one call and it keeps the buffer on L2 cache lines.
     * A COVER layout pays for the cropped-off margin: a 4:3 webcam covering the
     * 720 square is 960x720x2 = 1.38 MB against 720x540x2 = 1.04 MB fitted,
     * about 0.35 MB more PSRAM per staged copy (twice that for the one cycle
     * where the retired copy is still held). */
    size_t need = ((size_t)fit_w * (size_t)fit_h * 2u + 127u) & ~(size_t)127u;
    uint8_t *buf = heap_caps_aligned_calloc(128, 1, need, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed for %ux%u hero frame",
                 (unsigned)fit_w, (unsigned)fit_h);
        return;   /* new_image stays set, so the next cycle retries */
    }

    int64_t t0 = esp_timer_get_time();
    sw_scale_rgb565_bilinear((const uint16_t *)data->image_buf,
                             (int)data->image_w, (int)data->image_h,
                             (uint16_t *)buf, (int)fit_w, (int)fit_h);
    ESP_LOGD(TAG, "hero scale %ux%u -> %ux%u in %lld us",
             (unsigned)data->image_w, (unsigned)data->image_h,
             (unsigned)fit_w, (unsigned)fit_h,
             esp_timer_get_time() - t0);

    st->buf = buf;
    st->w   = fit_w;
    st->h   = fit_h;
    /* The label comes off the FRAME, never off live config: during a source
     * toggle config already says "webcam" while this buffer is still the
     * G-code preview, and tagging it 1 would both draw it as the webcam and
     * free the client's retained original as if it were one. */
    st->src = data->image_src;

    /* The webcam refetches every poll, so the client's retained full-res
     * original (~600 KB PSRAM at 640x480) is pure double-buffering once it has
     * been scaled: take ownership and free it here, under the client lock (no
     * other reader can hold it). The thumbnail original stays with the client
     * on purpose -- it is what a layout change re-fits from without a refetch.
     * If the commit is then skipped (display-lock timeout, moved box), the
     * webcam frame is simply lost; the next poll fetches a fresh one. */
    if (st->src == 1) {
        heap_caps_free(data->image_buf);
        data->image_buf = NULL;
        data->image_w   = 0;
        data->image_h   = 0;
    }
}

/**
 * Phase B of the image path: commit (or discard) the staged frame and refresh
 * the box cache. Runs under BOTH locks. The retired copy is handed back via
 * @p out_retired and freed by octoprint_page_update() after the display lock
 * is released.
 */
static void image_update(octoprint_data_t *data, octo_staged_t *st,
                         uint8_t **out_retired)
{
    if (!s_w.img_hero) {
        return;
    }

    bool have_client = data->image_buf && data->image_w != 0 && data->image_h != 0;

    if (!have_client && !st->buf) {
        /* NULL client buffer and nothing staged. Either the client dropped the
         * frame (page left, source switched, nothing decoded yet) -- clear --
         * or stage_image() consumed a webcam original on an earlier pass and
         * the copy on screen is still the selected source -- keep it.
         * PENDING breaks the tie the other way: the source was just switched or
         * the page just came back, so whatever is on screen is the wrong
         * picture and must give way to the loading text.
         *
         * Deliberately NOT gated on image_status == OCTO_IMG_OK: a failed
         * webcam refresh keeps the last good frame on screen (a live view
         * hiccup, not an empty slot -- user ruling 2026-08-15). Only PENDING
         * drops it, and PENDING is what a source switch stamps. Config is read
         * here only because there is no frame left to read a label off -- the
         * client consumed the original -- and the PENDING check is what makes
         * that safe across a toggle. */
        if (s_img_copy && s_copy_src == 1 &&
            app_config_get()->octoprint_image_source == 1 &&
            data->image_status != OCTO_IMG_PENDING) {
            return;
        }
        free_img_copy();
        set_txt(s_w.img_placeholder, placeholder_text(data->image_status));
        set_hidden(s_w.img_hero, true);
        set_hidden(s_w.img_placeholder, false);
        return;
    }

    /* Refresh the box cache for the next stage pass (reads LVGL layout, hence
     * display-lock only). */
    lv_obj_update_layout(s_w.img_hero);
    int32_t bw = lv_obj_get_content_width(s_w.img_hero);
    int32_t bh = lv_obj_get_content_height(s_w.img_hero);
    bool box_known = (bw >= 8 && bh >= 8);
    if (box_known) {
        s_box_w = (uint16_t)((bw > SCREEN_SIZE) ? SCREEN_SIZE : bw);
        s_box_h = (uint16_t)((bh > SCREEN_SIZE) ? SCREEN_SIZE : bh);
    }

    if (st->buf && box_known) {
        /* Commit: unbind, swap the pointer, rebind. Cheap -- the scale already
         * happened. The retired buffer is freed after unlock by the caller. */
        lv_image_set_src(s_w.img_hero, NULL);
        *out_retired = s_img_copy;
        s_img_copy   = st->buf;
        st->consumed = true;
        s_fit_w      = st->w;
        s_fit_h      = st->h;
        s_copy_src   = st->src;

        s_img_dsc.data          = s_img_copy;
        s_img_dsc.data_size     = (uint32_t)st->w * (uint32_t)st->h * 2u;
        s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_img_dsc.header.w      = st->w;
        s_img_dsc.header.h      = st->h;
        s_img_dsc.header.stride = (uint32_t)st->w * 2u;
        lv_image_set_src(s_w.img_hero, &s_img_dsc);

        /* Contract (octoprint_client.h): the UI clears new_image under the
         * client lock once the src is re-bound -- and ONLY then, so a failed
         * stage or commit keeps the flag set and retries. */
        data->new_image = false;

        /* Verify the box the stage pass targeted is still this box: a fit is
         * idempotent, so a changed result means the box moved in between.
         * Commit anyway (a slightly off-size frame beats a lost one; CENTER
         * shows it un-stretched) and rescale on the next cycle. */
        uint16_t vw = 0, vh = 0;
        s_need_rescale = !(fit_into_box(st->w, st->h, bw, bh, s_img_cover,
                                        &vw, &vh) &&
                           vw == st->w && vh == st->h);
    } else if (st->buf) {
        /* Box unlaid: discard (the caller frees) and rescale next cycle. */
        s_need_rescale = true;
    } else if (have_client && s_img_copy && box_known) {
        /* Nothing staged this pass; check the on-screen copy against the box
         * so a layout move re-fits the retained thumbnail. new_image alone
         * already re-triggers the stage pass, no flag needed for it. */
        uint16_t vw = 0, vh = 0;
        if (!(fit_into_box(s_fit_w, s_fit_h, bw, bh, s_img_cover, &vw, &vh) &&
              vw == s_fit_w && vh == s_fit_h)) {
            s_need_rescale = true;
        }
    }

    bool have = (s_img_copy != NULL);
    if (!have) {
        set_txt(s_w.img_placeholder, placeholder_text(data->image_status));
    }
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
     * so a re-entry before the first poll does not flash an empty image slot.
     * The text is the loading line, not a verdict: the client re-arms PENDING on
     * activation and a frame is on its way, so a re-entry must not read as
     * "there is no image" for the second or two before it lands. */
    set_txt(s_w.img_placeholder, placeholder_text(OCTO_IMG_PENDING));
    set_hidden(s_w.img_hero, true);
    set_hidden(s_w.img_placeholder, false);
}

void octoprint_page_image_source_changed(void)
{
    if (!s_root) {
        return;
    }
    /* The held frame belongs to the source the user just switched away from, so
     * it is wrong the instant the toggle moves. Drop it here rather than
     * waiting for the poller: the fetch+decode of the new source takes 1-3 s,
     * and leaving the old picture up for that long reads as a dead control. */
    free_img_copy();
    set_txt(s_w.img_placeholder, placeholder_text(OCTO_IMG_PENDING));
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

/** Widget refresh. Runs under BOTH locks (client outside, display inside). */
static void page_update_locked(octoprint_data_t *data, octo_staged_t *st,
                               uint8_t **out_retired)
{
    const app_config_t *cfg = app_config_get();

    /* Overlay states, mirroring json_page_update: page-specific because they
     * read the configured URL and the client's connectivity flags. */
    if (cfg->octoprint_url[0] == '\0') {
        empty_show("OctoPrint Not Configured");
        nina_empty_state_set_busy(s_empty, false);
        return;
    }

    /* One poll failing is not an outage. The four-state evaluator separates
     * "nothing yet" (busy overlay) from "gave up" (fault overlay) from "had
     * data, currently missing polls" -- which keeps the last readings on screen,
     * dimmed, instead of blanking every card behind a full-page overlay. */
    page_conn_t conn = page_conn_eval(data->data_valid, data->connected,
                                      (int)data->fail_count);
    if (conn == PAGE_CONN_CONNECTING) {
        empty_show("Connecting to OctoPrint...");
        nina_empty_state_set_busy(s_empty, true);
        return;
    }
    if (conn == PAGE_CONN_DOWN) {
        empty_show("Cannot Reach OctoPrint");
        nina_empty_state_set_busy(s_empty, false);
        return;
    }
    empty_hide();
    nina_empty_state_set_busy(s_empty, false);

    bool stale = (conn == PAGE_CONN_STALE);
    set_content_opa(stale ? LV_OPA_60 : LV_OPA_COVER);

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
     * link to the printer is down. Unknown/empty state stays hidden.
     *
     * A stale source borrows the same element for its own cue: the values on
     * screen are the last known good ones and the poller is retrying, which is
     * exactly what this chip exists to say. It outranks the printer-link texts
     * because we cannot trust a link state read from a poll that failed. */
    bool conn_error = (strncasecmp(data->conn_state, "Error", 5) == 0);
    bool conn_show  = stale || closed || conn_error;
    const char *conn_text = stale ? "RECONNECTING"
                                  : (conn_error ? data->conn_state : "PRINTER OFFLINE");
    if (conn_show) {
        set_txt(s_w.lbl_conn, conn_text);
        if (s_w.lbl_conn) {
            lv_obj_set_style_text_color(s_w.lbl_conn, lv_color_hex(col_alert()), 0);
        }
        if (s_w.conn_dot) {
            lv_obj_set_style_bg_color(s_w.conn_dot, lv_color_hex(col_alert()), 0);
        }
    }
    set_hidden(s_w.conn_chip, !conn_show);
    set_hidden(s_w.lbl_conn, !conn_show);

    /* Fault strip: shown only when it has something to say. Real error text wins,
     * then a closed serial connection (OctoPrint is up but the printer is off).
     * With no fault the strip is hidden outright -- a permanent "No faults" line
     * is noise on every layout. */
    bool fault = (data->error_text[0] != '\0') || data->error || closed;
    if (fault) {
        const char *fault_text;
        if (data->error_text[0] != '\0') {
            fault_text = data->error_text;
        } else if (closed) {
            fault_text = "PRINTER OFFLINE";
        } else {
            fault_text = "Printer error";
        }
        /* The strip would only repeat the visible connection element (Bento
         * places both in one header row), so suppress it when the texts match.
         * When they differ -- a real error alongside a conn line -- both show,
         * exactly as before. */
        if (conn_show && strcmp(fault_text, conn_text) == 0) {
            fault = false;
        } else {
            set_txt(s_w.lbl_error, fault_text);
        }
    }
    if (fault) {
        if (s_w.lbl_error) {
            lv_obj_set_style_text_color(s_w.lbl_error, lv_color_hex(col_alert()), 0);
        }
        if (s_w.error_dot) {
            lv_obj_set_style_bg_color(s_w.error_dot, lv_color_hex(col_alert()), 0);
        }
    }
    /* The label is hidden as well as the strip: a layout may place a bare fault
     * label with no chip around it, leaving error_strip NULL. The dot's own
     * visibility is left alone -- it lives inside the strip, and a layout that
     * hides it deliberately must stay hidden. */
    set_hidden(s_w.error_strip, !fault);
    set_hidden(s_w.lbl_error, !fault);

    /* -- image ------------------------------------------------------------- */
    image_update(data, st, out_retired);
}

void octoprint_page_update(octoprint_data_t *data)
{
    if (!s_root || !data) {
        return;
    }

    /* Phase A -- the caller holds the client lock, NOT the display lock: the
     * bilinear resample runs here so it cannot stall the flush task. */
    octo_staged_t staged;
    stage_image(data, &staged);

    /* Phase B -- display lock taken here, inside the client lock (lock order:
     * client OUTSIDE, display INSIDE; never reverse). Only cheap widget and
     * descriptor work happens under it. */
    uint8_t *retired = NULL;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        page_update_locked(data, &staged, &retired);
        bsp_display_unlock();
    }

    /* Free only after the display lock is released (established pattern), so
     * the flush task can never be rendering from either buffer. */
    if (retired) {
        heap_caps_free(retired);
    }
    if (staged.buf && !staged.consumed) {
        heap_caps_free(staged.buf);
    }
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
        /* Only the accent indicator. The MAIN track is painted by
         * octo_w_progress_bar() at build time and this function only ever runs
         * immediately after build_content(), so rewriting it here is redundant
         * -- and it clobbers layouts that restyle the track themselves (the
         * overlay and letterbox layouts hold it down to OCTO_COL_TEXT at
         * LV_OPA_20). Same reasoning as the conn_chip / error_strip note below. */
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
