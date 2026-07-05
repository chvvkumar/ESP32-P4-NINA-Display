#include "ota_github.h"
#include "build_version.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *TAG = "ota_github";

/* Releases endpoint, paginated. per_page=10 keeps each page's JSON under the
 * 128 KB MAX_RESPONSE_SIZE buffer (measured worst case ~93 KB). The full per-page
 * URL is composed by appending "&page=N" into a stack buffer (see fetch_releases_page). */
#define GITHUB_API_URL    "https://api.github.com/repos/chvvkumar/ESP32-P4-NINA-Display/releases?per_page=10"
#define MAX_RELEASE_PAGES 10       /* safety cap: 10 pages x per_page=10 = 100 releases (>2x current 43) */
#define RELEASE_URL_BUF   256      /* generous bound for GITHUB_API_URL + "&page=NN" */
#define OTA_BUF_SIZE      4096
#define MAX_RESPONSE_SIZE (128 * 1024)
#define OTA_ASSET_NAME    "nina-display-ota.bin"
#define SND_ALPHA_TAG     "snd-alpha"   /* fixed tag of the rolling Alpha (snd) pre-release */

/* ── Semver comparison ──────────────────────────────────────────────── */

/**
 * Compare two version strings (with optional 'v' prefix).
 * Returns >0 if v1 is newer, <0 if v2 is newer, 0 if equal.
 * Pre-release suffixes (e.g., "-dev.1") are treated as newer than
 * the same base version since this project's pre-release tags
 * use the latest main release as their base.
 */
static int compare_versions(const char *v1, const char *v2) {
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;

    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;
    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);

    if (maj1 != maj2) return maj1 - maj2;
    if (min1 != min2) return min1 - min2;
    if (pat1 != pat2) return pat1 - pat2;

    /* Same base version — check for pre-release suffix */
    const char *suf1 = strchr(v1, '-');
    const char *suf2 = strchr(v2, '-');

    if (suf1 && !suf2) return 1;   /* v1 is pre-release of same base → newer */
    if (!suf1 && suf2) return -1;  /* v2 is pre-release of same base → v2 newer */

    /* Both have suffix — compare numerically if same type (e.g. -dev.N) */
    if (suf1 && suf2) {
        const char *dot1 = strrchr(suf1, '.');
        const char *dot2 = strrchr(suf2, '.');
        if (dot1 && dot2) {
            /* Same prefix (e.g. both "-dev") → compare trailing number */
            size_t plen1 = (size_t)(dot1 - suf1);
            size_t plen2 = (size_t)(dot2 - suf2);
            if (plen1 == plen2 && strncmp(suf1, suf2, plen1) == 0) {
                return atoi(dot1 + 1) - atoi(dot2 + 1);
            }
        }
        /* Different suffix types (e.g. "-dev.2" vs "-6-ga026865") — no update */
        return 0;
    }

    return 0;  /* neither has suffix */
}

/* ── Alpha (snd) freshness via commit-sha marker ──────────────────── */

/*
 * The Alpha (snd) release tag is constant ("snd-alpha"), so semver comparison
 * cannot tell a fresh build from the installed one. Instead the release body
 * carries a marker: "<!-- nina:sha=<commit sha> -->". This returns true when the
 * marker sha differs from the running firmware's BUILD_GIT_SHA (update available).
 *
 * Comparison is case-insensitive over min(strlen(marker), strlen(BUILD_GIT_SHA))
 * characters, requiring at least 7 hex chars compared (BUILD_GIT_SHA is the short
 * sha). A missing or too-short marker is treated as "no update" (fail safe).
 */
static bool alpha_marker_indicates_update(const char *body_str) {
    if (!body_str) return false;

    const char *m = strstr(body_str, "nina:sha=");
    if (!m) return false;
    m += strlen("nina:sha=");

    /* Copy the hex sha up to the next whitespace or end-of-marker. */
    char marker_sha[64] = {0};
    size_t n = 0;
    while (m[n] && n < sizeof(marker_sha) - 1) {
        char c = m[n];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-' || c == '>') break;
        marker_sha[n] = c;
        n++;
    }
    marker_sha[n] = '\0';

    size_t mlen = strlen(marker_sha);
    size_t blen = strlen(BUILD_GIT_SHA);
    size_t cmp = (mlen < blen) ? mlen : blen;
    if (cmp < 7) {
        ESP_LOGW(TAG, "Alpha sha marker too short (%u chars), not offering update", (unsigned)mlen);
        return false;   /* fail safe: do not offer */
    }

    if (strncasecmp(marker_sha, BUILD_GIT_SHA, cmp) == 0) {
        ESP_LOGI(TAG, "Alpha sha matches running build (%.*s), no update", (int)cmp, BUILD_GIT_SHA);
        return false;   /* same commit → no update */
    }

    ESP_LOGI(TAG, "Alpha sha differs (marker=%s build=%s), update available", marker_sha, BUILD_GIT_SHA);
    return true;
}

