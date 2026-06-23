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
 *   - panic:       captured serial panic text (Layer B), "" if unavailable.
 */

#include "crash_log.h"
#include "power_mgmt.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "crash_log";

/* ── Layer B: panic-text capture ──────────────────────────────────────────────
 *
 * __wrap_panic_print_char() runs in panic context (interrupts disabled, no heap,
 * no locks). It appends each character to a bounded RTC_NOINIT ring guarded by a
 * magic value, then forwards to the real implementation so serial output is
 * unchanged. RTC_NOINIT memory survives the panic-triggered software reset.
 *
 * The ring stores the most recent CRASH_PANIC_BUF_SIZE characters; if the panic
 * banner overflows we keep the tail (which holds the backtrace line on RISC-V).
 */
#define CRASH_PANIC_BUF_SIZE  3072
#define CRASH_PANIC_MAGIC     0xC7A54106u   /* "crash log" sentinel */

typedef struct {
    uint32_t magic;                     /* CRASH_PANIC_MAGIC when buffer valid */
    uint32_t len;                       /* bytes written (may exceed size if wrapped) */
    uint32_t head;                      /* next write index into buf[] */
    char     buf[CRASH_PANIC_BUF_SIZE];
} crash_panic_rtc_t;

static RTC_NOINIT_ATTR crash_panic_rtc_t s_panic_rtc;

/* Provided by the linker (-Wl,--wrap=panic_print_char). */
extern void __real_panic_print_char(char c);

void __wrap_panic_print_char(char c)
{
    /* First character of a fresh panic: initialise the ring. We cannot tell the
     * difference between a continuation and a new panic here, so we only reset
     * when the magic is absent — crash_log_init() clears the magic after reading,
     * so the next crash starts clean. */
    if (s_panic_rtc.magic != CRASH_PANIC_MAGIC) {
        s_panic_rtc.magic = CRASH_PANIC_MAGIC;
        s_panic_rtc.len   = 0;
        s_panic_rtc.head  = 0;
    }

    s_panic_rtc.buf[s_panic_rtc.head] = c;
    s_panic_rtc.head = (s_panic_rtc.head + 1u) % CRASH_PANIC_BUF_SIZE;
    s_panic_rtc.len++;

    __real_panic_print_char(c);
}

/**
 * Copy the captured panic text (oldest→newest) into @p out. Returns the number
 * of characters written (excluding NUL). Returns 0 and out[0]='\0' if no valid
 * capture is present.
 */
static size_t panic_text_extract(char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (s_panic_rtc.magic != CRASH_PANIC_MAGIC || s_panic_rtc.len == 0) {
        return 0;
    }

    size_t total = s_panic_rtc.len;
    size_t start;
    if (total <= CRASH_PANIC_BUF_SIZE) {
        /* No wrap — buffer holds [0 .. head). */
        start = 0;
    } else {
        /* Wrapped — oldest char is at head, newest CRASH_PANIC_BUF_SIZE chars kept. */
        total = CRASH_PANIC_BUF_SIZE;
        start = s_panic_rtc.head;
    }

    size_t copy = total < (out_size - 1) ? total : (out_size - 1);
    for (size_t i = 0; i < copy; i++) {
        size_t idx = (start + i) % CRASH_PANIC_BUF_SIZE;
        out[i] = s_panic_rtc.buf[idx];
    }
    out[copy] = '\0';
    return copy;
}

/* Invalidate the RTC panic ring so the next crash captures fresh text. */
static void panic_text_clear(void)
{
    s_panic_rtc.magic = 0;
    s_panic_rtc.len   = 0;
    s_panic_rtc.head  = 0;
}

/* ── SPIFFS mount ─────────────────────────────────────────────────────────── */

static bool s_mounted = false;

static bool ensure_mounted(void)
{
    if (s_mounted) {
        return true;
    }

    /* The "crashlog" partition (128KB) ships unformatted; format on first mount.
     * It is small, so the format completes quickly. */
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = CRASH_LOG_MOUNT_POINT,
        .partition_label        = "crashlog",
        .max_files              = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        s_mounted = true;
        size_t total = 0, used = 0;
        if (esp_spiffs_info("crashlog", &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
        }
        return true;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        /* Already registered by something else — treat as mounted. */
        s_mounted = true;
        return true;
    }

    ESP_LOGE(TAG, "SPIFFS mount failed: %s — crash logging disabled this boot",
             esp_err_to_name(err));
    return false;
}

/* ── File helpers ─────────────────────────────────────────────────────────── */

