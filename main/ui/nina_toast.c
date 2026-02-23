/**
 * @file nina_toast.c
 * @brief Full-width severity-colored toast bar with thread-safe pending queue.
 *
 * Architecture: nina_toast_show() writes to a spinlock-protected pending
 * queue and returns immediately.  An LVGL timer (tick_cb, 200 ms) drains
 * the queue in the LVGL context and creates/manages the toast bar widget.
 */

#include "nina_toast.h"
#include "nina_dashboard_internal.h"
#include "themes.h"
#include "display_defs.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "toast";

/* ── Configuration ──────────────────────────────────────────────────── */
#define TOAST_DEFAULT_MS    8000
#define TOAST_ERROR_MS      15000
#define TOAST_DEDUP_MS      5000
#define PENDING_QUEUE_SIZE  4
#define TICK_INTERVAL_MS    200

/* Toast bar dimensions */
#define TOAST_HEIGHT        80
#define TOAST_RADIUS        18
#define TOAST_MARGIN_X      12
#define TOAST_BOTTOM_Y      -30   /* Offset from bottom, above page dots */
#define DOT_SIZE            14
#define DOT_RADIUS          7

/* Red Night theme forces all colors to the red palette */
static bool theme_forces_colors(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

/* Severity background colors (dark, muted) */
static const uint32_t sev_bg_colors[] = {
    [TOAST_INFO]    = 0x0C3060,   /* Dark blue */
    [TOAST_SUCCESS] = 0x1B5E20,   /* Dark green */
    [TOAST_WARNING] = 0x4E342E,   /* Dark brown */
    [TOAST_ERROR]   = 0x7F0000,   /* Dark red */
};

/* Severity dot colors (bright accent) */
static const uint32_t sev_dot_colors[] = {
    [TOAST_INFO]    = 0x42A5F5,   /* Blue */
    [TOAST_SUCCESS] = 0x4CAF50,   /* Green */
    [TOAST_WARNING] = 0xFFA726,   /* Orange */
    [TOAST_ERROR]   = 0xF44336,   /* Red */
};

/* Red Night severity backgrounds — different red brightness levels */
static const uint32_t sev_bg_red[] = {
    [TOAST_INFO]    = 0x330000,
    [TOAST_SUCCESS] = 0x450a0a,
    [TOAST_WARNING] = 0x5c1010,
    [TOAST_ERROR]   = 0x7F0000,
};

/* Red Night severity dots — brighter red accents */
static const uint32_t sev_dot_red[] = {
    [TOAST_INFO]    = 0x991b1b,
    [TOAST_SUCCESS] = 0x7f1d1d,
    [TOAST_WARNING] = 0xcc0000,
    [TOAST_ERROR]   = 0xff0000,
};

/* ── Toast state ────────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t           *bar;        /* Full-width toast bar */
    lv_obj_t           *dot;        /* Severity dot */
    lv_obj_t           *lbl_msg;    /* Message label */
    lv_obj_t           *lbl_age;    /* Age counter */
    toast_severity_t    sev;
    char                message[128];
    int64_t             shown_ms;
    int                 lifetime_ms;
    int                 dedup_count;
    bool                active;
} toast_state_t;

/* ── Thread-safe pending queue ──────────────────────────────────────── */
typedef struct {
    toast_severity_t sev;
    char             message[128];
    bool             valid;
} pending_toast_t;

/* ── Module state ───────────────────────────────────────────────────── */
static lv_obj_t        *s_screen = NULL;
static toast_state_t    s_toast = {0};
static lv_timer_t      *s_tick_timer = NULL;

static pending_toast_t  s_pending[PENDING_QUEUE_SIZE];
static portMUX_TYPE     s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Helpers ────────────────────────────────────────────────────────── */
static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* ── Animation callbacks ───────────────────────────────────────────── */
static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_y_cb(void *obj, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

static void exit_anim_done_cb(lv_anim_t *a) {
    lv_obj_t *bar = (lv_obj_t *)a->var;
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(bar, LV_OPA_COVER, 0);
}

/* ── Dismiss current toast ──────────────────────────────────────────── */
static void dismiss_toast(void) {
    if (!s_toast.active || !s_toast.bar) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast.bar);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_completed_cb(&a, exit_anim_done_cb);
    lv_anim_start(&a);

    s_toast.active = false;
}

