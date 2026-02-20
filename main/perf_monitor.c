#include "perf_monitor.h"

#if PERF_MONITOR_ENABLED

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "perf";

// Global singleton
perf_state_t g_perf;

// ── Start-time storage ──────────────────────────────────────────────
// Simple hash table to store start times, indexed by pointer hash.
// 32 slots should be sufficient since we never have more than a few
// timers running concurrently.

#define START_TIME_SLOTS 32

typedef struct {
    const perf_timer_t *key;
    int64_t             start_us;
} start_time_entry_t;

static start_time_entry_t s_start_times[START_TIME_SLOTS];

static uint32_t ptr_hash(const perf_timer_t *p)
{
    uintptr_t v = (uintptr_t)p;
    // Simple hash: shift out low bits (alignment) and mask to slot count
    return (uint32_t)((v >> 2) ^ (v >> 14)) & (START_TIME_SLOTS - 1);
}

// ── Timer functions ─────────────────────────────────────────────────

void perf_timer_start(perf_timer_t *t)
{
    int64_t now = esp_timer_get_time();
    uint32_t idx = ptr_hash(t);
    // Linear probe to find an empty slot or existing key
    for (int i = 0; i < START_TIME_SLOTS; i++) {
        uint32_t slot = (idx + i) & (START_TIME_SLOTS - 1);
        if (s_start_times[slot].key == NULL || s_start_times[slot].key == t) {
            s_start_times[slot].key = t;
            s_start_times[slot].start_us = now;
            return;
        }
    }
    // Table full -- should not happen in normal operation
    ESP_LOGW(TAG, "start_time table full, dropping measurement");
}

int64_t perf_timer_stop(perf_timer_t *t)
{
    int64_t now = esp_timer_get_time();
    int64_t start = 0;
    bool found = false;

    uint32_t idx = ptr_hash(t);
    for (int i = 0; i < START_TIME_SLOTS; i++) {
        uint32_t slot = (idx + i) & (START_TIME_SLOTS - 1);
        if (s_start_times[slot].key == t) {
            start = s_start_times[slot].start_us;
            s_start_times[slot].key = NULL;  // Release slot
            found = true;
            break;
        }
        if (s_start_times[slot].key == NULL) {
            break;  // Empty slot means key was never inserted
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "perf_timer_stop called without matching start");
        return 0;
    }

    int64_t elapsed = now - start;
    t->last_us = elapsed;
    t->total_us += elapsed;
    t->count++;
    if (elapsed < t->min_us || t->count == 1) {
        t->min_us = elapsed;
    }
    if (elapsed > t->max_us) {
        t->max_us = elapsed;
    }
    return elapsed;
}

void perf_timer_reset(perf_timer_t *t)
{
    memset(t, 0, sizeof(*t));
}

void perf_timer_record(perf_timer_t *t, int64_t duration_us)
{
    t->last_us = duration_us;
    t->total_us += duration_us;
    t->count++;
    if (duration_us < t->min_us || t->count == 1) {
        t->min_us = duration_us;
    }
    if (duration_us > t->max_us) {
        t->max_us = duration_us;
    }
}

// ── Counter functions ───────────────────────────────────────────────

void perf_counter_increment(perf_counter_t *c)
{
    c->total++;
    c->per_interval++;
}

void perf_counter_reset_interval(perf_counter_t *c)
{
    c->per_interval = 0;
}

// ── Initialization ──────────────────────────────────────────────────

void perf_monitor_init(uint32_t report_interval_s)
{
    memset(&g_perf, 0, sizeof(g_perf));
    memset(s_start_times, 0, sizeof(s_start_times));
    g_perf.report_interval_s = report_interval_s;
    g_perf.last_report_time_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Performance monitor initialized (report every %"PRIu32"s)", report_interval_s);
}

// ── Memory capture ──────────────────────────────────────────────────

void perf_monitor_capture_memory(void)
{
    // Internal heap
    g_perf.heap_free_bytes         = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    g_perf.heap_min_free_bytes     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    g_perf.heap_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    // PSRAM
    g_perf.psram_free_bytes         = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    g_perf.psram_min_free_bytes     = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    g_perf.psram_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // Task stack high-water marks (if task handles are available)
    // These are set externally by the tasks themselves via:
    //   g_perf.data_task_stack_hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    // We just capture what's been set.
}

