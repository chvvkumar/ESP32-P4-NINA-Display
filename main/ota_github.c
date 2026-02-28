#include "ota_github.h"
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
#include <stdlib.h>

static const char *TAG = "ota_github";

#define GITHUB_API_URL    "https://api.github.com/repos/chvvkumar/ESP32-P4-NINA-Display/releases?per_page=5"
#define OTA_BUF_SIZE      4096
#define MAX_RESPONSE_SIZE (128 * 1024)
#define OTA_ASSET_NAME    "nina-display-ota.bin"

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

    /* Both have suffix — if different, treat release (v1) as newer.
     * This handles git-describe versions (e.g., "-6-ga026865" from local
     * builds) being offered the official release (e.g., "-dev.1"). */
    if (suf1 && suf2) return strcmp(suf1, suf2) != 0 ? 1 : 0;

    return 0;  /* neither has suffix */
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
            return;
        }
    }

    /* No Summary section found — use first 500 chars of body */
    size_t len = strlen(body);
    if (len > 500) len = 500;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, body, len);
    out[len] = '\0';
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

/* ── Check GitHub for updates ─────────────────────────────────────── */

bool ota_github_check(bool include_prereleases, const char *current_version, github_release_info_t *out) {
    if (!current_version || !out) return false;

    ESP_LOGI(TAG, "Checking GitHub for updates (current: %s, channel: %s)",
             current_version, include_prereleases ? "pre-release" : "stable");

    http_response_t resp = {0};
    resp.buffer_size = MAX_RESPONSE_SIZE;
    resp.buffer = heap_caps_malloc(resp.buffer_size, MALLOC_CAP_SPIRAM);
    if (!resp.buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return false;
    }
    resp.buffer[0] = '\0';

    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(resp.buffer);
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-NINA-Display");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GitHub API request failed (err=%s, status=%d)", esp_err_to_name(err), status);
        free(resp.buffer);
        return false;
    }

    ESP_LOGI(TAG, "GitHub API response: %d bytes", resp.buffer_len);

    if (resp.overflow) {
        ESP_LOGE(TAG, "Response was truncated (buffer %d bytes too small)", resp.buffer_size);
        free(resp.buffer);
        return false;
    }

    /* Parse JSON array of releases */
    cJSON *releases = cJSON_Parse(resp.buffer);
    free(resp.buffer);
    if (!releases || !cJSON_IsArray(releases)) {
        ESP_LOGW(TAG, "Failed to parse GitHub releases JSON");
        if (releases) cJSON_Delete(releases);
        return false;
    }

    bool found = false;
    int count = cJSON_GetArraySize(releases);
    for (int i = 0; i < count && !found; i++) {
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

        /* Compare versions */
        if (compare_versions(tag->valuestring, current_version) <= 0) continue;

        /* Find OTA asset */
        cJSON *assets = cJSON_GetObjectItem(release, "assets");
        if (!cJSON_IsArray(assets)) continue;

        const char *ota_url = NULL;
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

        if (!ota_url) {
            ESP_LOGW(TAG, "Release %s has no %s asset, skipping", tag->valuestring, OTA_ASSET_NAME);
            continue;
        }

        /* Extract summary from release body */
        cJSON *body = cJSON_GetObjectItem(release, "body");
        const char *body_str = cJSON_IsString(body) ? body->valuestring : "";

        /* Fill output */
        memset(out, 0, sizeof(*out));
        strncpy(out->tag, tag->valuestring, sizeof(out->tag) - 1);
        extract_summary(body_str, out->summary, sizeof(out->summary));
        strncpy(out->ota_url, ota_url, sizeof(out->ota_url) - 1);
        out->is_prerelease = is_pre;

        ESP_LOGI(TAG, "Update available: %s (pre-release: %s)", out->tag, is_pre ? "yes" : "no");
        found = true;
    }

    cJSON_Delete(releases);

    if (!found) {
        ESP_LOGI(TAG, "No newer release found");
        return false;
    }

    /*
     * Resolve redirect chain: browser_download_url redirects via 302 from
     * github.com to objects.githubusercontent.com (S3).  We resolve here
     * because the OTA download task uses open/read which doesn't follow
     * redirects.  With auto-redirect disabled, the client won't retain the
     * Location header internally, so we capture it via an event handler
     * during HTTP_EVENT_ON_HEADER.  Using GET+perform() with auto-redirect
     * off — perform() completes at the 302 without chasing the redirect.
     */
    ESP_LOGI(TAG, "Resolving OTA download URL...");
    char resolved_url[2048] = {0};
    redirect_ctx_t redir_ctx = {
        .url_buf = resolved_url,
        .url_buf_size = sizeof(resolved_url),
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

    return true;
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
            int pct = (received * 100) / total_size;
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

#define OTA_TASK_STACK_SIZE 16384

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

#define OTA_NVS_NAMESPACE "ota_ver"
#define OTA_NVS_KEY       "tag"

void ota_github_save_installed_version(const char *tag) {
    if (!tag || !tag[0]) return;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, OTA_NVS_KEY, tag);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Saved OTA installed version: %s", tag);
    }
}

const char *ota_github_get_current_version(void) {
    static char stored[64];
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(stored);
        if (nvs_get_str(h, OTA_NVS_KEY, stored, &len) == ESP_OK && stored[0]) {
            nvs_close(h);
            ESP_LOGI(TAG, "Using OTA-installed version for comparison: %s", stored);
            return stored;
        }
        nvs_close(h);
    }
    return BUILD_GIT_TAG;
}
