/**
 * @file nina_toast.c
 * @brief Pool of 3 stacked toast bars with 16-slot FIFO backlog queue.
 *
 * Architecture: nina_toast_show() writes to a spinlock-protected pending
 * queue and returns immediately.  An LVGL timer (tick_cb, 200 ms) drains
 * the queue in the LVGL context, manages 3 independent toast bar widgets
 * stacked bottom-to-top, and promotes from a 16-slot backlog FIFO when
 * a visible toast expires or is tapped to dismiss.
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
#define PENDING_QUEUE_SIZE  16
#define TICK_INTERVAL_MS    200
#define TOAST_POOL_SIZE     3
#define BACKLOG_SIZE        16

/* Toast bar dimensions */
#define TOAST_RADIUS        18
#define TOAST_MARGIN_X      12
#define TOAST_BOTTOM_Y      (-30)   /* Offset from bottom, above page dots */
#define TOAST_BAR_HEIGHT    68      /* 16 pad + 28 font (2 lines) + 16 pad, room for wrapped hostnames */
#define TOAST_STACK_GAP     6       /* Gap between stacked bars */
#define DOT_SIZE            14
#define DOT_RADIUS          7

/* Animation durations */
#define ANIM_ENTER_MS       300
#define ANIM_EXIT_MS        200
#define ANIM_SHIFT_MS       200

