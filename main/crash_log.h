#pragma once

/**
 * @file crash_log.h
 * @brief Persistent crash history capture and storage.
 *
 * Two capture layers survive a reboot:
 *   - Layer A (reset reason): on every boot crash_log_init() inspects the latched
 *     reset reason (via power_mgmt). Abnormal resets (panic, watchdog, brownout)
 *     are recorded. This works for every crash type including power loss.
 *   - Layer B (panic text): crash_log_init() reads the core dump's panic-detail
 *     note back via esp_core_dump_get_panic_reason() and attaches it to the
 *     Layer-A record. Present for abort()/assert failures (the abort detail
 *     string) and task-watchdog panics (the triggered-task list); generic faults
 *     carry no detail note, so their record has an empty "panic" field and the
 *     full ELF core dump at GET /api/coredump is the diagnostic path.
 *
 * Records are appended one JSON object per line to /spiffs/crashlog.jsonl. The
 * file is capped to the newest CRASH_LOG_MAX_ENTRIES lines and purged of entries
 * older than the configured retention window.
 *
 * The web layer (web_handlers_*) consumes this module through the public helpers
 * below — it never opens the file directly.
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* littlefs mount point (historical "/spiffs" name) and crash-history file path. */
#define CRASH_LOG_MOUNT_POINT "/spiffs"
#define CRASH_LOG_FILE_PATH   CRASH_LOG_MOUNT_POINT "/crashlog.jsonl"

/* Ring size: newest N crash records are retained, oldest dropped. */
#define CRASH_LOG_MAX_ENTRIES 20

/**
 * Mount littlefs, record a crash entry if this boot followed an abnormal reset,
 * enforce the ring + retention, then clear the RTC panic buffer.
 *
 * Call once from app_main() after app_config_init() and after
 * power_mgmt_check_crash(). Reads crash_log_retention_days from app_config.
 * Safe to call even if the mount fails — the feature degrades to a no-op and
 * logs a warning rather than aborting boot.
 */
void crash_log_init(void);

/**
 * Open the crash-history file for reading.
 *
 * @return FILE* positioned at the start of /spiffs/crashlog.jsonl, or NULL if the
 *         file does not exist (no crashes yet) or the fs is unmounted. The caller
 *         owns the handle and must fclose() it.
 */
FILE *crash_log_open_read(void);

/**
 * Delete the crash-history file. Idempotent — returns ESP_OK whether or not the
 * file existed.
 */
esp_err_t crash_log_clear(void);

/**
 * Drop crash records older than @p days (wall-clock based; records without a
 * valid wall-clock timestamp are always kept). @p days == 0 means "never purge"
 * and is a no-op. Called on boot from crash_log_init() and once daily from the
 * data task tick.
 */
void crash_log_purge_old(uint8_t days);

#ifdef __cplusplus
}
#endif