/* ── Full-erase floor marker (fast-path erase decision) ───────────── */

/*
 * Parse the "nina:full_erase_floor=<tag>" hidden marker from a release body
 * (same HTML-comment convention as the alpha "nina:sha" marker above). The tag
 * is copied up to the next whitespace or the "-->" comment terminator, so
 * pre-release tags containing '-' (e.g. "v2.0.0-dev.3") survive intact.
 * Returns true when the marker is present with a non-empty value; no version
 * validation is done here — the caller decides whether the tag is usable.
 */
static bool parse_full_erase_floor(const char *body_str, char *tag_out, size_t tag_out_size) {
    if (!body_str || !tag_out || tag_out_size == 0) return false;
    tag_out[0] = '\0';

    const char *m = strstr(body_str, "nina:full_erase_floor=");
    if (!m) return false;
    m += strlen("nina:full_erase_floor=");

    size_t n = 0;
    while (m[n] && n < tag_out_size - 1) {
        char c = m[n];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>') break;
        if (c == '-' && m[n + 1] == '-' && m[n + 2] == '>') break;
        tag_out[n] = c;
        n++;
    }
    tag_out[n] = '\0';

    return tag_out[0] != '\0';
}

/* A floor tag is usable only when it parses as a full major.minor.patch
 * version (optional 'v' prefix), mirroring compare_versions' parsing. */
static bool floor_tag_parses_as_version(const char *tag) {
    if (tag[0] == 'v' || tag[0] == 'V') tag++;
    int maj = 0, min = 0, pat = 0;
    return sscanf(tag, "%d.%d.%d", &maj, &min, &pat) == 3;
}

/* ── Strip markdown images ![alt](url) from a string in-place ─────── */

static void strip_markdown_images(char *text) {
    char *read = text, *write = text;
    while (*read) {
        if (read[0] == '!' && read[1] == '[') {
            /* Find closing ] */
            const char *alt_end = strchr(read + 2, ']');
            if (alt_end && alt_end[1] == '(') {
                /* Find closing ) */
                const char *url_end = strchr(alt_end + 2, ')');
                if (url_end) {
                    read = (char *)(url_end + 1);
                    /* Skip trailing whitespace/newlines left behind */
                    while (*read == ' ' || *read == '\n' || *read == '\r') read++;
                    continue;
                }
            }
        }
        *write++ = *read++;
    }
    *write = '\0';
}

/* ── Extract summary from release body ────────────────────────────── */

static void extract_summary(const char *body, char *out, size_t out_size) {
    if (!body || !out || out_size == 0) return;
    out[0] = '\0';

    /* Look for "### Summary" section */
    const char *start = strstr(body, "### Summary");
    if (!start) start = strstr(body, "### summary");
    if (start) {
        start = strchr(start, '\n');
        if (start) {
            start++; /* skip newline */
            /* Find end: next ### heading or end of string */
            const char *end = strstr(start, "\n### ");
            if (!end) end = strstr(start, "\n## ");
            size_t len;
            if (end) {
                len = (size_t)(end - start);
            } else {
                len = strlen(start);
            }
            if (len >= out_size) len = out_size - 1;
            memcpy(out, start, len);
            out[len] = '\0';
            /* Trim leading/trailing whitespace */
            while (out[0] == '\n' || out[0] == '\r' || out[0] == ' ') {
                memmove(out, out + 1, strlen(out));
            }
            size_t slen = strlen(out);
            while (slen > 0 && (out[slen - 1] == '\n' || out[slen - 1] == '\r' || out[slen - 1] == ' ')) {
                out[--slen] = '\0';
            }
            strip_markdown_images(out);
            return;
        }
    }

    /* No Summary section found — use first 500 chars of body */
    size_t len = strlen(body);
    if (len > 500) len = 500;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, body, len);
    out[len] = '\0';
    strip_markdown_images(out);
}

/* ── HTTP event handler for reading response into buffer ──────────── */