// ── Serial log report ───────────────────────────────────────────────

static void log_timer(const char *name, const perf_timer_t *t)
{
    if (t->count == 0) {
        ESP_LOGI(TAG, "  %-24s (no samples)", name);
        return;
    }
    double avg_ms = (double)t->total_us / (double)t->count / 1000.0;
    double min_ms = (double)t->min_us / 1000.0;
    double max_ms = (double)t->max_us / 1000.0;
    double last_ms = (double)t->last_us / 1000.0;
    ESP_LOGI(TAG, "  %-24s avg=%7.1f  min=%7.1f  max=%7.1f  last=%7.1f ms  [n=%"PRIu32"]",
             name, avg_ms, min_ms, max_ms, last_ms, t->count);
}

void perf_monitor_report(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║              PERFORMANCE REPORT                             ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════════╝");

    ESP_LOGI(TAG, "── Poll Cycle ──");
    log_timer("poll_cycle_total",   &g_perf.poll_cycle_total);
    log_timer("effective_interval", &g_perf.effective_cycle_interval);

    ESP_LOGI(TAG, "── Endpoint Fetchers ──");
    log_timer("camera",        &g_perf.poll_camera);
    log_timer("guider",        &g_perf.poll_guider);
    log_timer("mount",         &g_perf.poll_mount);
    log_timer("focuser",       &g_perf.poll_focuser);
    log_timer("sequence",      &g_perf.poll_sequence);
    log_timer("switch",        &g_perf.poll_switch);
    log_timer("image_history", &g_perf.poll_image_history);
    log_timer("filter",        &g_perf.poll_filter);
    log_timer("profile",       &g_perf.poll_profile);

    ESP_LOGI(TAG, "── Network ──");
    log_timer("http_request",    &g_perf.http_request);
    ESP_LOGI(TAG, "  HTTP requests:  %"PRIu32" (interval) / %"PRIu32" (total)",
             g_perf.http_request_count.per_interval, g_perf.http_request_count.total);
    ESP_LOGI(TAG, "  HTTP retries:   %"PRIu32" (interval) / %"PRIu32" (total)",
             g_perf.http_retry_count.per_interval, g_perf.http_retry_count.total);
    ESP_LOGI(TAG, "  HTTP failures:  %"PRIu32" (interval) / %"PRIu32" (total)",
             g_perf.http_failure_count.per_interval, g_perf.http_failure_count.total);
    ESP_LOGI(TAG, "  WS events:      %"PRIu32" (interval) / %"PRIu32" (total)",
             g_perf.ws_event_count.per_interval, g_perf.ws_event_count.total);

    ESP_LOGI(TAG, "── JSON Parsing ──");
    log_timer("json_parse",        &g_perf.json_parse);
    log_timer("json_sequence",     &g_perf.json_sequence_parse);
    log_timer("json_config_color", &g_perf.json_config_color_parse);
    ESP_LOGI(TAG, "  Parse calls:    %"PRIu32" (interval) / %"PRIu32" (total)",
             g_perf.json_parse_count.per_interval, g_perf.json_parse_count.total);

    ESP_LOGI(TAG, "── UI Updates ──");
    log_timer("ui_update_total",    &g_perf.ui_update_total);
    log_timer("ui_lock_wait",       &g_perf.ui_lock_wait);
    log_timer("ui_dashboard",       &g_perf.ui_dashboard_update);
    log_timer("ui_summary",         &g_perf.ui_summary_update);

    ESP_LOGI(TAG, "── Latency ──");
    log_timer("ws_to_ui", &g_perf.latency_ws_to_ui);

    ESP_LOGI(TAG, "── JPEG ──");
    log_timer("jpeg_decode", &g_perf.jpeg_decode);
    log_timer("jpeg_fetch",  &g_perf.jpeg_fetch);

    ESP_LOGI(TAG, "── Memory ──");
    ESP_LOGI(TAG, "  Internal heap:  free=%"PRIu32"  min_ever=%"PRIu32"  largest_block=%"PRIu32,
             g_perf.heap_free_bytes, g_perf.heap_min_free_bytes, g_perf.heap_largest_free_block);
    ESP_LOGI(TAG, "  PSRAM:          free=%"PRIu32"  min_ever=%"PRIu32"  largest_block=%"PRIu32,
             g_perf.psram_free_bytes, g_perf.psram_min_free_bytes, g_perf.psram_largest_free_block);

    ESP_LOGI(TAG, "── Task Stacks ──");
    ESP_LOGI(TAG, "  data_task HWM:  %"PRIu32" bytes free", g_perf.data_task_stack_hwm);
    ESP_LOGI(TAG, "  input_task HWM: %"PRIu32" bytes free", g_perf.input_task_stack_hwm);

    // Reset per-interval counters
    perf_counter_reset_interval(&g_perf.http_request_count);
    perf_counter_reset_interval(&g_perf.http_retry_count);
    perf_counter_reset_interval(&g_perf.http_failure_count);
    perf_counter_reset_interval(&g_perf.ws_event_count);
    perf_counter_reset_interval(&g_perf.json_parse_count);

    g_perf.last_report_time_us = esp_timer_get_time();
}

