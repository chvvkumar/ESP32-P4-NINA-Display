/**
 * @file crash_log.c
 * @brief Persistent crash history capture (see crash_log.h).
 *
 * Storage: one JSON object per line in /spiffs/crashlog.jsonl. JSONL keeps the
 * append-and-trim path trivial and streams directly to the web Logs tab.
 *
 * Record shape (all fields always present):
 *   {"ts":1717459200,"uptime_s":0,"reason":4,"reason_str":"Panic / exception",
 *    "crash_count":3,"boot_count":42,"panic":"Guru Meditation Error...\n..."}
 *   - ts:          wall-clock unix seconds at capture, 0 if NTP not synced.
 *   - uptime_s:    seconds since boot at capture (always meaningful).
 *   - reason:      raw esp_reset_reason_t value.
 *   - reason_str:  human-readable reason string.
 *   - crash_count: RTC crash counter since last power-on.
 *   - boot_count:  total boot count from NVS.
 *   - panic:       panic detail text read back from the core dump (abort/assert
 *                  message, or the task-watchdog triggered-task list), "" if the
 *                  crash produced no core dump or carried no detail note.
 */

#include "crash_log.h"
#include "power_mgmt.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "crash_log";

/* ── Layer B: panic detail text ───────────────────────────────────────────────
 *
 * Read back from the core dump at boot via esp_core_dump_get_panic_reason().
 *
 * The previous implementation mirrored the serial panic output by linking with
 * -Wl,--wrap=panic_print_char into an RTC_NOINIT ring. That never worked and
 * could not: every call to panic_print_char() lives inside esp_system/panic.c,
 * the same translation unit that defines it, and --wrap only rewrites
 * cross-TU references. The wrapper was linked but never reached.
 *
 * The supported replacement is the core dump's ESP_PANIC_DETAILS note, which
 * the panic handler writes for abort()/assert failures (the abort detail
 * string) and for task-watchdog panics (the triggered-task list). It requires
 * CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH + CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF,
 * both already set for this project. Generic faults (LoadStoreFault and
 * friends) carry no detail note; for those the record's reason_str plus the
 * full ELF core dump at GET /api/coredump remain the diagnostic path.
 */
#define CRASH_PANIC_TEXT_MAX  512

/* ── littlefs mount ───────────────────────────────────────────────────────── */

static bool s_mounted = false;

static bool ensure_mounted(void)
{
    if (s_mounted) {
        return true;
    }

    /* The "crashlog" partition (128KB) ships unformatted (or holds a SPIFFS
     * image from older firmware); either fails the mount and is auto-formatted.
     * littlefs formats lazily (superblocks only), so this completes quickly. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = CRASH_LOG_MOUNT_POINT,
        .partition_label        = "crashlog",
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_OK) {
        s_mounted = true;
        size_t total = 0, used = 0;
        if (esp_littlefs_info("crashlog", &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "littlefs mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
        }
        return true;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        /* Already registered by something else — treat as mounted. */
        s_mounted = true;
        return true;
    }

    ESP_LOGE(TAG, "littlefs mount failed: %s — crash logging disabled this boot",
             esp_err_to_name(err));
    return false;
}

/* ── File helpers ─────────────────────────────────────────────────────────── */

FILE *crash_log_open_read(void)
{
    if (!s_mounted) {
        return NULL;
    }
    return fopen(CRASH_LOG_FILE_PATH, "r");
}

