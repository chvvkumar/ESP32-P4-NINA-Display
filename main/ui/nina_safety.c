/**
 * @file nina_safety.c
 * @brief Safety monitor state tracking (safe / unsafe / disconnected).
 *
 * The visual safety dot is managed by the dashboard (nina_dashboard.c)
 * as a child of each page's exposure box.  This module only tracks state.
 *
 * nina_safety_update() can be called from any task (state is atomic).
 * nina_safety_is_safe() and nina_safety_is_connected() are lock-free reads.
 */

#include "nina_safety.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "safety";

/* ── Module state ───────────────────────────────────────────────────── */
static volatile bool s_connected = false;
static volatile bool s_safe = false;

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_safety_create(lv_obj_t *screen) {
    (void)screen;
    s_connected = false;
    s_safe = false;
    ESP_LOGI(TAG, "Safety state tracker initialized");
}

void nina_safety_update(bool connected, bool is_safe) {
    s_connected = connected;
    s_safe = is_safe;
}

void nina_safety_apply_theme(void) {
    /* Visual dot is managed by dashboard — no-op */
}

bool nina_safety_is_safe(void) {
    return s_safe;
}

bool nina_safety_is_connected(void) {
    return s_connected;
}