typedef struct {
    char *buffer;
    int   buffer_len;
    int   buffer_size;
    bool  overflow;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    http_response_t *resp = (http_response_t *)evt->user_data;
    if (!resp) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp->buffer_len + evt->data_len < resp->buffer_size) {
            memcpy(resp->buffer + resp->buffer_len, evt->data, evt->data_len);
            resp->buffer_len += evt->data_len;
            resp->buffer[resp->buffer_len] = '\0';
        } else if (!resp->overflow) {
            resp->overflow = true;
            ESP_LOGW(TAG, "Response buffer overflow: %d + %d >= %d",
                     resp->buffer_len, evt->data_len, resp->buffer_size);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ── Redirect resolver — captures Location header from 3xx ────────── */

typedef struct {
    char *url_buf;
    size_t url_buf_size;
} redirect_ctx_t;

static esp_err_t redirect_event_handler(esp_http_client_event_t *evt) {
    redirect_ctx_t *ctx = (redirect_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "Location") == 0 && evt->header_value) {
            strncpy(ctx->url_buf, evt->header_value, ctx->url_buf_size - 1);
            ctx->url_buf[ctx->url_buf_size - 1] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Fetch one page of the releases list ──────────────────────────── */

/*
 * Fetch a single page (?per_page=10&page=N) of the GitHub releases list and
 * parse it into a cJSON array. Returns the parsed array (caller owns it and must
 * cJSON_Delete it), or NULL on ANY failure: HTTP error, non-200 status, response
 * buffer overflow, or JSON parse failure. *overflow_out is set true only when the
 * 128 KB response buffer overflowed (so the caller can apply the history fail-safe);
 * it is left untouched on other failures. The helper frees its own response buffer.
 */
static cJSON *fetch_releases_page(int page, bool *overflow_out) {
    http_response_t resp = {0};
    resp.buffer_size = MAX_RESPONSE_SIZE;
    resp.buffer = heap_caps_malloc(resp.buffer_size, MALLOC_CAP_SPIRAM);
    if (!resp.buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer (page %d)", page);
        return NULL;
    }
    resp.buffer[0] = '\0';

    char url[RELEASE_URL_BUF];
    snprintf(url, sizeof(url), "%s&page=%d", GITHUB_API_URL, page);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client (page %d)", page);
        free(resp.buffer);
        return NULL;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-NINA-Display");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GitHub API request failed (page %d, err=%s, status=%d)",
                 page, esp_err_to_name(err), status);
        free(resp.buffer);
        return NULL;
    }

    ESP_LOGI(TAG, "GitHub API response (page %d): %d bytes", page, resp.buffer_len);

    if (resp.overflow) {
        ESP_LOGE(TAG, "Response was truncated (page %d, buffer %d bytes too small)",
                 page, resp.buffer_size);
        if (overflow_out) {
            *overflow_out = true;
        }
        free(resp.buffer);
        return NULL;
    }

    cJSON *releases = cJSON_Parse(resp.buffer);
    free(resp.buffer);
    if (!releases || !cJSON_IsArray(releases)) {
        ESP_LOGW(TAG, "Failed to parse GitHub releases JSON (page %d)", page);
        if (releases) {
            cJSON_Delete(releases);
        }
        return NULL;
    }

    return releases;
}

/* ── Check GitHub for updates ─────────────────────────────────────── */

/* Resolve the OTA download URL in-place (follows the GitHub 302 redirect to the
 * S3 asset). Used after a release target has been captured into *out. */
static void resolve_ota_download_url(github_release_info_t *out);

