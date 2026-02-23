/**
 * @file nina_alerts.c
 * @brief Screen-border flash animation for critical events (thread-safe).
 *
 * Architecture: nina_alert_trigger() writes to a spinlock-protected
 * pending slot and returns immediately.  An LVGL timer (100 ms) drains
 * the slot and fires the flash animation in LVGL context.
 */

#include "nina_alerts.h"
#include "nina_dashboard_internal.h"
#include "themes.h"
#include "display_defs.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

static const char *TAG = "alerts";

#define ALERT_COOLDOWN_MS   30000   /* 30 s between same-type alerts */
#define FLASH_BORDER_W      8
#define FLASH_STEP_MS       150     /* Duration of each color step */
#define FLASH_CYCLES        3       /* red-white-red = 3 steps per cycle, 3 cycles */

/* ── Pending trigger slot ───────────────────────────────────────────── */
typedef struct {
    alert_type_t type;
    int          instance;
    float        value;
    bool         valid;
} pending_alert_t;

/* ── Module state ───────────────────────────────────────────────────── */
static lv_obj_t        *s_flash_overlay = NULL;
static lv_timer_t      *s_alert_timer = NULL;
static int64_t          s_last_flash_ms[3] = {0, 0, 0};  /* Per alert_type cooldown */
static pending_alert_t  s_pending_alert = {0};
static portMUX_TYPE     s_alert_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Flash animation state ──────────────────────────────────────────── */
static lv_timer_t *s_flash_anim_timer = NULL;
static int  s_flash_step = 0;       /* Current step in flash sequence */
static int  s_flash_total = 0;      /* Total steps (FLASH_CYCLES * 2) */

/* Red Night theme forces all colors to the red palette */
static bool theme_forces_colors(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

/* ── Helpers ────────────────────────────────────────────────────────── */
static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* Animation step callback for border color alternation */
static void flash_step_cb(lv_timer_t *timer) {
    if (!s_flash_overlay) {
        lv_timer_delete(timer);
        s_flash_anim_timer = NULL;
        return;
    }

    if (s_flash_step >= s_flash_total) {
        /* Animation complete — hide overlay */
        lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_timer_delete(timer);
        s_flash_anim_timer = NULL;
        return;
    }

    /* Alternate red / white (or bright/dim red in Red Night) */
    uint32_t color;
    if (theme_forces_colors()) {
        color = (s_flash_step % 2 == 0) ? 0xff0000 : 0x7f1d1d;
    } else {
        color = (s_flash_step % 2 == 0) ? 0xFF0000 : 0xFFFFFF;
    }
    lv_obj_set_style_border_color(s_flash_overlay, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(s_flash_overlay, 200, 0);
    s_flash_step++;
}

/* Fire the flash animation (LVGL context only) */
static void do_flash(void) {
    if (!s_flash_overlay) return;

    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_flash_overlay);

    /* Start border color alternation: FLASH_CYCLES * 2 steps (red,white per cycle) */
    s_flash_step = 0;
    s_flash_total = FLASH_CYCLES * 2;

    /* Set initial red border */
    lv_obj_set_style_border_color(s_flash_overlay, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_opa(s_flash_overlay, 200, 0);
    s_flash_step = 1;

    /* Cancel any in-progress flash animation timer before starting a new one */
    if (s_flash_anim_timer) {
        lv_timer_delete(s_flash_anim_timer);
        s_flash_anim_timer = NULL;
    }
    s_flash_anim_timer = lv_timer_create(flash_step_cb, FLASH_STEP_MS, NULL);

    ESP_LOGI(TAG, "Alert flash triggered");
}

/* ── LVGL timer callback: drain pending alert slot ──────────────────── */
static void alert_tick_cb(lv_timer_t *timer) {
    (void)timer;

    pending_alert_t local = {0};

    portENTER_CRITICAL(&s_alert_lock);
    if (s_pending_alert.valid) {
        local = s_pending_alert;
        s_pending_alert.valid = false;
    }
    portEXIT_CRITICAL(&s_alert_lock);

    if (local.valid) {
        do_flash();
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_alerts_init(lv_obj_t *screen) {
    if (!screen) return;

    /* Full-screen transparent overlay with thick border */
    s_flash_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_flash_overlay);
    lv_obj_set_size(s_flash_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_opa(s_flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_flash_overlay, FLASH_BORDER_W, 0);
    lv_obj_set_style_border_color(s_flash_overlay, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_opa(s_flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Clear state */
    memset(s_last_flash_ms, 0, sizeof(s_last_flash_ms));
    memset(&s_pending_alert, 0, sizeof(s_pending_alert));

    /* Drain timer: 100 ms interval */
    s_alert_timer = lv_timer_create(alert_tick_cb, 100, NULL);

    ESP_LOGI(TAG, "Alert system initialized");
}

void nina_alert_trigger(alert_type_t type, int instance, float value) {
    if (type < 0 || type > ALERT_SAFETY) return;

    /* Cooldown check (read-only, minor race is acceptable) */
    int64_t t = now_ms();
    if (t - s_last_flash_ms[type] < ALERT_COOLDOWN_MS) {
        return;
    }

    /* Update cooldown timestamp */
    s_last_flash_ms[type] = t;

    /* Write to pending slot under spinlock */
    portENTER_CRITICAL(&s_alert_lock);
    s_pending_alert.type = type;
    s_pending_alert.instance = instance;
    s_pending_alert.value = value;
    s_pending_alert.valid = true;
    portEXIT_CRITICAL(&s_alert_lock);

    ESP_LOGI(TAG, "Alert queued: type=%d instance=%d value=%.2f", type, instance, value);
}

void nina_alert_flash(void) {
    /* Direct flash — only call from LVGL context */
    do_flash();
}

void nina_alerts_apply_theme(void) {
    /* Flash colors are set dynamically per step — no persistent state to restyle */
}
