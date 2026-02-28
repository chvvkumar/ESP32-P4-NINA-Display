#include "nina_ota_prompt.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "display_defs.h"
#include <string.h>
#include <stdio.h>

/* ── Widget pointers ────────────────────────────────────────────────── */
static lv_obj_t *ota_overlay = NULL;

/* Info mode widgets */
static lv_obj_t *info_cont      = NULL;
static lv_obj_t *lbl_title      = NULL;
static lv_obj_t *lbl_version    = NULL;
static lv_obj_t *divider        = NULL;
static lv_obj_t *notes_cont     = NULL;
static lv_obj_t *lbl_notes      = NULL;
static lv_obj_t *btn_skip       = NULL;
static lv_obj_t *btn_update     = NULL;
static lv_obj_t *lbl_skip_text  = NULL;
static lv_obj_t *lbl_update_text = NULL;

/* Progress mode widgets */
static lv_obj_t *progress_cont  = NULL;
static lv_obj_t *lbl_dl_title   = NULL;
static lv_obj_t *lbl_percent    = NULL;
static lv_obj_t *lbl_hint       = NULL;
static lv_obj_t *bar_progress   = NULL;
static lv_obj_t *bar_glow       = NULL;

/* Error mode widgets */
static lv_obj_t *error_cont     = NULL;
static lv_obj_t *lbl_err_title  = NULL;
static lv_obj_t *lbl_error      = NULL;
static lv_obj_t *btn_dismiss    = NULL;
static lv_obj_t *lbl_dismiss_text = NULL;

/* User action flags */
static volatile bool update_accepted = false;
static volatile bool update_skipped  = false;

/* ── Callbacks ──────────────────────────────────────────────────────── */

static void update_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    update_accepted = true;
}

static void skip_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    update_skipped = true;
}