ota_check_result_t ota_github_check(int channel, const char *current_version, github_release_info_t *out) {
    if (!current_version || !out) return OTA_CHECK_ERROR;

    /* channel: 0 = Stable, 1 = Pre-releases / Beta, 2 = Alpha (snd). */
    const char *channel_name = (channel == 2) ? "alpha (snd)" :
                               (channel == 1) ? "pre-release" : "stable";
    ESP_LOGI(TAG, "Checking GitHub for updates (current: %s, channel: %s)",
             current_version, channel_name);
    bool include_prereleases = (channel == 1);

    /* ── Alpha (snd) channel ──────────────────────────────────────────────
     * The Alpha release is a single rolling pre-release with the constant tag
     * SND_ALPHA_TAG. Freshness is determined by the commit-sha marker in the
     * release body (not semver), and requires_full_erase is always false (the
     * body marker is nina:full_erase=0). Scan pages for the snd-alpha release,
     * verify its sha marker against the running build, and offer it if newer. */
    if (channel == 2) {
        bool alpha_fetch_error = false;
        for (int page = 1; page <= MAX_RELEASE_PAGES; page++) {
            bool overflow = false;
            cJSON *releases = fetch_releases_page(page, &overflow);
            if (!releases) {
                ESP_LOGW(TAG, "Alpha (snd) fetch %s on page %d",
                         overflow ? "overflowed" : "failed", page);
                alpha_fetch_error = true;
                break;
            }
            int count = cJSON_GetArraySize(releases);
            for (int i = 0; i < count; i++) {
                cJSON *release = cJSON_GetArrayItem(releases, i);
                if (!release) continue;
                cJSON *draft = cJSON_GetObjectItem(release, "draft");
                if (cJSON_IsTrue(draft)) continue;
                cJSON *tag = cJSON_GetObjectItem(release, "tag_name");
                if (!cJSON_IsString(tag) || !tag->valuestring) continue;
                if (strcmp(tag->valuestring, SND_ALPHA_TAG) != 0) continue;  /* only snd-alpha */

                cJSON *body = cJSON_GetObjectItem(release, "body");
                const char *body_str = cJSON_IsString(body) ? body->valuestring : "";

                if (!alpha_marker_indicates_update(body_str)) {
                    cJSON_Delete(releases);
                    ESP_LOGI(TAG, "Alpha (snd) up to date");
                    return OTA_CHECK_UP_TO_DATE;
                }

                /* Locate the OTA asset on the snd-alpha release. */
                const char *ota_url = NULL;
                cJSON *assets = cJSON_GetObjectItem(release, "assets");
                if (cJSON_IsArray(assets)) {
                    int asset_count = cJSON_GetArraySize(assets);
                    for (int j = 0; j < asset_count; j++) {
                        cJSON *asset = cJSON_GetArrayItem(assets, j);
                        cJSON *name = cJSON_GetObjectItem(asset, "name");
                        if (cJSON_IsString(name) && strcmp(name->valuestring, OTA_ASSET_NAME) == 0) {
                            cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
                            if (cJSON_IsString(url)) ota_url = url->valuestring;
                            break;
                        }
                    }
                }
                if (!ota_url) {
                    ESP_LOGW(TAG, "Alpha (snd) release has no %s asset", OTA_ASSET_NAME);
                    cJSON_Delete(releases);
                    return OTA_CHECK_ERROR;
                }

                memset(out, 0, sizeof(*out));
                strncpy(out->tag, tag->valuestring, sizeof(out->tag) - 1);
                out->tag[sizeof(out->tag) - 1] = '\0';
                extract_summary(body_str, out->summary, sizeof(out->summary));
                strncpy(out->ota_url, ota_url, sizeof(out->ota_url) - 1);
                out->ota_url[sizeof(out->ota_url) - 1] = '\0';
                out->is_prerelease = true;
                out->requires_full_erase = false;   /* alpha body marker is nina:full_erase=0 */
                out->full_erase_tag[0] = '\0';
                cJSON_Delete(releases);
                ESP_LOGI(TAG, "Alpha (snd) update target: %s", out->tag);
                resolve_ota_download_url(out);
                return OTA_CHECK_UPDATE_AVAILABLE;
            }
            cJSON_Delete(releases);
        }
        ESP_LOGI(TAG, "No Alpha (snd) release found");
        return alpha_fetch_error ? OTA_CHECK_ERROR : OTA_CHECK_UP_TO_DATE;
    }

    /* Detect channel switch: current version type doesn't match target channel.
     * When switching channels (e.g. pre-release → stable), the normal version
     * comparison may reject the latest target release as "older".  In that case
     * we skip the version check and offer the first (latest) matching release. */
    const char *cv = current_version;
    if (cv[0] == 'v' || cv[0] == 'V') cv++;
    bool current_is_prerelease = (strchr(cv, '-') != NULL);
    bool channel_switch = (current_is_prerelease && !include_prereleases) ||
                          (!current_is_prerelease && include_prereleases);
    if (channel_switch) {
        ESP_LOGI(TAG, "Channel switch detected (%s → %s), will install latest from target channel",
                 current_is_prerelease ? "pre-release" : "stable",
                 include_prereleases ? "pre-release" : "stable");
    }

    /* ── Paginated path scan (newest-first) ──────────────────────────────
     * Walk pages 1..MAX_RELEASE_PAGES. The TARGET is the newest in-channel
     * release newer than installed (first qualifying release, newest-first).
     * The full-erase marker is OR'd across EVERY in-channel release on the
     * path (installed < v <= target), so a marker on a skipped intermediate
     * release suppresses the OTA even when the target itself is unmarked.
     * full_erase_tag records the FIRST marked release encountered = newest
     * marked. Scanning stops once a release at-or-below installed is seen
     * (path fully covered). Fail-safe: if the page cap is hit before reaching
     * installed, or a fetch errors/overflows mid-path, force manual-flash. */
    bool found_target = false;          /* an OTA target release has been captured */
    bool any_marker = false;            /* OR of nina:full_erase=1 across the path */
    bool reached_installed = false;     /* a fetched in-channel release compared <= installed */
    bool fail_safe = false;             /* history could not be fully verified */
    bool verify_error = false;          /* transient mid-path fetch failure (retry, not manual-flash) */
    bool fetch_failed_no_target = false;/* page-1 fetch failed before any target captured */
    char full_erase_tag_local[32] = {0};/* newest marked tag on the path (written once) */
    bool floor_decided = false;         /* erase decision made via the target's floor marker */
    bool floor_requires_erase = false;  /* fast-path result: installed < floor */
    char floor_tag_local[32] = {0};     /* floor tag parsed from the target release body */

    for (int page = 1; page <= MAX_RELEASE_PAGES && !reached_installed; page++) {
        bool overflow = false;
        cJSON *releases = fetch_releases_page(page, &overflow);
        if (!releases) {
            /* Page-1 failure with nothing found yet → behave as the old
             * "request failed → return false" path. A MID-PATH failure (after a
             * target was captured, before reaching installed) means the history
             * is unverifiable → fail-safe to manual-flash. */
            if (found_target) {
                ESP_LOGW(TAG, "Couldn't verify full update history: page %d fetch %s",
                         page, overflow ? "overflowed" : "failed");
                verify_error = true;
            } else {
                ESP_LOGW(TAG, "GitHub release fetch failed on page %d with no target found", page);
                fetch_failed_no_target = true;
            }
            break;
        }

        int count = cJSON_GetArraySize(releases);
        if (count == 0) {
            /* Empty page: natural end of the releases list, history fully covered. */
            cJSON_Delete(releases);
            reached_installed = true;
            break;
        }
        for (int i = 0; i < count && !reached_installed; i++) {
            cJSON *release = cJSON_GetArrayItem(releases, i);
            if (!release) continue;

            /* Skip drafts */
            cJSON *draft = cJSON_GetObjectItem(release, "draft");
            if (cJSON_IsTrue(draft)) continue;

            /* Check pre-release flag — each channel only sees its own releases */
            cJSON *prerelease = cJSON_GetObjectItem(release, "prerelease");
            bool is_pre = cJSON_IsTrue(prerelease);
            if (is_pre && !include_prereleases) continue;   /* stable channel: skip pre-releases */
            if (!is_pre && include_prereleases) continue;    /* pre-release channel: skip stable */

            /* Get tag name */
            cJSON *tag = cJSON_GetObjectItem(release, "tag_name");
            if (!cJSON_IsString(tag) || !tag->valuestring) continue;

            /* The Alpha (snd) rolling release belongs only to channel 2 (handled
             * above); never offer it on the Stable or Pre-release/Beta channels. */
            if (strcmp(tag->valuestring, SND_ALPHA_TAG) == 0) continue;

            /* Classify by version. When switching channels the version check is
             * skipped so the latest in-channel release is always the target. */
            if (!channel_switch && compare_versions(tag->valuestring, current_version) <= 0) {
                /* At-or-below installed: the path is fully covered. This release is
                 * NOT on the installed→target path, so its marker is irrelevant. */
                reached_installed = true;
                break;
            }

            /* This is a path release (installed < v, in channel). */
            cJSON *body = cJSON_GetObjectItem(release, "body");
            const char *body_str = cJSON_IsString(body) ? body->valuestring : "";

            /* OR the full-erase marker across the path; record the newest marked tag. */
            if (strstr(body_str, "nina:full_erase=1") != NULL) {
                any_marker = true;
                if (full_erase_tag_local[0] == '\0') {
                    strncpy(full_erase_tag_local, tag->valuestring, sizeof(full_erase_tag_local) - 1);
                    full_erase_tag_local[sizeof(full_erase_tag_local) - 1] = '\0';
                }
            }

            /* Capture the FIRST qualifying release (newest-first) as the OTA target. */
            if (!found_target) {
                cJSON *assets = cJSON_GetObjectItem(release, "assets");
                const char *ota_url = NULL;
                if (cJSON_IsArray(assets)) {
                    int asset_count = cJSON_GetArraySize(assets);
                    for (int j = 0; j < asset_count; j++) {
                        cJSON *asset = cJSON_GetArrayItem(assets, j);
                        cJSON *name = cJSON_GetObjectItem(asset, "name");
                        if (cJSON_IsString(name) && strcmp(name->valuestring, OTA_ASSET_NAME) == 0) {
                            cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
                            if (cJSON_IsString(url)) {
                                ota_url = url->valuestring;
                            }
                            break;
                        }
                    }
                }

                if (!ota_url) {
                    /* No OTA asset on the newest release: it cannot be the OTA target,
                     * but its marker still counts toward the path determination above.
                     * Keep scanning for an older release that does carry the asset. */
                    ESP_LOGW(TAG, "Release %s has no %s asset, skipping as target",
                             tag->valuestring, OTA_ASSET_NAME);
                } else {
                    memset(out, 0, sizeof(*out));
                    strncpy(out->tag, tag->valuestring, sizeof(out->tag) - 1);
                    out->tag[sizeof(out->tag) - 1] = '\0';
                    extract_summary(body_str, out->summary, sizeof(out->summary));
                    strncpy(out->ota_url, ota_url, sizeof(out->ota_url) - 1);
                    out->ota_url[sizeof(out->ota_url) - 1] = '\0';
                    out->is_prerelease = is_pre;
                    found_target = true;
                    ESP_LOGI(TAG, "Update target: %s (pre-release: %s)", out->tag, is_pre ? "yes" : "no");

                    /* Fast path: the target release body may carry a precomputed
                     * full-erase floor ("nina:full_erase_floor=<tag>"). When present
                     * and parseable, the erase decision is installed < floor
                     * (strictly less-than: installed == floor means the device
                     * already went through that erase) and the history walk is
                     * skipped entirely. Missing or malformed markers fall back to
                     * the existing walk unchanged. The floor is also skipped during
                     * a channel switch because cross-channel version tags are not
                     * comparable, so the legacy path below decides. */
                    if (!channel_switch &&
                        parse_full_erase_floor(body_str, floor_tag_local, sizeof(floor_tag_local)) &&
                        floor_tag_parses_as_version(floor_tag_local)) {
                        floor_decided = true;
                        floor_requires_erase =
                            (compare_versions(current_version, floor_tag_local) < 0);
                        ESP_LOGI(TAG, "Erase decision via floor marker: floor=%s installed=%s -> full_erase=%d",
                                 floor_tag_local, current_version, (int)floor_requires_erase);
                        reached_installed = true;   /* stop paging; walk not needed */
                        break;
                    }
                    if (!channel_switch) {
                        ESP_LOGW(TAG, "No usable full-erase floor marker on target %s, using history walk",
                                 out->tag);
                    }

                    /* Under channel_switch there is no installed-version boundary to
                     * reach, so the latest in-channel release is the whole path. */
                    if (channel_switch) {
                        reached_installed = true;
                        break;
                    }
                }
            }
        }

        cJSON_Delete(releases);
    }

    /* Fail-safe (ERASE-05): a target was found but the scan ended without
     * reaching installed (page cap exhausted) — the history is not fully
     * verified, so never offer OTA. The mid-path fetch-error case above set
     * verify_error instead, which short-circuits to OTA_CHECK_ERROR below. */
    if (found_target && !reached_installed && !fail_safe && !verify_error) {
        ESP_LOGW(TAG, "Couldn't verify full update history: page cap (%d) reached before installed version",
                 MAX_RELEASE_PAGES);
        fail_safe = true;
    }

    /* Transient mid-path fetch failure: history unverifiable, but this is a retry
     * condition, not a manual-flash requirement. out is caller-owned; leave it. */
    if (verify_error) {
        return OTA_CHECK_ERROR;
    }

    if (!found_target) {
        ESP_LOGI(TAG, "No newer release found");
        return fetch_failed_no_target ? OTA_CHECK_ERROR : OTA_CHECK_UP_TO_DATE;
    }

    /* Populate the erase determination on the captured target. */
    if (floor_decided) {
        /* Fast path: the target's floor marker decided; the OR walk did not run.
         * The floor tag is only meaningful when an erase is actually required. */
        out->requires_full_erase = floor_requires_erase;
        if (floor_requires_erase) {
            strncpy(out->full_erase_tag, floor_tag_local, sizeof(out->full_erase_tag) - 1);
            out->full_erase_tag[sizeof(out->full_erase_tag) - 1] = '\0';
        } else {
            out->full_erase_tag[0] = '\0';
        }
    } else {
        out->requires_full_erase = any_marker || fail_safe;
        if (fail_safe) {
            /* History-incomplete variant: empty tag signals "couldn't verify". */
            out->full_erase_tag[0] = '\0';
        } else {
            strncpy(out->full_erase_tag, full_erase_tag_local, sizeof(out->full_erase_tag) - 1);
            out->full_erase_tag[sizeof(out->full_erase_tag) - 1] = '\0';
        }
    }

    resolve_ota_download_url(out);
    return OTA_CHECK_UPDATE_AVAILABLE;
}