// ── Reset all ───────────────────────────────────────────────────────

void perf_monitor_reset_all(void)
{
    uint32_t interval = g_perf.report_interval_s;
    memset(&g_perf, 0, sizeof(g_perf));
    g_perf.report_interval_s = interval;
    g_perf.last_report_time_us = esp_timer_get_time();
    ESP_LOGI(TAG, "All performance metrics reset");
}

// ── JSON report ─────────────────────────────────────────────────────

static cJSON *timer_to_json(const perf_timer_t *t)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    if (t->count == 0) {
        cJSON_AddNumberToObject(obj, "avg_ms", 0);
        cJSON_AddNumberToObject(obj, "min_ms", 0);
        cJSON_AddNumberToObject(obj, "max_ms", 0);
        cJSON_AddNumberToObject(obj, "last_ms", 0);
        cJSON_AddNumberToObject(obj, "count", 0);
    } else {
        cJSON_AddNumberToObject(obj, "avg_ms", (double)t->total_us / (double)t->count / 1000.0);
        cJSON_AddNumberToObject(obj, "min_ms", (double)t->min_us / 1000.0);
        cJSON_AddNumberToObject(obj, "max_ms", (double)t->max_us / 1000.0);
        cJSON_AddNumberToObject(obj, "last_ms", (double)t->last_us / 1000.0);
        cJSON_AddNumberToObject(obj, "count", t->count);
    }
    return obj;
}

static cJSON *counter_to_json(const perf_counter_t *c)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddNumberToObject(obj, "total", c->total);
    cJSON_AddNumberToObject(obj, "per_interval", c->per_interval);
    return obj;
}