/* Severity background colors (dark, muted) */
static const uint32_t sev_bg_colors[] = {
    [TOAST_INFO]    = 0x0C3060,   /* Dark blue */
    [TOAST_SUCCESS] = 0x0D3320,   /* Dark forest green */
    [TOAST_WARNING] = 0x3D2E00,   /* Dark amber */
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

/* ── Toast pool slot state ──────────────────────────────────────────── */
typedef struct {
    lv_obj_t           *bar;        /* Full-width toast bar */
    lv_obj_t           *dot;        /* Severity dot */
    lv_obj_t           *lbl_msg;    /* Message label */
    lv_obj_t           *lbl_age;    /* Age counter */
    toast_severity_t    sev;
    char                message[128]; /* Original message (without dedup count) */
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

/* ── Backlog FIFO entry ─────────────────────────────────────────────── */
typedef struct {
    toast_severity_t sev;
    char             message[128];
} backlog_entry_t;

/* ── Module state ───────────────────────────────────────────────────── */
static lv_obj_t        *s_screen = NULL;
static toast_state_t    s_pool[TOAST_POOL_SIZE];
static lv_timer_t      *s_tick_timer = NULL;

/* Pending queue (thread-safe, spinlock-protected) */
static pending_toast_t  s_pending[PENDING_QUEUE_SIZE];
static portMUX_TYPE     s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

/* Backlog FIFO ring buffer (LVGL context only, no lock needed) */
static backlog_entry_t  s_backlog[BACKLOG_SIZE];
static int              s_bl_head  = 0;  /* Next read position */
static int              s_bl_tail  = 0;  /* Next write position */
static int              s_bl_count = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */
static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/** Count how many pool slots are currently active. */
static int active_count(void) {
    int n = 0;
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        if (s_pool[i].active) n++;
    }
    return n;
}

/** Compute the Y translate value for a given visual stack position.
 *  Position 0 = bottommost bar, 1 = middle, 2 = topmost.
 *  Each bar sits at TOAST_BOTTOM_Y offset, then shifted up by
 *  pos * (TOAST_BAR_HEIGHT + TOAST_STACK_GAP). */
static int32_t y_for_position(int pos) {
    return TOAST_BOTTOM_Y - pos * (TOAST_BAR_HEIGHT + TOAST_STACK_GAP);
}

/** Get the configured base duration in ms. */
static int base_duration_ms(void) {
    int cfg_dur_s = app_config_get()->toast_duration_s;
    if (cfg_dur_s < 3 || cfg_dur_s > 30) cfg_dur_s = 8;
    return cfg_dur_s * 1000;
}

/** Compute lifetime for a given severity. ERROR/WARNING get 2x. */
static int lifetime_for_severity(toast_severity_t sev) {
    int base = base_duration_ms();
    return (sev == TOAST_ERROR || sev == TOAST_WARNING) ? (base * 2) : base;
}

/* ── Animation callbacks ───────────────────────────────────────────── */
static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_y_cb(void *obj, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

/** Called when a fade-out animation completes — hide the bar. */
static void exit_anim_done_cb(lv_anim_t *a) {
    lv_obj_t *bar = (lv_obj_t *)a->var;
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(bar, LV_OPA_COVER, 0);
}

/* ── Backlog ring buffer operations (LVGL context only) ─────────────── */
static void backlog_push(toast_severity_t sev, const char *msg) {
    if (s_bl_count >= BACKLOG_SIZE) {
        /* Overflow: drop oldest entry */
        ESP_LOGW(TAG, "Backlog overflow, dropping oldest: %s", s_backlog[s_bl_head].message);
        s_bl_head = (s_bl_head + 1) % BACKLOG_SIZE;
        s_bl_count--;
    }
    s_backlog[s_bl_tail].sev = sev;
    strncpy(s_backlog[s_bl_tail].message, msg, sizeof(s_backlog[s_bl_tail].message) - 1);
    s_backlog[s_bl_tail].message[sizeof(s_backlog[s_bl_tail].message) - 1] = '\0';
    s_bl_tail = (s_bl_tail + 1) % BACKLOG_SIZE;
    s_bl_count++;
}

static bool backlog_pop(toast_severity_t *sev, char *msg, size_t msg_sz) {
    if (s_bl_count <= 0) return false;
    *sev = s_backlog[s_bl_head].sev;
    strncpy(msg, s_backlog[s_bl_head].message, msg_sz - 1);
    msg[msg_sz - 1] = '\0';
    s_bl_head = (s_bl_head + 1) % BACKLOG_SIZE;
    s_bl_count--;
    return true;
}

/* ── Apply severity colors to a pool slot ───────────────────────────── */
static void apply_colors(toast_state_t *ts) {
    bool red_night = theme_is_red_night(current_theme);
    toast_severity_t sev = ts->sev;
    lv_obj_set_style_bg_color(ts->bar,
        lv_color_hex(red_night ? sev_bg_red[sev] : sev_bg_colors[sev]), 0);
    lv_obj_set_style_bg_color(ts->dot,
        lv_color_hex(red_night ? sev_dot_red[sev] : sev_dot_colors[sev]), 0);
    lv_obj_set_style_text_color(ts->lbl_msg,
        lv_color_hex(red_night ? 0xcc0000 : 0xFFFFFF), 0);
    lv_obj_set_style_text_color(ts->lbl_age,
        lv_color_hex(red_night ? 0x991b1b : 0xBBBBBB), 0);
}

/* ── Update displayed message (includes dedup count if > 1) ─────────── */
static void update_label(toast_state_t *ts) {
    if (ts->dedup_count > 1) {
        char buf[144];
        snprintf(buf, sizeof(buf), "%s (x%d)", ts->message, ts->dedup_count);
        lv_label_set_text(ts->lbl_msg, buf);
    } else {
        lv_label_set_text(ts->lbl_msg, ts->message);
    }
}

/* ── Show a toast in a specific pool slot (LVGL context) ────────────── */
static void show_in_slot(int idx, toast_severity_t sev, const char *msg, int visual_pos) {
    toast_state_t *ts = &s_pool[idx];

    /* Cancel any running animations on this bar */
    lv_anim_delete(ts->bar, anim_opa_cb);
    lv_anim_delete(ts->bar, anim_y_cb);

    /* Configure state */
    ts->sev = sev;
    strncpy(ts->message, msg, sizeof(ts->message) - 1);
    ts->message[sizeof(ts->message) - 1] = '\0';
    ts->shown_ms = now_ms();
    ts->lifetime_ms = lifetime_for_severity(sev);
    ts->dedup_count = 1;
    ts->active = true;

    /* Apply colors and label */
    apply_colors(ts);
    update_label(ts);

    /* Set countdown */
    char countdown_buf[16];
    snprintf(countdown_buf, sizeof(countdown_buf), "%ds", ts->lifetime_ms / 1000);
    lv_label_set_text(ts->lbl_age, countdown_buf);

    /* Position at visual stack position */
    int32_t target_y = y_for_position(visual_pos);

    /* Show and bring to front */
    lv_obj_clear_flag(ts->bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ts->bar);

    /* Enter animation: slide down from above + fade in.
     * New toasts drop into the top slot from above, which avoids conflicting
     * with the shift-down animation of existing toasts below. */
    lv_obj_set_style_opa(ts->bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(ts->bar, target_y - 30, 0);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, ts->bar);
    lv_anim_set_values(&a_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_opa, ANIM_ENTER_MS);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a_opa, anim_opa_cb);
    lv_anim_start(&a_opa);

    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, ts->bar);
    lv_anim_set_values(&a_y, target_y - 30, target_y);
    lv_anim_set_duration(&a_y, ANIM_ENTER_MS);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a_y, anim_y_cb);
    lv_anim_start(&a_y);

    ESP_LOGI(TAG, "Toast shown [slot %d, pos %d]: [%d] %s", idx, visual_pos, sev, msg);
}