/* ── Resolve the OTA download URL (follow the GitHub 302 redirect) ─── */

/*
 * Resolve redirect chain: browser_download_url redirects via 302 from
 * github.com to objects.githubusercontent.com (S3).  We resolve here
 * because the OTA download task uses open/read which doesn't follow
 * redirects.  With auto-redirect disabled, the client won't retain the
 * Location header internally, so we capture it via an event handler
 * during HTTP_EVENT_ON_HEADER.  Using GET+perform() with auto-redirect
 * off — perform() completes at the 302 without chasing the redirect.
 * On any failure out->ota_url is left at the original (still valid) URL.
 */
static void resolve_ota_download_url(github_release_info_t *out) {
    ESP_LOGI(TAG, "Resolving OTA download URL...");
    char *resolved_url = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM);
    if (!resolved_url) {
        ESP_LOGE(TAG, "Failed to allocate resolved_url");
        return;  /* URL still valid, just unresolved */
    }
    redirect_ctx_t redir_ctx = {
        .url_buf = resolved_url,
        .url_buf_size = 2048,
    };
    esp_http_client_config_t redir_cfg = {
        .url = out->ota_url,
        .event_handler = redirect_event_handler,
        .user_data = &redir_ctx,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
        .user_agent = "ESP32-NINA-Display",
    };
    esp_http_client_handle_t hc = esp_http_client_init(&redir_cfg);
    if (hc) {
        esp_err_t herr = esp_http_client_perform(hc);
        int status = esp_http_client_get_status_code(hc);
        if (herr == ESP_OK && (status == 301 || status == 302) && resolved_url[0]) {
            ESP_LOGI(TAG, "Resolved OTA URL via %d redirect", status);
            strncpy(out->ota_url, resolved_url, sizeof(out->ota_url) - 1);
            out->ota_url[sizeof(out->ota_url) - 1] = '\0';
        } else if (herr == ESP_OK && status == 200) {
            ESP_LOGI(TAG, "No redirect — URL is direct (status 200)");
        } else {
            ESP_LOGW(TAG, "Redirect resolve: err=%s status=%d (will use original URL)",
                     esp_err_to_name(herr), status);
        }
        esp_http_client_cleanup(hc);
    }

    free(resolved_url);
}

