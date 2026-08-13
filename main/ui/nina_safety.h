#pragma once

/**
 * @file nina_safety.h
 * @brief Safety monitor state tracking (safe / unsafe / disconnected).
 *
 * All functions are safe to call from any task (state tracking only,
 * no LVGL calls).  The visual dot is managed by the dashboard.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

/** Initialize safety state (call once, screen param kept for API compat). */
void nina_safety_create(lv_obj_t *screen);

/** Update safety state.  Safe from any task. */
void nina_safety_update(bool connected, bool is_safe);

/** Re-theme (no-op, visual dot managed by dashboard). */
void nina_safety_apply_theme(void);

#ifdef __cplusplus
}
#endif