/* ── Show a toast (LVGL context only) ───────────────────────────────── */
static void show_toast(toast_severity_t sev, const char *msg) {
    if (!s_toast.bar) return;

    /* Cancel any running animations */
    lv_anim_delete(s_toast.bar, anim_opa_cb);
    lv_anim_delete(s_toast.bar, anim_y_cb);

    /* Configure state — use config duration, errors get 2x */
    s_toast.sev = sev;
    strncpy(s_toast.message, msg, sizeof(s_toast.message) - 1);
    s_toast.shown_ms = now_ms();
    int cfg_dur_s = app_config_get()->toast_duration_s;
    if (cfg_dur_s < 3 || cfg_dur_s > 30) cfg_dur_s = 8;
    s_toast.lifetime_ms = (sev == TOAST_ERROR) ? (cfg_dur_s * 2000) : (cfg_dur_s * 1000);
    s_toast.dedup_count = 1;
    s_toast.active = true;

    /* Apply severity colors — use red palette in Red Night */
    bool red_night = theme_forces_colors();
    lv_obj_set_style_bg_color(s_toast.bar,
        lv_color_hex(red_night ? sev_bg_red[sev] : sev_bg_colors[sev]), 0);
    lv_obj_set_style_bg_color(s_toast.dot,
        lv_color_hex(red_night ? sev_dot_red[sev] : sev_dot_colors[sev]), 0);
    lv_obj_set_style_text_color(s_toast.lbl_msg,
        lv_color_hex(red_night ? 0xcc0000 : 0xFFFFFF), 0);
    lv_obj_set_style_text_color(s_toast.lbl_age,
        lv_color_hex(red_night ? 0x991b1b : 0xBBBBBB), 0);
    lv_label_set_text(s_toast.lbl_msg, msg);
    char countdown_buf[16];
    snprintf(countdown_buf, sizeof(countdown_buf), "%ds", s_toast.lifetime_ms / 1000);
    lv_label_set_text(s_toast.lbl_age, countdown_buf);

    /* Show and bring to front */
    lv_obj_clear_flag(s_toast.bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast.bar);
    lv_obj_set_style_opa(s_toast.bar, LV_OPA_COVER, 0);

    /* Enter animation: slide up + fade in */
    lv_obj_set_style_opa(s_toast.bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(s_toast.bar, 20, 0);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, s_toast.bar);
    lv_anim_set_values(&a_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_opa, 300);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a_opa, anim_opa_cb);
    lv_anim_start(&a_opa);

    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, s_toast.bar);
    lv_anim_set_values(&a_y, 20, 0);
    lv_anim_set_duration(&a_y, 300);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a_y, anim_y_cb);
    lv_anim_start(&a_y);

    ESP_LOGI(TAG, "Toast shown: [%d] %s", sev, msg);
}

/* ── Dedup: if same message already showing, bump counter ──────────── */
static bool try_dedup(toast_severity_t sev, const char *msg) {
    if (!s_toast.active) return false;

    int64_t deadline = now_ms() - TOAST_DEDUP_MS;
    if (s_toast.sev == sev && s_toast.shown_ms >= deadline
        && strcmp(s_toast.message, msg) == 0) {
        s_toast.dedup_count++;
        s_toast.shown_ms = now_ms(); /* Reset age */
        /* Update message with count */
        char buf[144];
        snprintf(buf, sizeof(buf), "%s (x%d)", msg, s_toast.dedup_count);
        lv_label_set_text(s_toast.lbl_msg, buf);
        return true;
    }
    return false;
}