/* ── Download and flash OTA binary ────────────────────────────────── */

/*
 * OTA flash operations (esp_ota_begin/write/end) require the calling task's
 * stack to be in internal SRAM because SPI flash operations disable the cache,
 * making PSRAM inaccessible.  The data_update_task uses a PSRAM stack, so we
 * run the actual download+flash work in a dedicated short-lived task whose
 * stack is allocated from internal RAM.
 */

typedef struct {
    const char *url;
    void (*progress_cb)(int percent);
    esp_err_t   result;
    SemaphoreHandle_t done;
} ota_task_ctx_t;

static void ota_download_task(void *arg) {
    ota_task_ctx_t *ctx = (ota_task_ctx_t *)arg;

    ESP_LOGI(TAG, "OTA task started, URL: %.128s...", ctx->url);

    int total_size = 0;

    /* Prepare OTA partition */
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "No update partition found");
        ctx->result = ESP_ERR_NOT_FOUND;
        goto done;
    }
    ESP_LOGI(TAG, "Writing to partition '%s' at offset 0x%lx", part->label, part->address);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ctx->result = err;
        goto done;
    }

    /* GET from pre-resolved URL — open/read loop streams directly to OTA */
    {
        esp_http_client_config_t dl_cfg = {
            .url = ctx->url,
            .timeout_ms = 60000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = OTA_BUF_SIZE,
            .buffer_size_tx = 2048,     /* S3 pre-signed URLs can be ~1KB */
            .user_agent = "ESP32-NINA-Display",
        };
        esp_http_client_handle_t client = esp_http_client_init(&dl_cfg);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init download client");
            esp_ota_abort(ota_handle);
            ctx->result = ESP_FAIL;
            goto done;
        }

        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            esp_ota_abort(ota_handle);
            ctx->result = err;
            goto done;
        }

        int content_length = esp_http_client_fetch_headers(client);
        if (content_length > 0) {
            total_size = content_length;
        } else {
            total_size = 2 * 1024 * 1024;  /* estimate for progress bar */
            ESP_LOGW(TAG, "Content-Length unknown, estimating %d bytes", total_size);
        }

        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP status %d (expected 200)", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            esp_ota_abort(ota_handle);
            ctx->result = ESP_FAIL;
            goto done;
        }

        /* Read loop — use internal SRAM buffer for flash-safe writes */
        char *buf = heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to alloc download buffer");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            esp_ota_abort(ota_handle);
            ctx->result = ESP_ERR_NO_MEM;
            goto done;
        }

        int received = 0;
        int last_pct = -1;
        bool failed = false;

        while (1) {
            int len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
            if (len < 0) {
                ESP_LOGE(TAG, "Read error at %d/%d", received, total_size);
                failed = true;
                break;
            }
            if (len == 0) {
                if (esp_http_client_is_complete_data_received(client)) break;
                ESP_LOGE(TAG, "Connection dropped at %d/%d", received, total_size);
                failed = true;
                break;
            }

            err = esp_ota_write(ota_handle, buf, len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                failed = true;
                break;
            }

            received += len;
            /* 64-bit math: received*100 overflows int32 above ~21MB */
            int pct = (int)(((int64_t)received * 100) / total_size);
            if (pct > 100) pct = 100;
            if (pct != last_pct) {
                last_pct = pct;
                if (ctx->progress_cb) ctx->progress_cb(pct);
                if (pct % 10 == 0) {
                    ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d)", pct, received, total_size);
                }
            }
        }

        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (failed) {
            esp_ota_abort(ota_handle);
            ctx->result = ESP_FAIL;
            goto done;
        }

        ESP_LOGI(TAG, "Download complete: %d bytes", received);
    }

    /* Finalize OTA */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        ctx->result = err;
        goto done;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ctx->result = err;
        goto done;
    }

    ESP_LOGI(TAG, "OTA complete, boot partition updated");
    ctx->result = ESP_OK;