esp_err_t crash_log_clear(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    if (remove(CRASH_LOG_FILE_PATH) != 0) {
        /* Missing file is not an error — clearing an empty log is a no-op. */
        struct stat st;
        if (stat(CRASH_LOG_FILE_PATH, &st) == 0) {
            ESP_LOGW(TAG, "Failed to remove crash log");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Crash log cleared");
    return ESP_OK;
}

/**
 * Rewrite the crash log keeping only the lines for which keep_fn() returns true,
 * limited to the newest @p max_keep entries. Reads the whole file into a PSRAM
 * buffer, filters, and writes back. The file is small (≤ a few KB), so this is
 * cheap and avoids partial-write corruption.
 *
 * @param max_keep   newest N lines to retain (0 = unlimited beyond keep_fn)
 * @param min_ts     drop lines whose "ts" is non-zero and < min_ts (0 = no cutoff)
 */
static void rewrite_filtered(size_t max_keep, time_t min_ts)
{
    if (!s_mounted) {
        return;
    }

    struct stat st;
    if (stat(CRASH_LOG_FILE_PATH, &st) != 0 || st.st_size == 0) {
        return;  /* nothing to do */
    }

    size_t sz = (size_t)st.st_size;
    char *data = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
    if (!data) {
        ESP_LOGE(TAG, "OOM reading crash log (%u bytes)", (unsigned)sz);
        return;
    }

    FILE *f = fopen(CRASH_LOG_FILE_PATH, "r");
    if (!f) {
        heap_caps_free(data);
        return;
    }
    size_t got = fread(data, 1, sz, f);
    fclose(f);
    data[got] = '\0';

    /* Collect line start pointers (in place; replace '\n' with '\0'). A generous
     * cap guards against an unexpectedly large file; in practice the ring keeps
     * the file at ≤ CRASH_LOG_MAX_ENTRIES + 1 lines. */
    #define CRASH_LOG_PARSE_CAP 256
    char **lines = heap_caps_malloc(sizeof(char *) * CRASH_LOG_PARSE_CAP, MALLOC_CAP_SPIRAM);
    if (!lines) {
        heap_caps_free(data);
        return;
    }
    size_t n = 0;
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (*p != '\0' && n < CRASH_LOG_PARSE_CAP) {
            /* Apply retention cutoff: parse "ts" cheaply via cJSON. */
            bool keep = true;
            if (min_ts > 0) {
                cJSON *o = cJSON_Parse(p);
                if (o) {
                    cJSON *ts = cJSON_GetObjectItem(o, "ts");
                    if (cJSON_IsNumber(ts) && ts->valuedouble > 0 &&
                        (time_t)ts->valuedouble < min_ts) {
                        keep = false;
                    }
                    cJSON_Delete(o);
                }
            }
            if (keep) {
                lines[n++] = p;
            }
        }
        p = nl ? (nl + 1) : NULL;
    }

    /* Trim to newest max_keep lines. */
    size_t first = 0;
    if (max_keep > 0 && n > max_keep) {
        first = n - max_keep;
    }

    FILE *out = fopen(CRASH_LOG_FILE_PATH, "w");
    if (out) {
        for (size_t i = first; i < n; i++) {
            fputs(lines[i], out);
            fputc('\n', out);
        }
        fclose(out);
    } else {
        ESP_LOGE(TAG, "Failed to rewrite crash log");
    }

    heap_caps_free(lines);
    heap_caps_free(data);
}

void crash_log_purge_old(uint8_t days)
{
    if (days == 0 || !s_mounted) {
        return;  /* 0 = never purge */
    }

    time_t now = time(NULL);
    if (now < 1577836800) {  /* Jan 1 2020 — clock not yet set, skip purge */
        return;
    }
    time_t cutoff = now - (time_t)days * 86400;
    rewrite_filtered(0 /* keep ring as-is */, cutoff);
}

/* ── Record append ────────────────────────────────────────────────────────── */

static void append_crash_record(uint32_t reason)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }

    time_t now = time(NULL);
    bool synced = (now >= 1577836800);  /* Jan 1 2020 */
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);  /* s since boot, wrap-safe */

    power_mgmt_crash_info_t info = power_mgmt_get_crash_info();

    cJSON_AddNumberToObject(o, "ts", synced ? (double)now : 0.0);
    cJSON_AddNumberToObject(o, "uptime_s", (double)uptime_s);
    cJSON_AddNumberToObject(o, "reason", (double)reason);
    cJSON_AddStringToObject(o, "reason_str", power_mgmt_reset_reason_str(reason));
    cJSON_AddNumberToObject(o, "crash_count", (double)info.crash_count);
    cJSON_AddNumberToObject(o, "boot_count", (double)info.boot_count);

    /* Whether an ELF core dump was saved to the coredump partition for this
     * crash. esp_core_dump_image_get() returns ESP_OK only when a valid image
     * is present in flash. */
    size_t cd_addr = 0, cd_size = 0;
    bool coredump_present = (esp_core_dump_image_get(&cd_addr, &cd_size) == ESP_OK);
    cJSON_AddBoolToObject(o, "coredump_present", coredump_present);

    /* Panic detail text, from the core dump's ESP_PANIC_DETAILS note. Only read
     * when an image is present; like coredump_present itself this can describe a
     * previous panic if THIS reset produced no dump (brownout after a panic) and
     * the older image was never erased via GET /api/coredump. */
    char panic_text[CRASH_PANIC_TEXT_MAX] = {0};
    if (coredump_present) {
        (void)esp_core_dump_get_panic_reason(panic_text, sizeof(panic_text));
    }
    cJSON_AddStringToObject(o, "panic", panic_text);

    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!line) {
        return;
    }

    FILE *f = fopen(CRASH_LOG_FILE_PATH, "a");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
        ESP_LOGW(TAG, "Recorded crash: reason=%lu (%s)",
                 (unsigned long)reason, power_mgmt_reset_reason_str(reason));
    } else {
        ESP_LOGE(TAG, "Failed to append crash record");
    }

    free(line);

    /* Enforce the ring (newest CRASH_LOG_MAX_ENTRIES). */
    rewrite_filtered(CRASH_LOG_MAX_ENTRIES, 0);
}

