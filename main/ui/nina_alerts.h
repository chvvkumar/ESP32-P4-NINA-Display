#pragma once

/**
 * @file nina_alerts.h
 * @brief Screen-border flash animation for critical events (thread-safe).
 *
 * nina_alert_trigger() is safe to call from ANY FreeRTOS task.  It
 * buffers the request in a spinlock-protected pending slot; an LVGL
 * timer drains the slot and fires the flash animation in LVGL context.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

typedef enum {
    ALERT_RMS,
    ALERT_HFR,
    ALERT_SAFETY,
} alert_type_t;

/** Create the flash overlay.  Call once from LVGL context. */
void nina_alerts_init(lv_obj_t *screen);

/** Trigger an alert flash.  Thread-safe: callable from any task. */
void nina_alert_trigger(alert_type_t type, int instance, float value);

/** Fire the flash animation immediately (LVGL context only, internal use). */
void nina_alert_flash(void);

/** Re-theme (no-op -- colors are computed at trigger time). */
void nina_alerts_apply_theme(void);

#ifdef __cplusplus
}
#endif
