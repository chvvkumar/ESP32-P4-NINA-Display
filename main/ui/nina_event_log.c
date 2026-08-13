/**
 * @file nina_event_log.c
 * @brief Event history ring buffer (data path only).
 *
 * add/add_fmt use a mutex-protected ring buffer with zero LVGL calls --
 * safe from any FreeRTOS task.  Readers (copy_entries/clear) back
 * /api/events.  The on-device overlay UI that used to live here was
 * removed: nothing could ever open it.
 */

#include "nina_event_log.h"
#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "evtlog";

#define EVENT_LOG_MAX_ENTRIES 100

/* ── Ring buffer entry ──────────────────────────────────────────────── */
typedef struct {
    event_severity_t sev;
    int              instance;
    char             message[128];
    int64_t          timestamp_ms;  /* esp_timer_get_time()/1000 */
} event_entry_t;

/* ── Module state ───────────────────────────────────────────────────── */
/* 14.4 KB ring buffer: mutex-protected, task context only, never touched from
 * an ISR or before app_main, so it is safe in PSRAM .ext_ram.bss (zeroed by
 * esp_psram_bss_init() in cpu_start, well before this module is reachable). */
static EXT_RAM_BSS_ATTR event_entry_t s_entries[EVENT_LOG_MAX_ENTRIES];
static int            s_count = 0;
static int            s_write_index = 0;
static SemaphoreHandle_t s_log_mutex = NULL;

/* ── Public API: Data path (thread-safe) ────────────────────────────── */

void nina_event_log_init(void) {
    /* Boot-path only: create the mutex once before any task/HTTP handler can
     * reach the log, so add()/clear()/copy_entries() never race on creation. */
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
}

void nina_event_log_add(event_severity_t sev, int instance, const char *message) {
    if (!message || !s_log_mutex) return;

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50))) {
        event_entry_t *entry = &s_entries[s_write_index];
        entry->sev = sev;
        entry->instance = instance;
        strncpy(entry->message, message, sizeof(entry->message) - 1);
        entry->message[sizeof(entry->message) - 1] = '\0';
        entry->timestamp_ms = esp_timer_get_time() / 1000;

        s_write_index = (s_write_index + 1) % EVENT_LOG_MAX_ENTRIES;
        if (s_count < EVENT_LOG_MAX_ENTRIES) s_count++;
        xSemaphoreGive(s_log_mutex);
    }

    ESP_LOGD(TAG, "[%d] %s", instance, message);
}

void nina_event_log_add_fmt(event_severity_t sev, int instance, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    nina_event_log_add(sev, instance, buf);
}

int nina_event_log_copy_entries(nina_event_log_entry_t *out, int max_out) {
    if (!out || max_out <= 0) return 0;

    int written = 0;
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50))) {
        int avail = s_count;
        if (avail > max_out) avail = max_out;
        /* Copy newest-first */
        int read_idx = (s_write_index - 1 + EVENT_LOG_MAX_ENTRIES) % EVENT_LOG_MAX_ENTRIES;
        for (int i = 0; i < avail; i++) {
            const event_entry_t *e = &s_entries[read_idx];
            out[i].sev = e->sev;
            out[i].instance = e->instance;
            strncpy(out[i].message, e->message, sizeof(out[i].message) - 1);
            out[i].message[sizeof(out[i].message) - 1] = '\0';
            out[i].timestamp_ms = e->timestamp_ms;
            read_idx = (read_idx - 1 + EVENT_LOG_MAX_ENTRIES) % EVENT_LOG_MAX_ENTRIES;
        }
        written = avail;
        xSemaphoreGive(s_log_mutex);
    }
    return written;
}

void nina_event_log_clear(void) {
    if (!s_log_mutex) return;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50))) {
        s_count = 0;
        s_write_index = 0;
        xSemaphoreGive(s_log_mutex);
    }
}
