/**
 * @file nina_event_log.c
 * @brief Event history ring buffer with scrollable overlay UI.
 *
 * Data path (add/add_fmt) uses a spinlock-protected ring buffer with
 * zero LVGL calls -- safe from any FreeRTOS task.
 * UI path (overlay_create, show, hide, apply_theme) is LVGL-context only.
 */

#include "nina_event_log.h"
#include "nina_dashboard_internal.h"
#include "display_defs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "evtlog";

#define EVENT_LOG_MAX_ENTRIES 100

/* Red Night theme forces all colors to the red palette */
static bool theme_forces_colors(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

/* Severity colors (matching toast colors) */
static const uint32_t sev_colors[] = {
    [EVENT_SEV_INFO]    = 0x4FC3F7,
    [EVENT_SEV_SUCCESS] = 0x66BB6A,
    [EVENT_SEV_WARNING] = 0xFFA726,
    [EVENT_SEV_ERROR]   = 0xEF5350,
};

/* Red Night severity colors — different red brightness levels */
static const uint32_t sev_colors_red[] = {
    [EVENT_SEV_INFO]    = 0x991b1b,
    [EVENT_SEV_SUCCESS] = 0x7f1d1d,
    [EVENT_SEV_WARNING] = 0xcc0000,
    [EVENT_SEV_ERROR]   = 0xff0000,
};

/* ── Ring buffer entry ──────────────────────────────────────────────── */
typedef struct {
    event_severity_t sev;
    int              instance;
    char             message[128];
    int64_t          timestamp_ms;  /* esp_timer_get_time()/1000 */
} event_entry_t;

/* ── Module state ───────────────────────────────────────────────────── */
static event_entry_t  s_entries[EVENT_LOG_MAX_ENTRIES];
static int            s_count = 0;
static int            s_write_index = 0;
static portMUX_TYPE   s_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Overlay UI widgets */
static lv_obj_t      *s_overlay = NULL;
static lv_obj_t      *s_scroll_cont = NULL;
static lv_obj_t      *s_title_label = NULL;

/* ── Format relative time ───────────────────────────────────────────── */
static void format_relative_time(int64_t now_ms, int64_t event_ms, char *buf, size_t buf_size) {
    int64_t diff_s = (now_ms - event_ms) / 1000;
    if (diff_s < 0) diff_s = 0;

    if (diff_s < 60) {
        snprintf(buf, buf_size, "%ds ago", (int)diff_s);
    } else if (diff_s < 3600) {
        snprintf(buf, buf_size, "%dm ago", (int)(diff_s / 60));
    } else {
        snprintf(buf, buf_size, "%dh ago", (int)(diff_s / 3600));
    }
}

/* ── Close button callback ──────────────────────────────────────────── */
static void close_cb(lv_event_t *e) {
    (void)e;
    nina_event_log_hide();
}

/* ── Public API: Data path (thread-safe) ────────────────────────────── */

void nina_event_log_add(event_severity_t sev, int instance, const char *message) {
    if (!message) return;

    portENTER_CRITICAL(&s_spinlock);
    event_entry_t *entry = &s_entries[s_write_index];
    entry->sev = sev;
    entry->instance = instance;
    strncpy(entry->message, message, sizeof(entry->message) - 1);
    entry->message[sizeof(entry->message) - 1] = '\0';
    entry->timestamp_ms = esp_timer_get_time() / 1000;

    s_write_index = (s_write_index + 1) % EVENT_LOG_MAX_ENTRIES;
    if (s_count < EVENT_LOG_MAX_ENTRIES) s_count++;
    portEXIT_CRITICAL(&s_spinlock);

    ESP_LOGD(TAG, "[%d] %s", instance, message);
}

void nina_event_log_add_fmt(event_severity_t sev, int instance, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    nina_event_log_add(sev, instance, buf);
}

/* ── Public API: UI path (LVGL context only) ────────────────────────── */

void nina_event_log_overlay_create(lv_obj_t *screen) {
    if (!screen) return;

    /* Fullscreen overlay */
    s_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_opa(s_overlay, 200, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_pad_all(s_overlay, 20, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_overlay, 8, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Header row: title + close button */
    lv_obj_t *header = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_title_label = lv_label_create(header);
    lv_label_set_text(s_title_label, "Event Log");
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_title_label,
        lv_color_hex(current_theme ? current_theme->text_color : 0xFFFFFF), 0);

    lv_obj_t *btn_close = lv_label_create(header);
    lv_label_set_text(btn_close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(btn_close, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(btn_close,
        lv_color_hex(current_theme ? current_theme->label_color : 0xAAAAAA), 0);
    lv_obj_add_flag(btn_close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn_close, 20);
    lv_obj_add_event_cb(btn_close, close_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable container for event rows */
    s_scroll_cont = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_scroll_cont);
    lv_obj_set_size(s_scroll_cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_scroll_cont, 1);
    lv_obj_set_flex_flow(s_scroll_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scroll_cont, 4, 0);
    lv_obj_add_flag(s_scroll_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll_cont, LV_DIR_VER);
    lv_obj_set_style_max_height(s_scroll_cont, SCREEN_SIZE - 80, 0);

    ESP_LOGI(TAG, "Event log overlay created");
}

void nina_event_log_show(void) {
    if (!s_overlay || !s_scroll_cont) return;

    /* Copy ring buffer under spinlock */
    event_entry_t local[EVENT_LOG_MAX_ENTRIES];
    int local_count = 0;

    portENTER_CRITICAL(&s_spinlock);
    local_count = s_count;
    if (local_count > 0) {
        /* Copy entries in reverse chronological order */
        int read_idx = (s_write_index - 1 + EVENT_LOG_MAX_ENTRIES) % EVENT_LOG_MAX_ENTRIES;
        for (int i = 0; i < local_count; i++) {
            local[i] = s_entries[read_idx];
            read_idx = (read_idx - 1 + EVENT_LOG_MAX_ENTRIES) % EVENT_LOG_MAX_ENTRIES;
        }
    }
    portEXIT_CRITICAL(&s_spinlock);

    /* Clear existing rows */
    lv_obj_clean(s_scroll_cont);

    /* Theme colors */
    uint32_t txt_color = current_theme ? current_theme->text_color : 0xFFFFFF;
    uint32_t label_color = current_theme ? current_theme->label_color : 0x888888;

    int64_t t = esp_timer_get_time() / 1000;

    /* Populate rows (already in reverse chronological order) */
    for (int i = 0; i < local_count; i++) {
        event_entry_t *e = &local[i];

        lv_obj_t *row = lv_obj_create(s_scroll_cont);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 3, 0);

        /* Severity dot */
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(
            theme_forces_colors() ? sev_colors_red[e->sev] : sev_colors[e->sev]), 0);

        /* Instance prefix + message */
        char text[160];
        snprintf(text, sizeof(text), "[%d] %s", e->instance + 1, e->message);
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(txt_color), 0);
        lv_label_set_text(lbl, text);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_flex_grow(lbl, 1);

        /* Relative time */
        char time_buf[16];
        format_relative_time(t, e->timestamp_ms, time_buf, sizeof(time_buf));
        lv_obj_t *lbl_time = lv_label_create(row);
        lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_time, lv_color_hex(label_color), 0);
        lv_label_set_text(lbl_time, time_buf);
    }

    if (local_count == 0) {
        lv_obj_t *empty = lv_label_create(s_scroll_cont);
        lv_label_set_text(empty, "No events recorded");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(label_color), 0);
        lv_obj_set_style_pad_top(empty, 20, 0);
    }

    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_obj_scroll_to_y(s_scroll_cont, 0, LV_ANIM_OFF);

    ESP_LOGI(TAG, "Event log shown (%d entries)", local_count);
}

void nina_event_log_hide(void) {
    if (!s_overlay) return;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    /* Clean rows to free memory */
    if (s_scroll_cont) lv_obj_clean(s_scroll_cont);
}

void nina_event_log_apply_theme(void) {
    if (!s_overlay) return;
    uint32_t txt = current_theme ? current_theme->text_color : 0xFFFFFF;
    if (s_title_label)
        lv_obj_set_style_text_color(s_title_label, lv_color_hex(txt), 0);
    /* Rows are recreated on each show(), so no need to restyle them */
}
