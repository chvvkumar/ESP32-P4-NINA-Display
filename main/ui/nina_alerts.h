#pragma once

/**
 * @file nina_alerts.h
 * @brief Screen-border flash animation for critical events (thread-safe).
 *
 * nina_alert_trigger() is safe to call from ANY FreeRTOS task.  It
 * buffers the request in a spinlock-protected queue; an LVGL timer
 * drains the queue and fires the flash animation in LVGL context.
 *
 * nina_alert_eval() is the breach-state engine on top of that: callers
 * feed it a raw (value, threshold) every cycle and it owns the entry
 * edge, the hysteresis recovery, the voice re-announce timer and both
 * the flash and voice gates.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

/* Append-only: new types go directly above ALERT_TYPE_COUNT.  The value
 * doubles as the alert_voice_types bit position (1=RMS 2=HFR 4=SAFETY). */
typedef enum {
    ALERT_RMS,
    ALERT_HFR,
    ALERT_SAFETY,
    ALERT_TYPE_COUNT,
} alert_type_t;

/** Create the flash overlay.  Call once from LVGL context. */
void nina_alerts_init(lv_obj_t *screen);

/** Trigger an alert flash.  Thread-safe: callable from any task. */
void nina_alert_trigger(alert_type_t type, int instance, float value);

/**
 * @brief Feed one threshold-based sample into the breach engine.
 *
 * Call every eval cycle regardless of the alert_flash_enabled /
 * alert_voice_enabled settings — the gates are applied internally.
 * Breach entry is value > threshold, recovery is value < 0.95*threshold
 * (values inside that band hold the current state).  value <= 0 or
 * threshold <= 0 means "no data / not configured" and is ignored.
 *
 * Thread-safe: callable from any task.
 */
void nina_alert_eval(alert_type_t type, int instance, float value, float threshold);

/**
 * @brief Boolean-valued breach edge for the safety monitor (no threshold).
 *
 * Feed both edges: is_safe=false enters the breach, is_safe=true re-arms it.
 * Thread-safe: callable from any task.
 */
void nina_alert_eval_safety(int instance, bool is_safe);

/** Fire the flash animation immediately (LVGL context only, internal use). */
void nina_alert_flash(void);

/** Re-theme (no-op -- colors are computed at trigger time). */
void nina_alerts_apply_theme(void);

#ifdef __cplusplus
}
#endif
