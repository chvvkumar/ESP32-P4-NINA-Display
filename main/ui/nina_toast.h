#pragma once

/**
 * @file nina_toast.h
 * @brief Transient stacking toast notifications (thread-safe).
 *
 * nina_toast_show() / nina_toast_show_fmt() are safe to call from ANY
 * FreeRTOS task (WebSocket handler, data task, etc.).  They buffer the
 * request in a spinlock-protected pending queue; an LVGL timer drains
 * the queue and creates the pill widgets in the LVGL context.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

typedef enum {
    TOAST_INFO,
    TOAST_SUCCESS,
    TOAST_WARNING,
    TOAST_ERROR
} toast_severity_t;

/** Create the toast container and tick timer.  Call once from LVGL context. */
void nina_toast_init(lv_obj_t *screen);

/** Show a toast notification.  Thread-safe: callable from any task. */
void nina_toast_show(toast_severity_t sev, const char *msg);

/** Printf-style wrapper around nina_toast_show().  Thread-safe. */
void nina_toast_show_fmt(toast_severity_t sev, const char *fmt, ...);

/** Dismiss all visible toasts (LVGL context only). */
void nina_toast_dismiss_all(void);

/** Re-apply current theme colors to visible toasts (LVGL context only). */
void nina_toast_apply_theme(void);

#ifdef __cplusplus
}
#endif