done:
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

#define OTA_TASK_STACK_SIZE 8192

esp_err_t ota_github_download(const char *url, void (*progress_cb)(int percent)) {
    if (!url) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Starting OTA download from: %s", url);

    ota_task_ctx_t ctx = {
        .url = url,
        .progress_cb = progress_cb,
        .result = ESP_FAIL,
        .done = xSemaphoreCreateBinary(),
    };
    if (!ctx.done) {
        ESP_LOGE(TAG, "Failed to create OTA semaphore");
        return ESP_ERR_NO_MEM;
    }

    /* Spawn OTA task with internal SRAM stack — required for SPI flash operations */
    BaseType_t ret = xTaskCreatePinnedToCore(
        ota_download_task, "ota_dl", OTA_TASK_STACK_SIZE,
        &ctx, 6, NULL, tskNO_AFFINITY);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA download task");
        vSemaphoreDelete(ctx.done);
        return ESP_ERR_NO_MEM;
    }

    /* Block until the OTA task completes */
    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);

    return ctx.result;
}

/* ── NVS-backed OTA version tracking ─────────────────────────────── */
/* Three keys in the "ota_ver" namespace:
 *   tag     — confirmed release tag of the image that is ACTUALLY running.
 *   build   — BUILD_GIT_TAG that the stored `tag` belongs to (identity guard).
 *   pending — release tag an in-flight OTA intends to install; promoted to
 *             `tag` only after that image boots and is confirmed valid.
 * The `tag` key is never written at OTA-apply time, so a downloaded-but-not-
 * booted image (rollback / slot mismatch) cannot poison the version banner. */