static void dismiss_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_ota_prompt_hide();
}

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Dim a color to ~30% brightness for bar track / glow */
static uint32_t dim_color(uint32_t c) {
    uint8_t r = ((c >> 16) & 0xFF) * 30 / 100;
    uint8_t g = ((c >> 8)  & 0xFF) * 30 / 100;
    uint8_t b = ((c)       & 0xFF) * 30 / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ── Create ─────────────────────────────────────────────────────────── */

void nina_ota_prompt_create(lv_obj_t *parent) {
    ota_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(ota_overlay);
    lv_obj_set_size(ota_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* ── Info container (release notes + buttons) ── */
    info_cont = lv_obj_create(ota_overlay);
    lv_obj_remove_style_all(info_cont);
    lv_obj_set_size(info_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(info_cont);
    lv_obj_set_flex_flow(info_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(info_cont, 30, 0);
    lv_obj_set_style_pad_row(info_cont, 12, 0);
    lv_obj_clear_flag(info_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lbl_title = lv_label_create(info_cont);
    lv_label_set_text(lbl_title, "Firmware Update Available");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_title, LV_PCT(100));
    lv_obj_set_style_pad_top(lbl_title, 40, 0);
    lv_obj_set_style_pad_bottom(lbl_title, 8, 0);

    /* Version line */
    lbl_version = lv_label_create(info_cont);
    lv_label_set_text(lbl_version, "");
    lv_obj_set_style_text_font(lbl_version, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_version, lv_color_hex(0x3b82f6), 0);
    lv_obj_set_style_text_align(lbl_version, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_version, LV_PCT(100));

    /* Divider */
    divider = lv_obj_create(info_cont);
    lv_obj_remove_style_all(divider);
    lv_obj_set_width(divider, LV_PCT(90));
    lv_obj_set_height(divider, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_60, 0);
    lv_obj_set_style_margin_top(divider, 4, 0);
    lv_obj_set_style_margin_bottom(divider, 4, 0);

    /* Scrollable release notes area */
    notes_cont = lv_obj_create(info_cont);
    lv_obj_remove_style_all(notes_cont);
    lv_obj_set_width(notes_cont, LV_PCT(90));
    lv_obj_set_height(notes_cont, 320);
    lv_obj_set_scrollbar_mode(notes_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(notes_cont, 10, 0);
    lv_obj_add_flag(notes_cont, LV_OBJ_FLAG_SCROLLABLE);

    lbl_notes = lv_label_create(notes_cont);
    lv_label_set_text(lbl_notes, "");
    lv_obj_set_width(lbl_notes, LV_PCT(100));
    lv_label_set_long_mode(lbl_notes, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_notes, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_notes, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_line_space(lbl_notes, 6, 0);

    /* Button row */
    lv_obj_t *btn_row = lv_obj_create(info_cont);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, 80);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 24, 0);
    lv_obj_set_style_pad_top(btn_row, 8, 0);

    /* Skip button — ghost outline */
    btn_skip = lv_button_create(btn_row);
    lv_obj_set_size(btn_skip, 280, 68);
    lv_obj_set_style_radius(btn_skip, 14, 0);
    lv_obj_set_style_bg_opa(btn_skip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_skip, 1, 0);
    lv_obj_set_style_border_color(btn_skip, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(btn_skip, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn_skip, 0, 0);
    lv_obj_set_style_bg_color(btn_skip, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_skip, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_skip, skip_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_skip_text = lv_label_create(btn_skip);
    lv_label_set_text(lbl_skip_text, "Skip");
    lv_obj_set_style_text_font(lbl_skip_text, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_skip_text, lv_color_hex(0x999999), 0);
    lv_obj_center(lbl_skip_text);

    /* Update button — solid accent */
    btn_update = lv_button_create(btn_row);
    lv_obj_set_size(btn_update, 280, 68);
    lv_obj_set_style_radius(btn_update, 14, 0);
    lv_obj_set_style_bg_color(btn_update, lv_color_hex(0x3b82f6), 0);
    lv_obj_set_style_bg_opa(btn_update, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_update, 0, 0);
    lv_obj_set_style_shadow_width(btn_update, 0, 0);
    lv_obj_set_style_bg_color(btn_update, lv_color_hex(0x1d4ed8), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_update, update_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_update_text = lv_label_create(btn_update);
    lv_label_set_text(lbl_update_text, "Update Now");
    lv_obj_set_style_text_font(lbl_update_text, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_update_text, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl_update_text);

    /* ── Progress container (hidden initially) ── */
    progress_cont = lv_obj_create(ota_overlay);
    lv_obj_remove_style_all(progress_cont);
    lv_obj_set_size(progress_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(progress_cont);
    lv_obj_set_flex_flow(progress_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(progress_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(progress_cont, 16, 0);
    lv_obj_add_flag(progress_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(progress_cont, LV_OBJ_FLAG_SCROLLABLE);

    lbl_dl_title = lv_label_create(progress_cont);
    lv_label_set_text(lbl_dl_title, "Downloading Update");
    lv_obj_set_style_text_font(lbl_dl_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_dl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl_dl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_dl_title, LV_PCT(90));

    lbl_percent = lv_label_create(progress_cont);
    lv_label_set_text(lbl_percent, "0%");
    lv_obj_set_style_text_font(lbl_percent, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_percent, lv_color_hex(0x3b82f6), 0);
    lv_obj_set_style_text_align(lbl_percent, LV_TEXT_ALIGN_CENTER, 0);

    lbl_hint = lv_label_create(progress_cont);
    lv_label_set_text(lbl_hint, "Do not power off");
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hint, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_align(lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(lbl_hint, 8, 0);

    /* Bar container — holds glow + main bar overlapping */
    lv_obj_t *bar_wrap = lv_obj_create(progress_cont);
    lv_obj_remove_style_all(bar_wrap);
    lv_obj_set_size(bar_wrap, 580, 28);
    lv_obj_clear_flag(bar_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(bar_wrap, 16, 0);

    /* Glow behind bar */
    bar_glow = lv_bar_create(bar_wrap);
    lv_obj_remove_style_all(bar_glow);
    lv_obj_set_size(bar_glow, 580, 24);
    lv_bar_set_range(bar_glow, 0, 100);
    lv_bar_set_value(bar_glow, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(bar_glow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_glow, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_glow, lv_color_hex(0x3b82f6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_glow, LV_OPA_40, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_glow, 12, LV_PART_INDICATOR);
    lv_obj_center(bar_glow);

    /* Main bar */
    bar_progress = lv_bar_create(bar_wrap);
    lv_obj_remove_style_all(bar_progress);
    lv_obj_set_size(bar_progress, 560, 12);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_progress, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x3b82f6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 6, LV_PART_INDICATOR);
    lv_obj_center(bar_progress);

    /* ── Error container (hidden initially) ── */
    error_cont = lv_obj_create(ota_overlay);
    lv_obj_remove_style_all(error_cont);
    lv_obj_set_size(error_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(error_cont);
    lv_obj_set_flex_flow(error_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(error_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(error_cont, 20, 0);
    lv_obj_add_flag(error_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(error_cont, LV_OBJ_FLAG_SCROLLABLE);

    lbl_err_title = lv_label_create(error_cont);
    lv_label_set_text(lbl_err_title, "Update Failed");
    lv_obj_set_style_text_font(lbl_err_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_err_title, lv_color_hex(0xFF4444), 0);
    lv_obj_set_style_text_align(lbl_err_title, LV_TEXT_ALIGN_CENTER, 0);

    lbl_error = lv_label_create(error_cont);
    lv_label_set_text(lbl_error, "");
    lv_obj_set_width(lbl_error, LV_PCT(80));
    lv_label_set_long_mode(lbl_error, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_error, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_error, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(lbl_error, LV_TEXT_ALIGN_CENTER, 0);

    btn_dismiss = lv_button_create(error_cont);
    lv_obj_set_size(btn_dismiss, 280, 68);
    lv_obj_set_style_radius(btn_dismiss, 14, 0);
    lv_obj_set_style_bg_opa(btn_dismiss, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_dismiss, 1, 0);
    lv_obj_set_style_border_color(btn_dismiss, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(btn_dismiss, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn_dismiss, 0, 0);
    lv_obj_set_style_bg_color(btn_dismiss, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_dismiss, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_dismiss, dismiss_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_dismiss_text = lv_label_create(btn_dismiss);
    lv_label_set_text(lbl_dismiss_text, "Dismiss");
    lv_obj_set_style_text_font(lbl_dismiss_text, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_dismiss_text, lv_color_hex(0x999999), 0);
    lv_obj_center(lbl_dismiss_text);
}

/* ── Markdown-to-plaintext helper ───────────────────────────────────── */

/**
 * Light cleanup of markdown summary for on-screen display:
 *  - Strips `**bold**` markers
 *  - Converts `- ` list items into separate lines
 *  - Collapses multiple blank lines
 */
static void format_notes_for_display(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    size_t di = 0;

    while (*src && di + 1 < dst_size) {
        /* Strip **bold** markers */
        if (src[0] == '*' && src[1] == '*') {
            src += 2;
            continue;
        }
        /* Convert markdown bullet "- " at start of line to a newline-separated item */
        if ((src[0] == '-' || src[0] == '*') && src[1] == ' ' &&
            (di == 0 || dst[di - 1] == '\n')) {
            /* Already at line start — skip the bullet marker */
            src += 2;
            /* Skip leading whitespace after bullet */
            while (*src == ' ') src++;
            continue;
        }
        dst[di++] = *src++;
    }
    dst[di] = '\0';

    /* Trim trailing whitespace */
    while (di > 0 && (dst[di - 1] == '\n' || dst[di - 1] == '\r' || dst[di - 1] == ' ')) {
        dst[--di] = '\0';
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_ota_prompt_show(const char *new_version, const char *current_version, const char *release_notes) {
    if (!ota_overlay) return;

    update_accepted = false;
    update_skipped = false;

    /* Apply theme before showing */
    nina_ota_prompt_apply_theme();

    /* Set version text — use ASCII arrow (Montserrat lacks the Unicode glyph) */
    char ver_buf[80];
    snprintf(ver_buf, sizeof(ver_buf), "%s  >>  %s", current_version, new_version);
    lv_label_set_text(lbl_version, ver_buf);

    /* Set notes — clean up markdown for on-screen display */
    if (release_notes) {
        char formatted[1024];
        format_notes_for_display(release_notes, formatted, sizeof(formatted));
        lv_label_set_text(lbl_notes, formatted[0] ? formatted : "No release notes available.");
    } else {
        lv_label_set_text(lbl_notes, "No release notes available.");
    }

    /* Show info, hide progress and error */
    lv_obj_clear_flag(info_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(progress_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(error_cont, LV_OBJ_FLAG_HIDDEN);

    /* Reset progress */
    lv_label_set_text(lbl_percent, "0%");
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_bar_set_value(bar_glow, 0, LV_ANIM_OFF);

    /* Show overlay */
    lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
}

void nina_ota_prompt_hide(void) {
    if (!ota_overlay) return;
    lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
    update_accepted = false;
    update_skipped = false;
}

bool nina_ota_prompt_visible(void) {
    if (!ota_overlay) return false;
    return !lv_obj_has_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
}

void nina_ota_prompt_show_progress(void) {
    if (!ota_overlay) return;
    lv_obj_add_flag(info_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(error_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(progress_cont, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(lbl_percent, "0%");
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_bar_set_value(bar_glow, 0, LV_ANIM_OFF);
}

void nina_ota_prompt_set_progress(int percent) {
    if (!ota_overlay) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(lbl_percent, buf);
    lv_bar_set_value(bar_progress, percent, LV_ANIM_ON);
    lv_bar_set_value(bar_glow, percent, LV_ANIM_ON);
}

void nina_ota_prompt_show_error(const char *error_msg) {
    if (!ota_overlay) return;
    lv_obj_add_flag(info_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(progress_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(error_cont, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(lbl_err_title, "Update Failed");
    lv_obj_set_style_text_color(lbl_err_title, lv_color_hex(0xFF4444), 0);
    lv_label_set_text(lbl_error, error_msg ? error_msg : "Unknown error");
}

void nina_ota_prompt_show_status(const char *title, const char *message) {
    if (!ota_overlay) return;
    lv_obj_add_flag(info_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(progress_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(error_cont, LV_OBJ_FLAG_HIDDEN);

    /* Use accent color for title instead of error red */
    uint32_t accent = 0x3b82f6;
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        accent = app_config_apply_brightness(current_theme->progress_color, gb);
    }
    lv_label_set_text(lbl_err_title, title ? title : "");
    lv_obj_set_style_text_color(lbl_err_title, lv_color_hex(accent), 0);
    lv_label_set_text(lbl_error, message ? message : "");
}

void nina_ota_prompt_apply_theme(void) {
    if (!ota_overlay || !current_theme) return;
    int gb = app_config_get()->color_brightness;

    uint32_t accent = app_config_apply_brightness(current_theme->progress_color, gb);
    uint32_t text   = app_config_apply_brightness(current_theme->text_color, gb);
    uint32_t label  = app_config_apply_brightness(current_theme->label_color, gb);
    uint32_t border = current_theme->bento_border;
    uint32_t bg     = current_theme->bg_main;

    /* ── Overlay background ── */
    lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(bg), 0);

    /* ── Info screen ── */
    if (lbl_title)
        lv_obj_set_style_text_color(lbl_title, lv_color_hex(text), 0);
    if (lbl_version)
        lv_obj_set_style_text_color(lbl_version, lv_color_hex(accent), 0);
    if (divider)
        lv_obj_set_style_bg_color(divider, lv_color_hex(border), 0);
    if (lbl_notes)
        lv_obj_set_style_text_color(lbl_notes, lv_color_hex(text), 0);

    /* Skip button */
    if (btn_skip) {
        lv_obj_set_style_border_color(btn_skip, lv_color_hex(border), 0);
        lv_obj_set_style_bg_color(btn_skip, lv_color_hex(border), LV_STATE_PRESSED);
    }
    if (lbl_skip_text)
        lv_obj_set_style_text_color(lbl_skip_text, lv_color_hex(label), 0);

    /* Update button */
    if (btn_update) {
        lv_obj_set_style_bg_color(btn_update, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_color(btn_update, lv_color_hex(dim_color(accent)), LV_STATE_PRESSED);
    }

    /* ── Progress screen ── */
    if (lbl_dl_title)
        lv_obj_set_style_text_color(lbl_dl_title, lv_color_hex(text), 0);
    if (lbl_percent)
        lv_obj_set_style_text_color(lbl_percent, lv_color_hex(accent), 0);
    if (lbl_hint)
        lv_obj_set_style_text_color(lbl_hint, lv_color_hex(label), 0);
    if (bar_glow)
        lv_obj_set_style_bg_color(bar_glow, lv_color_hex(accent), LV_PART_INDICATOR);
    if (bar_progress) {
        lv_obj_set_style_bg_color(bar_progress, lv_color_hex(dim_color(accent)), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_progress, lv_color_hex(accent), LV_PART_INDICATOR);
    }

    /* ── Error screen ── */
    if (lbl_error)
        lv_obj_set_style_text_color(lbl_error, lv_color_hex(text), 0);
    if (btn_dismiss) {
        lv_obj_set_style_border_color(btn_dismiss, lv_color_hex(border), 0);
        lv_obj_set_style_bg_color(btn_dismiss, lv_color_hex(border), LV_STATE_PRESSED);
    }
    if (lbl_dismiss_text)
        lv_obj_set_style_text_color(lbl_dismiss_text, lv_color_hex(label), 0);

    lv_obj_invalidate(ota_overlay);
}

bool nina_ota_prompt_update_accepted(void) {
    bool val = update_accepted;
    update_accepted = false;
    return val;
}

bool nina_ota_prompt_skipped(void) {
    bool val = update_skipped;
    update_skipped = false;
    return val;
}