bool crash_log_exists(void)
{
    if (!s_mounted) {
        return false;
    }
    struct stat st;
    return stat(CRASH_LOG_FILE_PATH, &st) == 0 && st.st_size > 0;
}

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

static void append_crash_record(uint32_t reason, const char *panic_text)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }

    time_t now = time(NULL);
    bool synced = (now >= 1577836800);  /* Jan 1 2020 */
    uint32_t uptime_ms = esp_log_timestamp();  /* ms since boot */

    power_mgmt_crash_info_t info = power_mgmt_get_crash_info();

    cJSON_AddNumberToObject(o, "ts", synced ? (double)now : 0.0);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(uptime_ms / 1000u));
    cJSON_AddNumberToObject(o, "reason", (double)reason);
    cJSON_AddStringToObject(o, "reason_str", power_mgmt_reset_reason_str(reason));
    cJSON_AddNumberToObject(o, "crash_count", (double)info.crash_count);
    cJSON_AddNumberToObject(o, "boot_count", (double)info.boot_count);
    cJSON_AddStringToObject(o, "panic", panic_text ? panic_text : "");

    /* Whether an ELF core dump was saved to the coredump partition for this
     * crash. esp_core_dump_image_get() returns ESP_OK only when a valid image
     * is present in flash. */
    size_t cd_addr = 0, cd_size = 0;
    bool coredump_present = (esp_core_dump_image_get(&cd_addr, &cd_size) == ESP_OK);
    cJSON_AddBoolToObject(o, "coredump_present", coredump_present);

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
 * RTC panic text must be extracted and the ring cleared synchronously in
 * crash_log_init() (RTC reads are instant); SPIFFS work is deferred. */
static bool      s_crash_pending = false;     /* a crash record is waiting to be written */
static uint32_t  s_pending_reason = 0;
static char     *s_pending_panic = NULL;      /* PSRAM buffer, owned by the worker */

/**
 * One-shot background worker: performs the (potentially ~70s on first boot)
 * SPIFFS mount/format off the boot critical path, then writes any pending crash
 * record and runs the retention purge. Self-deletes when done.
 *
 * Its stack MUST live in internal RAM. SPIFFS format issues many flash
 * erase/write operations that execute with the CPU data cache disabled; a stack
 * in (cached) PSRAM would fault when touched during those operations. Plain
 * xTaskCreate() allocates the stack in internal RAM — do NOT switch this to a
 * PSRAM/static-PSRAM stack or xTaskCreateWithCaps(MALLOC_CAP_SPIRAM).
 */
static void crash_log_deferred_worker(void *arg)
{
    (void)arg;

    if (!ensure_mounted()) {
        /* Mount failed — drop any pending capture so we don't leak it. */
        if (s_pending_panic) {
            heap_caps_free(s_pending_panic);
            s_pending_panic = NULL;
        }
        s_crash_pending = false;
        vTaskDelete(NULL);
        return;
    }

    if (s_crash_pending) {
        append_crash_record(s_pending_reason, s_pending_panic);
        if (s_pending_panic) {
            heap_caps_free(s_pending_panic);
            s_pending_panic = NULL;
        }
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
    /* Read the reset reason and capture panic text from the RTC ring NOW — these
     * are instant reads. The SPIFFS mount/format (which can take ~70s on first
     * boot) is deferred to a background task so it never blocks app_main(). */
    uint32_t reason = power_mgmt_get_last_reset_reason();
    s_pending_reason = reason;

    if (power_mgmt_reset_is_abnormal(reason)) {
        /* Pull captured panic text (Layer B) from the RTC ring, if present.
         * This MUST happen before panic_text_clear() below. */
        char *panic_text = heap_caps_malloc(CRASH_PANIC_BUF_SIZE + 1, MALLOC_CAP_SPIRAM);
        if (panic_text) {
            panic_text_extract(panic_text, CRASH_PANIC_BUF_SIZE + 1);
        }
        s_pending_panic = panic_text;   /* may be NULL on OOM; worker tolerates it */
        s_crash_pending = true;
    }

    /* Clear the RTC panic ring so the next crash captures fresh text. Safe to do
     * now: any text we cared about is already copied into s_pending_panic. */
    panic_text_clear();

    /* Defer SPIFFS mount/format + record write to a background task. Internal-RAM
     * stack is mandatory (see crash_log_deferred_worker). Low priority on Core 0
     * keeps it out of the way of UI/network bring-up. */
    xTaskCreatePinnedToCore(crash_log_deferred_worker, "crash_log_def",
                            8192, NULL, tskIDLE_PRIORITY + 2, NULL, 0);
}