/* ── LVGL timer callback: drain pending queue + manage toast ────────── */
static void tick_cb(lv_timer_t *timer) {
    (void)timer;

    /* 1. Drain pending queue under spinlock (copy out) */
    pending_toast_t local[PENDING_QUEUE_SIZE];
    int pending_count = 0;

    portENTER_CRITICAL(&s_pending_lock);
    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        if (s_pending[i].valid) {
            local[pending_count++] = s_pending[i];
            s_pending[i].valid = false;
        }
    }
    portEXIT_CRITICAL(&s_pending_lock);

    /* 2. Process pending — newest wins (replaces current toast) */
    for (int i = 0; i < pending_count; i++) {
        if (!try_dedup(local[i].sev, local[i].message)) {
            show_toast(local[i].sev, local[i].message);
        }
    }

    /* 3. Update countdown label and auto-dismiss */
    if (s_toast.active && s_toast.bar) {
        int64_t t = now_ms();
        int64_t elapsed = t - s_toast.shown_ms;

        /* Auto-dismiss */
        if (elapsed >= s_toast.lifetime_ms) {
            dismiss_toast();
            return;
        }

        /* Update countdown text */
        if (s_toast.lbl_age) {
            int remaining_s = (int)((s_toast.lifetime_ms - elapsed + 999) / 1000);
            if (remaining_s < 0) remaining_s = 0;
            char buf[16];
            snprintf(buf, sizeof(buf), "%ds", remaining_s);
            lv_label_set_text(s_toast.lbl_age, buf);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_toast_init(lv_obj_t *screen) {
    if (!screen) return;
    s_screen = screen;

    /* Create the persistent toast bar (hidden initially) */
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCREEN_SIZE - 2 * TOAST_MARGIN_X, TOAST_HEIGHT);
    lv_obj_set_align(bar, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_translate_y(bar, TOAST_BOTTOM_Y, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_radius(bar, TOAST_RADIUS, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    /* Row layout: dot | message | age */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, 16, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);
    s_toast.bar = bar;

    /* Severity dot */
    lv_obj_t *dot = lv_obj_create(bar);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, DOT_RADIUS, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x4CAF50), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    s_toast.dot = dot;

    /* Message label (grows to fill available space) */
    lv_obj_t *lbl_msg = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_msg, "");
    lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(lbl_msg, 1);
    s_toast.lbl_msg = lbl_msg;

    /* Age label (right-aligned) */
    lv_obj_t *lbl_age = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_age, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_age, lv_color_hex(0xBBBBBB), 0);
    lv_label_set_text(lbl_age, "");
    s_toast.lbl_age = lbl_age;

    /* Clear pending queue */
    memset(s_pending, 0, sizeof(s_pending));
    s_toast.active = false;

    /* Tick timer: 200 ms interval, runs in LVGL context */
    s_tick_timer = lv_timer_create(tick_cb, TICK_INTERVAL_MS, NULL);

    ESP_LOGI(TAG, "Toast system initialized");
}

void nina_toast_show(toast_severity_t sev, const char *msg) {
    if (!msg || !s_toast.bar) return;

    /* Write to pending queue under spinlock — zero LVGL calls */
    portENTER_CRITICAL(&s_pending_lock);
    int slot = -1;
    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        if (!s_pending[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) slot = 0;

    s_pending[slot].sev = sev;
    strncpy(s_pending[slot].message, msg, sizeof(s_pending[slot].message) - 1);
    s_pending[slot].message[sizeof(s_pending[slot].message) - 1] = '\0';
    s_pending[slot].valid = true;
    portEXIT_CRITICAL(&s_pending_lock);
}

void nina_toast_show_fmt(toast_severity_t sev, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    nina_toast_show(sev, buf);
}

void nina_toast_dismiss_all(void) {
    dismiss_toast();
}

void nina_toast_apply_theme(void) {
    if (!s_toast.active || !s_toast.bar) return;

    /* Re-color active toast for current theme */
    bool red_night = theme_forces_colors();
    toast_severity_t sev = s_toast.sev;
    lv_obj_set_style_bg_color(s_toast.bar,
        lv_color_hex(red_night ? sev_bg_red[sev] : sev_bg_colors[sev]), 0);
    lv_obj_set_style_bg_color(s_toast.dot,
        lv_color_hex(red_night ? sev_dot_red[sev] : sev_dot_colors[sev]), 0);
    lv_obj_set_style_text_color(s_toast.lbl_msg,
        lv_color_hex(red_night ? 0xcc0000 : 0xFFFFFF), 0);
    lv_obj_set_style_text_color(s_toast.lbl_age,
        lv_color_hex(red_night ? 0x991b1b : 0xBBBBBB), 0);
}
