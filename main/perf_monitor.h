#pragma once
#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>

// Enable/disable profiling via Kconfig (idf.py menuconfig)
#ifdef CONFIG_PERF_MONITOR_ENABLED
#define PERF_MONITOR_ENABLED 1
#else
#define PERF_MONITOR_ENABLED 0
#endif

#if PERF_MONITOR_ENABLED

#include "esp_timer.h"

// ── Timing Metrics ──────────────────────────────────────────────────

typedef struct {
    int64_t  last_us;        // Most recent duration (microseconds)
    int64_t  min_us;         // Minimum observed
    int64_t  max_us;         // Maximum observed
    int64_t  total_us;       // Running total for average calculation
    uint32_t count;          // Number of samples
} perf_timer_t;

// Start/stop a named timer. Returns elapsed microseconds.
void     perf_timer_start(perf_timer_t *t);
int64_t  perf_timer_stop(perf_timer_t *t);
void     perf_timer_reset(perf_timer_t *t);

// ── Counter Metrics ─────────────────────────────────────────────────

typedef struct {
    uint32_t total;          // Cumulative count
    uint32_t per_interval;   // Count since last report
} perf_counter_t;

void perf_counter_increment(perf_counter_t *c);
void perf_counter_reset_interval(perf_counter_t *c);

// ── Global Profiling State ──────────────────────────────────────────

typedef struct {
    // Poll cycle timing
    perf_timer_t poll_cycle_total;        // Total time for one full poll loop iteration
    perf_timer_t poll_camera;             // fetch_camera_info_robust duration
    perf_timer_t poll_guider;             // fetch_guider_robust duration
    perf_timer_t poll_mount;              // fetch_mount_robust duration
    perf_timer_t poll_focuser;            // fetch_focuser_robust duration
    perf_timer_t poll_sequence;           // fetch_sequence_counts_optional duration
    perf_timer_t poll_switch;             // fetch_switch_info duration
    perf_timer_t poll_image_history;      // fetch_image_history_robust duration
    perf_timer_t poll_filter;             // fetch_filter_robust_ex duration
    perf_timer_t poll_profile;            // fetch_profile_robust duration

    // UI update timing
    perf_timer_t ui_update_total;         // Total time in bsp_display_lock for UI updates
    perf_timer_t ui_lock_wait;            // Time spent waiting for display lock
    perf_timer_t ui_dashboard_update;     // update_nina_dashboard_page duration
    perf_timer_t ui_summary_update;       // summary_page_update duration

    // Network metrics
    perf_timer_t http_request;            // Individual HTTP request duration (per-request)
    perf_counter_t http_request_count;    // Total HTTP requests per reporting interval
    perf_counter_t http_retry_count;      // HTTP retries per interval
    perf_counter_t http_failure_count;    // HTTP failures per interval
    perf_counter_t ws_event_count;        // WebSocket events received per interval

    // JSON parsing
    perf_timer_t json_parse;              // cJSON_Parse duration (per-parse)
    perf_timer_t json_sequence_parse;     // Sequence JSON specifically (heaviest parse)
    perf_timer_t json_config_color_parse; // app_config_get_filter_color parse timing
    perf_counter_t json_parse_count;      // Total cJSON_Parse calls per interval

    // Memory snapshots (captured at each reporting interval)
    uint32_t heap_free_bytes;
    uint32_t heap_min_free_bytes;         // Minimum ever (highwater mark)
    uint32_t heap_largest_free_block;     // Largest contiguous free block (fragmentation)
    uint32_t psram_free_bytes;
    uint32_t psram_min_free_bytes;
    uint32_t psram_largest_free_block;

    // Task stack highwater marks
    uint32_t data_task_stack_hwm;         // Minimum free stack (bytes)
    uint32_t input_task_stack_hwm;

    // Data freshness latencies (measured from data change to UI update)
    perf_timer_t latency_ws_to_ui;        // Time from WS event receipt to UI refresh
    int64_t      last_ws_event_time_us;   // Timestamp of last WS event (for latency calc)

    // Effective poll cycle interval (actual time between poll starts)
    perf_timer_t effective_cycle_interval;

    // JPEG thumbnail decode timing
    perf_timer_t jpeg_decode;
    perf_timer_t jpeg_fetch;

    // Report control
    int64_t last_report_time_us;
    uint32_t report_interval_s;           // How often to print report (default 30s)
} perf_state_t;

// Global singleton
extern perf_state_t g_perf;

// Initialize profiling state. Call once in app_main().
void perf_monitor_init(uint32_t report_interval_s);

// Capture memory and stack snapshots. Call periodically from data task.
void perf_monitor_capture_memory(void);

// Print a formatted report to serial log. Called automatically at interval.
void perf_monitor_report(void);

// Reset all metrics (call between profiling runs to get clean data).
void perf_monitor_reset_all(void);

// Get report as JSON string (for web API endpoint). Caller must free().
char *perf_monitor_report_json(void);

// Helper to manually record a duration (for intervals measured externally)
void perf_timer_record(perf_timer_t *t, int64_t duration_us);

#else  // !PERF_MONITOR_ENABLED

// No-op stubs when profiling is disabled
#define perf_timer_start(t)           ((void)0)
#define perf_timer_stop(t)            (0)
#define perf_timer_reset(t)           ((void)0)
#define perf_timer_record(t, d)       ((void)0)
#define perf_counter_increment(c)     ((void)0)
#define perf_counter_reset_interval(c)((void)0)
#define perf_monitor_init(i)          ((void)0)
#define perf_monitor_capture_memory() ((void)0)
#define perf_monitor_report()         ((void)0)
#define perf_monitor_reset_all()      ((void)0)
#define perf_monitor_report_json()    (NULL)

#endif
