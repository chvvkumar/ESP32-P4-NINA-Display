#pragma once

/**
 * @file nina_event_log.h
 * @brief Persistent event history ring buffer.
 *
 * All functions here are safe to call from ANY FreeRTOS task -- they
 * touch a mutex-protected ring buffer with zero LVGL calls.  Readers
 * (copy_entries/clear) back the /api/events endpoint.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    EVENT_SEV_INFO,
    EVENT_SEV_SUCCESS,
    EVENT_SEV_WARNING,
    EVENT_SEV_ERROR
} event_severity_t;

/** Snapshot copy of a single event log entry (for API readers). */
typedef struct {
    event_severity_t sev;
    int              instance;
    char             message[128];
    int64_t          timestamp_ms;  /* esp_timer_get_time()/1000 */
} nina_event_log_entry_t;

/**
 * Create the ring-buffer mutex.  Call once from app_main on the boot path
 * before any task or HTTP handler can reach the log.  Not thread-safe: it
 * must run before concurrency begins.
 */
void nina_event_log_init(void);

/** Add an event to the log.  Thread-safe: callable from any task. */
void nina_event_log_add(event_severity_t sev, int instance, const char *message);

/** Printf-style wrapper.  Thread-safe: callable from any task. */
void nina_event_log_add_fmt(event_severity_t sev, int instance, const char *fmt, ...);

/**
 * Copy up to @p max_out entries into @p out under the ring-buffer lock,
 * newest first.  Returns the number of entries written.  Thread-safe:
 * callable from any task.
 */
int nina_event_log_copy_entries(nina_event_log_entry_t *out, int max_out);

/** Clear all entries from the log.  Thread-safe: callable from any task. */
void nina_event_log_clear(void);

#ifdef __cplusplus
}
#endif