#define OTA_NVS_NAMESPACE   "ota_ver"
#define OTA_NVS_KEY         "tag"
#define OTA_NVS_KEY_BUILD   "build"
#define OTA_NVS_KEY_PENDING "pending"

void ota_github_save_pending_version(const char *tag) {
    if (!tag || !tag[0]) return;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, OTA_NVS_KEY_PENDING, tag);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "OTA pending version stamped: %s (confirmed on next boot)", tag);
    }
}

void ota_github_reconcile_version(bool first_boot_new_image) {
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    char pending[64] = {0};
    size_t len = sizeof(pending);
    bool have_pending = (nvs_get_str(h, OTA_NVS_KEY_PENDING, pending, &len) == ESP_OK && pending[0]);

    if (first_boot_new_image && have_pending) {
        /* The OTA image actually booted — promote intent to confirmed state and
         * bind it to the running build so the read path can validate it later. */
        nvs_set_str(h, OTA_NVS_KEY, pending);
        nvs_set_str(h, OTA_NVS_KEY_BUILD, BUILD_GIT_TAG);
        nvs_erase_key(h, OTA_NVS_KEY_PENDING);
        ESP_LOGI(TAG, "OTA confirmed booted: installed=%s build=%s", pending, BUILD_GIT_TAG);
    } else if (have_pending) {
        /* Normal boot or rollback to the old image — the pending stamp is stale. */
        nvs_erase_key(h, OTA_NVS_KEY_PENDING);
        ESP_LOGW(TAG, "Discarded stale pending OTA version %s (image did not take)", pending);
    }

    nvs_commit(h);
    nvs_close(h);
}

const char *ota_github_get_current_version(void) {
    static char tag[64];
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        char build[64];
        size_t tl = sizeof(tag);
        size_t bl = sizeof(build);
        bool ok_tag   = (nvs_get_str(h, OTA_NVS_KEY,       tag,   &tl) == ESP_OK && tag[0]);
        bool ok_build = (nvs_get_str(h, OTA_NVS_KEY_BUILD, build, &bl) == ESP_OK && build[0]);
        nvs_close(h);
        /* Only trust the stored release tag if it belongs to the build that is
         * actually running. A mismatch means the stored tag is stale (e.g. an OTA
         * that never booted, or a manual flash) — fall back to the truth. */
        if (ok_tag && ok_build && strcmp(build, BUILD_GIT_TAG) == 0) {
            ESP_LOGI(TAG, "Using OTA-installed version for comparison: %s", tag);
            return tag;
        }
    }
    return BUILD_GIT_TAG;
}
