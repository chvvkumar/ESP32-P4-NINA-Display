#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char tag[32];              // e.g., "v1.0.14"
    char summary[1024];        // Release summary text (just the description, no commit details)
    char ota_url[2048];        // Pre-signed S3 URL can be ~1KB with auth tokens
    bool is_prerelease;
    bool requires_full_erase;  // Release requires manual USB erase+flash (cannot OTA)
    char full_erase_tag[32];   // Newest release tag on the install path carrying the full-erase
                               // marker; empty when no path release is marked, or when the
                               // fail-safe fired on an unverifiable update history.
} github_release_info_t;

typedef enum {
    OTA_CHECK_UP_TO_DATE = 0,   /* definitive: no newer release available */
    OTA_CHECK_UPDATE_AVAILABLE, /* *out filled with the target release */
    OTA_CHECK_ERROR,            /* transient: network/unverifiable — retry, NOT up-to-date, NOT manual-flash */
    OTA_CHECK_RATE_LIMITED,     /* GitHub answered 403/429: quota exhausted, back off ~1 h */
} ota_check_result_t;

/**
 * Check GitHub for a newer firmware release.
 * @param channel Update channel: 0 = Stable (released builds), 1 = Pre-releases
 *                / Beta (excluding the Alpha snd-alpha release), 2 = Alpha (snd)
 *                (only the rolling snd-alpha pre-release).
 * @param current_version Current firmware version string (e.g., "1.0.12")
 * @param out Filled with release info when the result is OTA_CHECK_UPDATE_AVAILABLE
 * @return OTA_CHECK_UPDATE_AVAILABLE when a newer release is available (*out filled);
 *         OTA_CHECK_UP_TO_DATE when definitively on the latest release;
 *         OTA_CHECK_ERROR on a transient failure (network/unverifiable
 *         history) — the caller should retry and must not treat this as up-to-date
 *         or as a manual-flash requirement;
 *         OTA_CHECK_RATE_LIMITED when GitHub rejected a page fetch with 403/429 —
 *         same "not up-to-date, not manual-flash" contract as ERROR, but the caller
 *         must back off for roughly an hour instead of retrying in a minute.
 */
ota_check_result_t ota_github_check(int channel, const char *current_version, github_release_info_t *out);

/**
 * Download and flash an OTA binary from a URL.
 * Streams the binary in 4KB chunks directly to the OTA partition.
 * @param url URL of the OTA binary (e.g., GitHub release asset URL)
 * @param progress_cb Called with percentage 0-100 during download. May be NULL.
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ota_github_download(const char *url, void (*progress_cb)(int percent));

/**
 * Arm the boot-time rollback confirm guard. Call once, early in app_main
 * (after NVS init, before the web server starts serving OTA requests).
 * Captures whether the running image is on its first boot after an OTA
 * (ESP_OTA_IMG_PENDING_VERIFY) and, if so, spawns a small internal-stack task
 * that marks the image valid once boot is confirmed healthy (display and
 * network milestones reported) or, as a fail-safe, after an uptime fallback,
 * so no path leaves the image pending forever. The return value of the
 * mark-valid call is checked and retried on failure. A normal boot spawns
 * nothing.
 */
void ota_github_boot_guard_init(void);

/**
 * True if the running image was in ESP_OTA_IMG_PENDING_VERIFY at
 * ota_github_boot_guard_init() time (first boot of a freshly OTA'd image).
 * Use this instead of re-reading the partition state later in boot: the guard
 * may already have confirmed the image by then.
 */
bool ota_github_image_was_pending(void);

/** Boot-health milestone: display initialized. Idempotent, ISR-unsafe. */
void ota_github_note_display_ready(void);

/** Boot-health milestone: station network up (got IP). Idempotent. */
void ota_github_note_network_ready(void);

/**
 * Ensure the running image does not block an OTA (esp_ota_begin refuses with
 * ESP_ERR_OTA_ROLLBACK_INVALID_STATE while it is pending verification).
 * If the image is still pending, confirm it now: the device is demonstrably
 * up if it can service an update request. Returns ESP_OK when an update can
 * proceed, else the mark-valid error. Performs a flash write on the pending
 * path: callers must run on an internal-RAM stack (httpd workers and the
 * ota_dl task qualify; PSRAM-stack tasks must not call this).
 */
esp_err_t ota_github_ensure_can_update(void);

/**
 * Record the release tag an OTA update intends to install (pending state).
 * Stamped at apply time, before reboot. Promoted to the confirmed installed
 * version only after that image actually boots (see ota_github_reconcile_version).
 */
void ota_github_save_pending_version(const char *tag);

/**
 * Reconcile OTA version state once at boot.
 * If first_boot_new_image is true, promote the pending OTA tag to the confirmed
 * installed version (bound to the running build); otherwise discard a stale
 * pending stamp left by an OTA image that never booted (rollback/slot mismatch).
 */
void ota_github_reconcile_version(bool first_boot_new_image);

/**
 * Get the effective current version for OTA comparison.
 * Returns the NVS-stored OTA version only if it belongs to the running build,
 * otherwise BUILD_GIT_TAG.
 * The returned pointer is valid until the next call to this function.
 */
const char *ota_github_get_current_version(void);

#ifdef __cplusplus
}
#endif