char *perf_monitor_report_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    // Uptime
    int64_t now = esp_timer_get_time();
    cJSON_AddNumberToObject(root, "uptime_s", (double)now / 1000000.0);
    cJSON_AddNumberToObject(root, "report_interval_s", g_perf.report_interval_s);

    // Poll cycle
    cJSON_AddItemToObject(root, "poll_cycle", timer_to_json(&g_perf.poll_cycle_total));
    cJSON_AddItemToObject(root, "effective_interval", timer_to_json(&g_perf.effective_cycle_interval));

    // Endpoints
    cJSON *endpoints = cJSON_CreateObject();
    cJSON_AddItemToObject(endpoints, "camera_info",    timer_to_json(&g_perf.poll_camera));
    cJSON_AddItemToObject(endpoints, "guider_info",    timer_to_json(&g_perf.poll_guider));
    cJSON_AddItemToObject(endpoints, "mount_info",     timer_to_json(&g_perf.poll_mount));
    cJSON_AddItemToObject(endpoints, "focuser_info",   timer_to_json(&g_perf.poll_focuser));
    cJSON_AddItemToObject(endpoints, "sequence_json",  timer_to_json(&g_perf.poll_sequence));
    cJSON_AddItemToObject(endpoints, "switch_info",    timer_to_json(&g_perf.poll_switch));
    cJSON_AddItemToObject(endpoints, "image_history",  timer_to_json(&g_perf.poll_image_history));
    cJSON_AddItemToObject(endpoints, "filter_info",    timer_to_json(&g_perf.poll_filter));
    cJSON_AddItemToObject(endpoints, "profile_show",   timer_to_json(&g_perf.poll_profile));
    cJSON_AddItemToObject(root, "endpoints", endpoints);

    // Network
    cJSON *network = cJSON_CreateObject();
    cJSON_AddItemToObject(network, "http_request",      timer_to_json(&g_perf.http_request));
    cJSON_AddItemToObject(network, "http_request_count", counter_to_json(&g_perf.http_request_count));
    cJSON_AddItemToObject(network, "http_retry_count",   counter_to_json(&g_perf.http_retry_count));
    cJSON_AddItemToObject(network, "http_failure_count", counter_to_json(&g_perf.http_failure_count));
    cJSON_AddItemToObject(network, "ws_event_count",     counter_to_json(&g_perf.ws_event_count));
    cJSON_AddItemToObject(root, "network", network);

    // JSON parsing
    cJSON *json_parsing = cJSON_CreateObject();
    cJSON_AddItemToObject(json_parsing, "json_parse",        timer_to_json(&g_perf.json_parse));
    cJSON_AddItemToObject(json_parsing, "json_sequence",     timer_to_json(&g_perf.json_sequence_parse));
    cJSON_AddItemToObject(json_parsing, "json_config_color", timer_to_json(&g_perf.json_config_color_parse));
    cJSON_AddItemToObject(json_parsing, "json_parse_count",  counter_to_json(&g_perf.json_parse_count));
    cJSON_AddItemToObject(root, "json_parsing", json_parsing);

    // UI
    cJSON *ui = cJSON_CreateObject();
    cJSON_AddItemToObject(ui, "ui_update_total",    timer_to_json(&g_perf.ui_update_total));
    cJSON_AddItemToObject(ui, "ui_lock_wait",       timer_to_json(&g_perf.ui_lock_wait));
    cJSON_AddItemToObject(ui, "ui_dashboard",       timer_to_json(&g_perf.ui_dashboard_update));
    cJSON_AddItemToObject(ui, "ui_summary",         timer_to_json(&g_perf.ui_summary_update));
    cJSON_AddItemToObject(ui, "latency_ws_to_ui",   timer_to_json(&g_perf.latency_ws_to_ui));
    cJSON_AddItemToObject(root, "ui", ui);

    // Memory
    cJSON *memory = cJSON_CreateObject();
    cJSON_AddNumberToObject(memory, "heap_free_bytes",         g_perf.heap_free_bytes);
    cJSON_AddNumberToObject(memory, "heap_min_free_bytes",     g_perf.heap_min_free_bytes);
    cJSON_AddNumberToObject(memory, "heap_largest_free_block", g_perf.heap_largest_free_block);
    cJSON_AddNumberToObject(memory, "psram_free_bytes",         g_perf.psram_free_bytes);
    cJSON_AddNumberToObject(memory, "psram_min_free_bytes",     g_perf.psram_min_free_bytes);
    cJSON_AddNumberToObject(memory, "psram_largest_free_block", g_perf.psram_largest_free_block);
    cJSON_AddItemToObject(root, "memory", memory);

    // Tasks
    cJSON *tasks = cJSON_CreateObject();
    cJSON_AddNumberToObject(tasks, "data_task_stack_hwm",  g_perf.data_task_stack_hwm);
    cJSON_AddNumberToObject(tasks, "input_task_stack_hwm", g_perf.input_task_stack_hwm);
    cJSON_AddItemToObject(root, "tasks", tasks);

    // JPEG
    cJSON *jpeg = cJSON_CreateObject();
    cJSON_AddItemToObject(jpeg, "jpeg_decode", timer_to_json(&g_perf.jpeg_decode));
    cJSON_AddItemToObject(jpeg, "jpeg_fetch",  timer_to_json(&g_perf.jpeg_fetch));
    cJSON_AddItemToObject(root, "jpeg", jpeg);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

#endif  // PERF_MONITOR_ENABLED