/* ── Dismiss a pool slot (fade-out, mark inactive) ──────────────────── */
static void dismiss_slot(int idx) {
    toast_state_t *ts = &s_pool[idx];
    if (!ts->active || !ts->bar) return;

    ts->active = false;

    /* Cancel any running animations */
    lv_anim_delete(ts->bar, anim_opa_cb);
    lv_anim_delete(ts->bar, anim_y_cb);

    /* Fade out */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ts->bar);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, ANIM_EXIT_MS);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_completed_cb(&a, exit_anim_done_cb);
    lv_anim_start(&a);

    ESP_LOGD(TAG, "Toast dismissed [slot %d]", idx);
}

/* ── Compact pool and promote from backlog ──────────────────────────── */
/**
 * After one or more toasts are dismissed, rebuild the visual positions:
 * - Collect all active slots, sorted by shown_ms (oldest first = pos 0).
 * - Animate each to its correct Y position.
 * - If backlog has items and there's a free slot, promote into top position.
 */
static void compact_and_promote(void) {
    /* Gather active slot indices sorted by shown_ms (oldest first) */
    int order[TOAST_POOL_SIZE];
    int n = 0;
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        if (s_pool[i].active) order[n++] = i;
    }
    /* Simple insertion sort by shown_ms ascending */
    for (int i = 1; i < n; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && s_pool[order[j]].shown_ms > s_pool[key].shown_ms) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    /* Animate each active toast to its correct visual position */
    for (int pos = 0; pos < n; pos++) {
        toast_state_t *ts = &s_pool[order[pos]];
        int32_t target_y = y_for_position(pos);

        /* Cancel any running Y animation */
        lv_anim_delete(ts->bar, anim_y_cb);

        lv_anim_t a_y;
        lv_anim_init(&a_y);
        lv_anim_set_var(&a_y, ts->bar);
        lv_anim_set_values(&a_y, lv_obj_get_style_translate_y(ts->bar, 0), target_y);
        lv_anim_set_duration(&a_y, ANIM_SHIFT_MS);
        lv_anim_set_path_cb(&a_y, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a_y, anim_y_cb);
        lv_anim_start(&a_y);

        /* Ensure visible and in front */
        lv_obj_move_foreground(ts->bar);
    }

    /* Promote from backlog if there's a free slot */
    if (n < TOAST_POOL_SIZE && s_bl_count > 0) {
        toast_severity_t sev;
        char msg[128];
        if (backlog_pop(&sev, msg, sizeof(msg))) {
            /* Find a free pool slot */
            for (int i = 0; i < TOAST_POOL_SIZE; i++) {
                if (!s_pool[i].active) {
                    show_in_slot(i, sev, msg, n); /* n = next visual position (top) */
                    break;
                }
            }
        }
    }
}

/* ── Click handler for tap-to-dismiss ───────────────────────────────── */
static void toast_click_cb(lv_event_t *e) {
    lv_obj_t *bar = lv_event_get_target(e);

    /* Find which pool slot this bar belongs to */
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        if (s_pool[i].bar == bar && s_pool[i].active) {
            ESP_LOGI(TAG, "Toast tapped [slot %d], dismissing", i);
            dismiss_slot(i);
            compact_and_promote();
            return;
        }
    }
}

/* ── Dedup: check all active pool slots for matching message+severity ── */
static bool try_dedup(toast_severity_t sev, const char *msg) {
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        toast_state_t *ts = &s_pool[i];
        if (!ts->active) continue;

        if (ts->sev == sev && strcmp(ts->message, msg) == 0) {
            /* Bump count and reset timer */
            ts->dedup_count++;
            ts->shown_ms = now_ms();
            ts->lifetime_ms = lifetime_for_severity(sev);
            update_label(ts);
            /* Update countdown */
            char countdown_buf[16];
            snprintf(countdown_buf, sizeof(countdown_buf), "%ds", ts->lifetime_ms / 1000);
            lv_label_set_text(ts->lbl_age, countdown_buf);
            ESP_LOGD(TAG, "Toast dedup [slot %d]: %s (x%d)", i, msg, ts->dedup_count);
            return true;
        }
    }
    return false;
}

/* ── Add a toast to pool or backlog (LVGL context) ──────────────────── */
static void add_toast(toast_severity_t sev, const char *msg) {
    /* Try dedup first */
    if (try_dedup(sev, msg)) return;

    /* Find an inactive pool slot */
    int n_active = active_count();
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        if (!s_pool[i].active) {
            show_in_slot(i, sev, msg, n_active); /* Visual pos = current active count (top) */
            return;
        }
    }

    /* All pool slots full — add to backlog */
    backlog_push(sev, msg);
    ESP_LOGD(TAG, "Toast queued to backlog (%d items): %s", s_bl_count, msg);
}

