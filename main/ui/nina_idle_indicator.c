/**
 * @file nina_idle_indicator.c
 * @brief Minimal pill-shaped connection status indicator for idle pages.
 */

#include "nina_idle_indicator.h"
#include "app_config.h"
#include "display_defs.h"
#include "esp_lvgl_port.h"

#include <string.h>

/* ── Configuration ──────────────────────────────────────────────────── */
#define MAX_INDICATORS  4

/* Container dimensions (used on non-clock pages) */
#define PILL_SIZE       18      /* circle container */
#define PILL_RADIUS     LV_RADIUS_CIRCLE
#define DOT_SIZE        6

/* Clock-only: bare dot, no container */
#define CLOCK_DOT_SIZE  10

/* Disconnected — muted brick red */
#define RED_BG          0x7f0000
#define RED_BG_OPA      128             /* 50% */
#define RED_BORDER      0x8B4513
#define RED_BORDER_OPA  178             /* 70% */
#define RED_DOT         0x8B4513        /* muted brick red */

/* Reconnecting — muted sage green */
#define GREEN_BG        0x1a2e1a
#define GREEN_BG_OPA    128             /* 50% */
#define GREEN_BORDER    0x6B8E6B
#define GREEN_BORDER_OPA 178            /* 70% */
#define GREEN_DOT       0x6B8E6B        /* sage green */

/* ── Per-indicator state ────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *pill;     /* NULL for clock (bare dot mode) */
    lv_obj_t *dot;
} indicator_entry_t;

static indicator_entry_t s_indicators[MAX_INDICATORS];
static int               s_count;

/* ── Helpers ────────────────────────────────────────────────────────── */

static void apply_red(indicator_entry_t *ind)
{
    if (ind->pill) {
        lv_obj_set_style_bg_color(ind->pill, lv_color_hex(RED_BG), 0);
        lv_obj_set_style_bg_opa(ind->pill, RED_BG_OPA, 0);
        lv_obj_set_style_border_color(ind->pill, lv_color_hex(RED_BORDER), 0);
        lv_obj_set_style_border_opa(ind->pill, RED_BORDER_OPA, 0);
    }
    lv_obj_set_style_bg_color(ind->dot, lv_color_hex(RED_DOT), 0);
    lv_obj_set_style_shadow_color(ind->dot, lv_color_hex(RED_DOT), 0);
}

static void apply_green(indicator_entry_t *ind)
{
    if (ind->pill) {
        lv_obj_set_style_bg_color(ind->pill, lv_color_hex(GREEN_BG), 0);
        lv_obj_set_style_bg_opa(ind->pill, GREEN_BG_OPA, 0);
        lv_obj_set_style_border_color(ind->pill, lv_color_hex(GREEN_BORDER), 0);
        lv_obj_set_style_border_opa(ind->pill, GREEN_BORDER_OPA, 0);
    }
    lv_obj_set_style_bg_color(ind->dot, lv_color_hex(GREEN_DOT), 0);
    lv_obj_set_style_shadow_color(ind->dot, lv_color_hex(GREEN_DOT), 0);
}

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_idle_indicator_create(lv_obj_t *parent, lv_align_t align, bool bare_dot)
{
    if (s_count >= MAX_INDICATORS) return;

    indicator_entry_t *ind = &s_indicators[s_count++];
    int y_offs = (align == LV_ALIGN_TOP_MID) ? 4 : -4;

    if (bare_dot) {
        /* Clock mode: bare dot directly on parent, no container */
        ind->pill = NULL;
        int dot_sz = CLOCK_DOT_SIZE;

        ind->dot = lv_obj_create(parent);
        lv_obj_remove_style_all(ind->dot);
        lv_obj_set_size(ind->dot, dot_sz, dot_sz);
        lv_obj_set_style_radius(ind->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ind->dot, lv_color_hex(RED_DOT), 0);
        lv_obj_set_style_bg_opa(ind->dot, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(ind->dot, lv_color_hex(RED_DOT), 0);
        lv_obj_set_style_shadow_width(ind->dot, 3, 0);
        lv_obj_set_style_shadow_spread(ind->dot, 1, 0);
        lv_obj_set_style_shadow_opa(ind->dot, 128, 0);
        lv_obj_add_flag(ind->dot, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(ind->dot, align, 0, y_offs);

        lv_obj_add_flag(ind->dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Standard mode: dot inside circle container */
        ind->pill = lv_obj_create(parent);
        lv_obj_remove_style_all(ind->pill);
        lv_obj_set_size(ind->pill, PILL_SIZE, PILL_SIZE);
        lv_obj_set_style_radius(ind->pill, PILL_RADIUS, 0);
        lv_obj_set_style_bg_color(ind->pill, lv_color_hex(RED_BG), 0);
        lv_obj_set_style_bg_opa(ind->pill, RED_BG_OPA, 0);
        lv_obj_set_style_border_width(ind->pill, 1, 0);
        lv_obj_set_style_border_color(ind->pill, lv_color_hex(RED_BORDER), 0);
        lv_obj_set_style_border_opa(ind->pill, RED_BORDER_OPA, 0);
        lv_obj_set_style_pad_all(ind->pill, 0, 0);
        lv_obj_add_flag(ind->pill, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(ind->pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(ind->pill, align, 0, y_offs);

        ind->dot = lv_obj_create(ind->pill);
        lv_obj_remove_style_all(ind->dot);
        lv_obj_set_size(ind->dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(ind->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ind->dot, lv_color_hex(RED_DOT), 0);
        lv_obj_set_style_bg_opa(ind->dot, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(ind->dot, lv_color_hex(RED_DOT), 0);
        lv_obj_set_style_shadow_width(ind->dot, 4, 0);
        lv_obj_set_style_shadow_spread(ind->dot, 2, 0);
        lv_obj_set_style_shadow_opa(ind->dot, LV_OPA_COVER, 0);
        lv_obj_align(ind->dot, LV_ALIGN_CENTER, 0, 0);

        lv_obj_add_flag(ind->pill, LV_OBJ_FLAG_HIDDEN);
    }
}

void nina_idle_indicator_reset(void)
{
    memset(s_indicators, 0, sizeof(s_indicators));
    s_count = 0;
}

/* Get the root object for show/hide — pill if present, else dot */
static inline lv_obj_t *root_obj(indicator_entry_t *ind)
{
    return ind->pill ? ind->pill : ind->dot;
}

void nina_idle_indicator_set_active(bool idle_active)
{
    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        for (int i = 0; i < s_count; i++) {
            indicator_entry_t *ind = &s_indicators[i];
            lv_obj_t *root = root_obj(ind);
            if (!root) continue;

            if (idle_active && app_config_get()->idle_indicator_enabled) {
                apply_red(ind);
                lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lvgl_port_unlock();
    }
}

void nina_idle_indicator_set_reconnecting(void)
{
    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        for (int i = 0; i < s_count; i++) {
            indicator_entry_t *ind = &s_indicators[i];
            lv_obj_t *root = root_obj(ind);
            if (!root) continue;

            if (!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
                apply_green(ind);
            }
        }
        lvgl_port_unlock();
    }
}
