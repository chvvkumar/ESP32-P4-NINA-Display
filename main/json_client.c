/**
 * @file json_client.c
 * @brief Generic JSON HTTP client for the JSON Display page.
 *
 * Uses http_fetch's shared fetcher with a module-static keep-alive slot
 * (s_conn). JSON polling is single-owner (only json_poll_task ever calls
 * json_client_poll), so the connection slot needs no locking.
 *
 * Resolver: C port of the JSON Display mockup tokenizePath()/evalPath().
 * Config cache: mirrors allsky_client.c get_field_config() (parse-once,
 * invalidate on change), extended to also cache the row-major flattened path
 * list so a poll never re-walks the rows[][] structure on a cache hit.
 */

#include "json_client.h"
#include "http_fetch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <stdlib.h>

static const char *TAG = "json_client";

/* Maximum response size (PSRAM), protective cap. Point the page at a CURATED
 * endpoint (a single HA entity `/api/states/<id>`, or a template sensor whose
 * attributes bundle the wanted values), NOT the full `/api/states` dump
 * (~1.5 MB / 3000+ entities on a real HA — impractical to poll + parse on an
 * embedded device). A response over this cap is truncated → parse fails → the
 * page shows "Cannot Reach Source" rather than OOM-crashing. */
#define JSON_RESPONSE_BUF_SIZE   262144

/* HTTP timeout for JSON source requests */
#define JSON_HTTP_TIMEOUT_MS     10000

/* Cached parsed tiles_config_json + its source string — avoids re-parsing and
 * re-walking rows[][] on every poll cycle. */
static cJSON *s_cached_tiles     = NULL;
static char  *s_cached_tiles_str = NULL;

/* Row-major flattened path list, rebuilt on cache miss. Each pointer aliases
 * a string inside s_cached_tiles (valid until the cache is invalidated). A
 * NULL entry means the tile had no usable "path" (renders unresolved). */
static const char *s_tile_paths[JSON_MAX_TILES];
static int         s_tile_path_count = 0;

/* Persistent keep-alive slot, owned exclusively by json_poll_task; lazily
 * created on first poll. */
static http_fetch_conn_t *s_conn = NULL;

/* Forward declarations (prototype-before-use under -Werror). */
static bool  cjson_scalar_str(const cJSON *node, char *buf, size_t len);
static cJSON *json_eval_path(cJSON *root, const char *path);
static void  invalidate_tiles_config(void);
static int   get_tiles_config(const char *tiles_config_json);

// =============================================================================
// Mutex Helpers (mirror allsky_data_init/lock/unlock)
// =============================================================================

void json_client_init(json_data_t *data) {
    if (!data) {
        return;
    }
    memset(data, 0, sizeof(json_data_t));
    data->connected = false;
    data->tile_count = 0;
    data->last_poll_ms = 0;
    data->mutex = xSemaphoreCreateMutex();
}