/* ── LVGL timer callback: drain pending, manage pool, expire ────────── */
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

    /* 2. Process pending toasts → add to pool or backlog */
    for (int i = 0; i < pending_count; i++) {
        add_toast(local[i].sev, local[i].message);
    }

    /* 3. Check lifetimes, expire any that exceeded their lifetime */
    int64_t t = now_ms();
    bool any_expired = false;
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        toast_state_t *ts = &s_pool[i];
        if (!ts->active) continue;

        int64_t elapsed = t - ts->shown_ms;
        if (elapsed >= ts->lifetime_ms) {
            dismiss_slot(i);
            any_expired = true;
        }
    }

    /* 4. If anything expired, compact the pool and promote from backlog */
    if (any_expired) {
        compact_and_promote();
    }

    /* 5. Update countdown labels for all active toasts */
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        toast_state_t *ts = &s_pool[i];
        if (!ts->active || !ts->lbl_age) continue;

        int64_t elapsed = now_ms() - ts->shown_ms;
        int remaining_s = (int)((ts->lifetime_ms - elapsed + 999) / 1000);
        if (remaining_s < 0) remaining_s = 0;
        char buf[16];
        snprintf(buf, sizeof(buf), "%ds", remaining_s);
        lv_label_set_text(ts->lbl_age, buf);
    }
}

/* ── Create a single toast bar widget (hidden initially) ────────────── */
static void create_bar(int idx) {
    toast_state_t *ts = &s_pool[idx];

    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, SCREEN_SIZE - 2 * TOAST_MARGIN_X);
    lv_obj_set_height(bar, TOAST_BAR_HEIGHT);
    lv_obj_set_align(bar, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_translate_y(bar, TOAST_BOTTOM_Y, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_radius(bar, TOAST_RADIUS, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Row layout: dot | message | age */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, 16, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_set_style_pad_top(bar, 16, 0);
    lv_obj_set_style_pad_bottom(bar, 16, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);
    ts->bar = bar;

    /* Tap-to-dismiss click event */
    lv_obj_add_event_cb(bar, toast_click_cb, LV_EVENT_CLICKED, NULL);

    /* Severity dot */
    lv_obj_t *dot = lv_obj_create(bar);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, DOT_RADIUS, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x4CAF50), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    ts->dot = dot;

    /* Message label (grows to fill available space) */
    lv_obj_t *lbl_msg = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_msg, "");
    lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(lbl_msg, 1);
    ts->lbl_msg = lbl_msg;

    /* Age label (right-aligned, fixed minimum width) */
    lv_obj_t *lbl_age = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_age, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_age, lv_color_hex(0xBBBBBB), 0);
    lv_label_set_text(lbl_age, "");
    lv_obj_set_style_min_width(lbl_age, 48, 0);
    lv_obj_set_style_text_align(lbl_age, LV_TEXT_ALIGN_RIGHT, 0);
    ts->lbl_age = lbl_age;

    /* Initialize state */
    ts->active = false;
    ts->dedup_count = 0;
    ts->message[0] = '\0';
}

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_toast_init(lv_obj_t *screen) {
    if (!screen) return;
    s_screen = screen;

    /* Create 3 toast bar widgets (hidden initially) */
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        create_bar(i);
    }

    /* Clear pending queue */
    memset(s_pending, 0, sizeof(s_pending));

    /* Clear backlog */
    s_bl_head = 0;
    s_bl_tail = 0;
    s_bl_count = 0;

    /* Tick timer: 200 ms interval, runs in LVGL context */
    s_tick_timer = lv_timer_create(tick_cb, TICK_INTERVAL_MS, NULL);

    ESP_LOGI(TAG, "Toast system initialized (pool=%d, backlog=%d)", TOAST_POOL_SIZE, BACKLOG_SIZE);
}

void nina_toast_show(toast_severity_t sev, const char *msg) {
    if (!msg || !s_pool[0].bar) return;

    /* Write to pending queue under spinlock — zero LVGL calls */
    portENTER_CRITICAL(&s_pending_lock);
    int slot = -1;
    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        if (!s_pending[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Queue full — overwrite oldest (slot 0) */
        slot = 0;
    }

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
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        dismiss_slot(i);
    }
    /* Clear backlog too */
    s_bl_head = 0;
    s_bl_tail = 0;
    s_bl_count = 0;
}

void nina_toast_apply_theme(void) {
    for (int i = 0; i < TOAST_POOL_SIZE; i++) {
        if (s_pool[i].active && s_pool[i].bar) {
            apply_colors(&s_pool[i]);
        }
    }
}