/* ── Public init ──────────────────────────────────────────────────────────── */

/* State captured by crash_log_init() and consumed by the deferred worker. The
 * reset reason is latched synchronously; all flash work (littlefs mount, core
 * dump read, record write) is deferred. */
static bool      s_crash_pending = false;     /* a crash record is waiting to be written */
static uint32_t  s_pending_reason = 0;
static crash_log_summary_t s_summary;         /* filled once by the worker, read-only after */

const crash_log_summary_t *crash_log_get_summary(void)
{
    return &s_summary;
}

/* Read the coredump summary + panic note into s_summary. Flash mmap inside:
 * internal-RAM stack only (see crash_log_deferred_worker). */
static void cache_core_summary(void)
{
    size_t cd_addr = 0, cd_size = 0;
    if (esp_core_dump_image_get(&cd_addr, &cd_size) != ESP_OK) {
        return;
    }
    esp_core_dump_summary_t *sum = heap_caps_calloc(1, sizeof(*sum), MALLOC_CAP_SPIRAM);
    if (sum) {
        if (esp_core_dump_get_summary(sum) == ESP_OK) {
            strlcpy(s_summary.task, sum->exc_task, sizeof(s_summary.task));
            s_summary.pc = sum->exc_pc;
        }
        heap_caps_free(sum);
    }
    (void)esp_core_dump_get_panic_reason(s_summary.detail, sizeof(s_summary.detail));
    s_summary.valid = true;
}

/**
 * One-shot background worker: performs the littlefs mount/format off the boot
 * critical path (littlefs formats lazily, so even a first-boot format is
 * quick — the old ~70 s SPIFFS ordeal is gone), then writes any pending crash
 * record and runs the retention purge. Self-deletes when done.
 *
 * Its stack MUST live in internal RAM. Format/write issues flash erase/write
 * operations that execute with the CPU data cache disabled; a stack in
 * (cached) PSRAM would fault when touched during those operations. Plain
 * xTaskCreate() allocates the stack in internal RAM — do NOT switch this to a
 * PSRAM/static-PSRAM stack or xTaskCreateWithCaps(MALLOC_CAP_SPIRAM).
 */
static void crash_log_deferred_worker(void *arg)
{
    (void)arg;

    /* Before the mount: the summary cache must exist even when littlefs is
     * unavailable, because telemetry reads it in place of the flash. */
    if (s_crash_pending) {
        cache_core_summary();
    }

    if (!ensure_mounted()) {
        s_crash_pending = false;
        vTaskDelete(NULL);
        return;
    }

    if (s_crash_pending) {
        append_crash_record(s_pending_reason);
        s_crash_pending = false;
    } else {
        ESP_LOGI(TAG, "Boot reset reason: %s (no crash recorded)",
                 power_mgmt_reset_reason_str(s_pending_reason));
    }

    /* Retention purge on boot. */
    crash_log_purge_old(app_config_get()->crash_log_retention_days);

    vTaskDelete(NULL);
}

void crash_log_init(void)
{
    /* Latch the reset reason NOW — an instant read. Everything that touches
     * flash (littlefs mount/format, core dump read, record write) is deferred to
     * a background task so it never blocks app_main(). */
    uint32_t reason = power_mgmt_get_last_reset_reason();
    s_pending_reason = reason;
    s_crash_pending  = power_mgmt_reset_is_abnormal(reason);

    /* Defer littlefs mount/format + record write to a background task. Internal-RAM
     * stack is mandatory (see crash_log_deferred_worker). Low priority on Core 0
     * keeps it out of the way of UI/network bring-up. */
    xTaskCreatePinnedToCore(crash_log_deferred_worker, "crash_log_def",
                            10240, NULL, tskIDLE_PRIORITY + 2, NULL, 0);
}
