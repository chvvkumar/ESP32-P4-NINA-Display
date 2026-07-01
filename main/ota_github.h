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
    OTA_CHECK_ERROR,            /* transient: network/rate-limit/unverifiable — retry, NOT up-to-date, NOT manual-flash */
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
 *         OTA_CHECK_ERROR on a transient failure (network/rate-limit/unverifiable
 *         history) — the caller should retry and must not treat this as up-to-date
 *         or as a manual-flash requirement.
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