bool json_client_lock(json_data_t *data, int timeout_ms) {
    if (!data || !data->mutex) {
        return false;
    }
    return xSemaphoreTake(data->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void json_client_unlock(json_data_t *data) {
    if (data && data->mutex) {
        xSemaphoreGive(data->mutex);
    }
}

// =============================================================================
// Scalar extraction — string/number/bool -> string (mirrors allsky:98-116)
// =============================================================================

/**
 * Render a scalar cJSON node into @p buf as a string. Returns false (and sets
 * buf to "") for non-scalar nodes (object/array/null) or a NULL node.
 */
static bool cjson_scalar_str(const cJSON *node, char *buf, size_t len) {
    if (!buf || len == 0) {
        return false;
    }
    buf[0] = '\0';
    if (!node) {
        return false;
    }

    if (cJSON_IsString(node)) {
        snprintf(buf, len, "%s", node->valuestring ? node->valuestring : "");
        return true;
    }

    if (cJSON_IsNumber(node)) {
        double val = node->valuedouble;
        if (val == (double)(int)val) {
            snprintf(buf, len, "%d", (int)val);
        } else {
            snprintf(buf, len, "%.2f", val);
        }
        return true;
    }

    if (cJSON_IsBool(node)) {
        snprintf(buf, len, "%s", cJSON_IsTrue(node) ? "true" : "false");
        return true;
    }

    return false;
}

// =============================================================================
// Path resolver — C port of mockup tokenizePath()/evalPath()
// =============================================================================

/**
 * Walk @p path against @p root and return the terminal cJSON node, or NULL.
 *
 * Token grammar (matches mockup:592-638):
 *   .key            -> cJSON_GetObjectItem(cur, key)
 *   [N]             -> cJSON_GetArrayItem(cur, N)
 *   [field=value]   -> first array element el where scalar(el[field]) == value
 *                      (field = substring up to first '='; value = the rest,
 *                       which may contain dots)
 *
 * Operates on a mutable copy so segments can be NUL-terminated in place.
 */
static cJSON *json_eval_path(cJSON *root, const char *path) {
    if (!root || !path || path[0] == '\0') {
        return NULL;
    }

    char path_copy[256];
    size_t len = strlen(path);
    if (len >= sizeof(path_copy)) {
        return NULL;   /* reject overlong paths (mirrors allsky:79-81) */
    }
    memcpy(path_copy, path, len + 1);

    cJSON *cur = root;
    size_t i = 0;

    while (i < len) {
        if (cur == NULL) {
            return NULL;
        }

        char ch = path_copy[i];

        if (ch == '.') {
            i++;
            continue;
        }

        if (ch == '[') {
            char *close = strchr(&path_copy[i], ']');
            if (!close) {
                break;   /* malformed selector; stop (mockup breaks the loop) */
            }
            *close = '\0';                     /* terminate the inner slice */
            char *inner = &path_copy[i + 1];
            size_t next = (size_t)(close - path_copy) + 1;

            char *eq = strchr(inner, '=');
            if (eq) {
                /* [field=value] match selector */
                *eq = '\0';
                const char *field = inner;
                const char *value = eq + 1;

                if (!cJSON_IsArray(cur)) {
                    return NULL;
                }
                cJSON *found = NULL;
                cJSON *el = NULL;
                char scalar[JSON_TILE_VALUE_LEN];
                cJSON_ArrayForEach(el, cur) {
                    cJSON *fv = cJSON_GetObjectItem(el, field);
                    if (cjson_scalar_str(fv, scalar, sizeof(scalar)) &&
                        strcmp(scalar, value) == 0) {
                        found = el;
                        break;
                    }
                }
                cur = found;
            } else {
                /* [N] array index */
                int idx = (int)strtol(inner, NULL, 10);
                cur = cJSON_GetArrayItem(cur, idx);
            }
            i = next;
        } else {
            /* bare key: scan to next '.' or '[' */
            size_t j = i;
            while (j < len && path_copy[j] != '.' && path_copy[j] != '[') {
                j++;
            }
            char saved = path_copy[j];
            path_copy[j] = '\0';
            cur = cJSON_GetObjectItem(cur, &path_copy[i]);
            path_copy[j] = saved;
            i = j;
        }
    }

    return cur;
}

bool json_client_resolve_path(void *root, const char *path, char *out, size_t out_len) {
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    cJSON *node = json_eval_path((cJSON *)root, path);
    return cjson_scalar_str(node, out, out_len);
}

// =============================================================================
// Tiles config cache — parse-once + row-major flattened path list
// =============================================================================

void json_client_invalidate_config_cache(void) {
    invalidate_tiles_config();
}

static void invalidate_tiles_config(void) {
    if (s_cached_tiles) {
        cJSON_Delete(s_cached_tiles);
        s_cached_tiles = NULL;
    }
    if (s_cached_tiles_str) {
        heap_caps_free(s_cached_tiles_str);
        s_cached_tiles_str = NULL;
    }
    for (int i = 0; i < JSON_MAX_TILES; i++) {
        s_tile_paths[i] = NULL;
    }
    s_tile_path_count = 0;
}

/**
 * Ensure the tiles config is parsed and the row-major path list is built.
 * Returns the number of flattened tiles (0 on empty/parse failure). On a cache
 * hit (config string unchanged) the cached list is reused unchanged.
 */
static int get_tiles_config(const char *tiles_config_json) {
    if (!tiles_config_json || tiles_config_json[0] == '\0') {
        invalidate_tiles_config();
        return 0;
    }

    /* Cache hit — config string unchanged. */
    if (s_cached_tiles && s_cached_tiles_str &&
        strcmp(s_cached_tiles_str, tiles_config_json) == 0) {
        return s_tile_path_count;
    }

    /* Cache miss — invalidate and re-parse. */
    invalidate_tiles_config();

    s_cached_tiles = cJSON_Parse(tiles_config_json);
    if (!s_cached_tiles) {
        ESP_LOGW(TAG, "Failed to parse json_tiles_config");
        return 0;
    }

    size_t len = strlen(tiles_config_json) + 1;
    s_cached_tiles_str = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!s_cached_tiles_str) {
        cJSON_Delete(s_cached_tiles);
        s_cached_tiles = NULL;
        return 0;
    }
    memcpy(s_cached_tiles_str, tiles_config_json, len);

    /* Flatten rows[][] into a row-major path list (row 0 L->R, then row 1...). */
    cJSON *rows = cJSON_GetObjectItem(s_cached_tiles, "rows");
    if (cJSON_IsArray(rows)) {
        cJSON *row = NULL;
        cJSON_ArrayForEach(row, rows) {
            if (s_tile_path_count >= JSON_MAX_TILES) {
                break;
            }
            if (!cJSON_IsArray(row)) {
                continue;
            }
            cJSON *tile = NULL;
            cJSON_ArrayForEach(tile, row) {
                if (s_tile_path_count >= JSON_MAX_TILES) {
                    break;
                }
                cJSON *path_item = cJSON_GetObjectItem(tile, "path");
                const char *p = NULL;
                if (cJSON_IsString(path_item) && path_item->valuestring[0] != '\0') {
                    p = path_item->valuestring;
                }
                s_tile_paths[s_tile_path_count] = p;   /* NULL => unresolved slot */
                s_tile_path_count++;
            }
        }
    }

    ESP_LOGD(TAG, "Tiles config cache updated: %d tiles", s_tile_path_count);
    return s_tile_path_count;
}

// =============================================================================
// Public API — Poll a JSON endpoint
// =============================================================================

void json_client_poll(const char *url, const char *auth_header,
                      const char *tiles_config_json, json_data_t *data) {
    if (!url || url[0] == '\0' || !data) {
        return;
    }

    ESP_LOGD(TAG, "Polling JSON source: %s", url);

    /* Lazily create the keep-alive slot on first poll. If allocation fails,
     * fall through with conn == NULL -- http_fetch falls back to a one-shot
     * client for this attempt. */
    if (!s_conn) {
        s_conn = http_fetch_conn_create();
        if (!s_conn) {
            ESP_LOGW(TAG, "Failed to allocate JSON keep-alive connection slot");
        }
    }

    bool is_https = (strncasecmp(url, "https", 5) == 0);

    /* Auth header: forwarded verbatim as a raw "Name: value" request header via
     * opts.extra_header. This covers any scheme -- "Authorization: Bearer <tok>",
     * "X-API-Key: <key>", etc. -- since http_fetch splits it at the first ':'. */
    http_fetch_opts_t opts = {
        .timeout_ms = JSON_HTTP_TIMEOUT_MS,
        .use_tls_bundle = is_https,
        .max_redirects = 3,
        .max_attempts = 1,
        .max_response_bytes = JSON_RESPONSE_BUF_SIZE,
        .extra_header = (auth_header && auth_header[0] != '\0') ? auth_header : NULL,
        .conn = s_conn,
    };

    char *buffer = NULL;
    size_t total_read = 0;
    esp_err_t err = http_fetch_text(url, &opts, &buffer, &total_read);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JSON HTTP fetch failed for %s: %s", url, esp_err_to_name(err));
        if (json_client_lock(data, 100)) {
            data->connected = false;
            json_client_unlock(data);
        }
        return;
    }

    if (total_read == 0) {
        ESP_LOGW(TAG, "JSON: empty response body");
        heap_caps_free(buffer);
        if (json_client_lock(data, 100)) {
            data->connected = false;
            json_client_unlock(data);
        }
        return;
    }

    cJSON *json = cJSON_Parse(buffer);
    heap_caps_free(buffer);

    if (!json) {
        ESP_LOGW(TAG, "JSON: failed to parse response");
        if (json_client_lock(data, 100)) {
            data->connected = false;
            json_client_unlock(data);
        }
        return;
    }

    /* Resolve every tile path into a local struct, then copy under mutex. */
    json_data_t local;
    memset(&local, 0, sizeof(local));
    local.mutex = NULL;   /* local scratch — never locked */

    int count = get_tiles_config(tiles_config_json);
    if (count > JSON_MAX_TILES) {
        count = JSON_MAX_TILES;
    }
    for (int i = 0; i < count; i++) {
        const char *path = s_tile_paths[i];
        if (!path) {
            local.values[i][0] = '\0';
            local.resolved[i] = false;
            continue;
        }
        bool ok = json_client_resolve_path(json, path, local.values[i], JSON_TILE_VALUE_LEN);
        local.resolved[i] = ok;
        if (!ok) {
            local.values[i][0] = '\0';
            ESP_LOGD(TAG, "Tile %d path '%s' unresolved", i, path);
        }
    }
    local.tile_count = count;

    cJSON_Delete(json);

    /* Publish under the mutex. */
    if (json_client_lock(data, 200)) {
        data->connected = true;
        data->last_poll_ms = esp_timer_get_time() / 1000;
        data->tile_count = local.tile_count;
        memcpy(data->values, local.values, sizeof(data->values));
        memcpy(data->resolved, local.resolved, sizeof(data->resolved));
        json_client_unlock(data);
        ESP_LOGD(TAG, "JSON poll OK — %u bytes, %d tiles", (unsigned)total_read, count);
    } else {
        ESP_LOGW(TAG, "JSON: failed to acquire mutex for data update");
    }
}
