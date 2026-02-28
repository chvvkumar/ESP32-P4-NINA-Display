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

#ifdef __cplusplus
}
#endif
