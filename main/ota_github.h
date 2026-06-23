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
} github_release_info_t;

/**
 * Check GitHub for a newer firmware release.
 * @param include_prereleases If true, also consider pre-release versions
 * @param current_version Current firmware version string (e.g., "1.0.12")
 * @param out Filled with release info if update available
 * @return true if a newer release is available, false otherwise
 */
bool ota_github_check(bool include_prereleases, const char *current_version, github_release_info_t *out);

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
