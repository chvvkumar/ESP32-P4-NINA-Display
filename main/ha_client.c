/**
 * @file ha_client.c
 * @brief Home Assistant client for the HA page.
 *
 * Uses http_fetch's shared fetcher. Within ha_client_poll a module-static
 * keep-alive slot (s_conn) is reused across the sequential per-entity fetches;
 * polling is single-owner (only ha_poll_task ever calls ha_client_poll), so the
 * slot needs no locking. The PUBLIC ha_client_fetch_entity() is a one-shot fetch
 * (conn=NULL) so the /api/ha-probe handler can call it safely from the httpd
 * worker task without touching the poll task's keep-alive slot.
 *
 * Per tile: (entity_id, attr). Fetch GET {base}/api/states/{entity_id} per
 * UNIQUE entity, sequentially, de-duped; resolve every tile's value from the
 * parsed entity (top-level "state" when attr=="state", else attributes.<attr>).
 * Config cache mirrors json_client.c get_tiles_config() (parse-once, invalidate
 * on change), storing a row-major (entity_id, attr) list.
 */

#include "ha_client.h"
#include "http_fetch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>   /* strncasecmp / strcasecmp */
#include <stdlib.h>

static const char *TAG = "ha_client";

/* Maximum response size (PSRAM), protective cap. A single HA entity is tiny
 * (~0.5-2.6 KB), but keep JSON's protective cap: a response over this is
 * truncated -> parse fails -> the tile shows unresolved rather than OOM. */
#define HA_RESPONSE_BUF_SIZE   262144

/* HTTP timeout for HA state requests */
#define HA_HTTP_TIMEOUT_MS     10000

/* Longest entity_id we store per de-dupe slot. HA entity ids are domain.object
 * and comfortably fit; overlong ids are rejected. */
#define HA_ENTITY_ID_LEN       128

/* Cached parsed tiles_config_json + its source string -- avoids re-parsing and
 * re-walking rows[][] on every poll cycle. */
static cJSON *s_cached_tiles     = NULL;
static char  *s_cached_tiles_str = NULL;

/* Row-major flattened (entity_id, attr) list, rebuilt on cache miss. Each
 * pointer aliases a string inside s_cached_tiles (valid until invalidation). A
 * NULL entity means the tile had no usable entity_id (renders unresolved). A
 * NULL attr defaults to "state" at resolve time. */
static const char *s_tile_entities[JSON_MAX_TILES];
static const char *s_tile_attrs[JSON_MAX_TILES];
static int         s_tile_count = 0;

/* Persistent keep-alive slot, owned exclusively by ha_poll_task; lazily created
 * on first poll. */
static http_fetch_conn_t *s_conn = NULL;

/* Forward declarations (prototype-before-use under -Werror). */
static bool   cjson_scalar_str(const cJSON *node, char *buf, size_t len);
static void   invalidate_tiles_config(void);
static int    get_tiles_config(const char *tiles_config_json);
static cJSON *fetch_entity_core(const char *base_url, const char *token,
                                const char *entity_id, http_fetch_conn_t *conn);
static bool   resolve_tile_value(cJSON *entity, const char *attr,
                                 char *out, size_t out_len);

// =============================================================================
// Mutex Helpers (mirror json_client_init/lock/unlock)
// =============================================================================

void ha_client_init(ha_data_t *data) {
    if (!data) {
        return;
    }
    memset(data, 0, sizeof(ha_data_t));
    data->connected = false;
    data->tile_count = 0;
    data->last_poll_ms = 0;
    data->mutex = xSemaphoreCreateMutex();
}

bool ha_client_lock(ha_data_t *data, int timeout_ms) {
    if (!data || !data->mutex) {
        return false;
    }
    return xSemaphoreTake(data->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ha_client_unlock(ha_data_t *data) {
    if (data && data->mutex) {
        xSemaphoreGive(data->mutex);
    }
}

// =============================================================================
// Scalar extraction -- string/number/bool -> string (mirrors json_client:95-125)
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
// Tiles config cache -- parse-once + row-major (entity_id, attr) list
// =============================================================================

void ha_client_invalidate_config_cache(void) {
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
        s_tile_entities[i] = NULL;
        s_tile_attrs[i] = NULL;
    }
    s_tile_count = 0;
}

/**
 * Ensure the tiles config is parsed and the row-major (entity_id, attr) list is
 * built. Returns the number of flattened tiles (0 on empty/parse failure). On a
 * cache hit (config string unchanged) the cached list is reused unchanged.
 */
static int get_tiles_config(const char *tiles_config_json) {
    if (!tiles_config_json || tiles_config_json[0] == '\0') {
        invalidate_tiles_config();
        return 0;
    }

    /* Cache hit -- config string unchanged. */
    if (s_cached_tiles && s_cached_tiles_str &&
        strcmp(s_cached_tiles_str, tiles_config_json) == 0) {
        return s_tile_count;
    }

    /* Cache miss -- invalidate and re-parse. */
    invalidate_tiles_config();

    s_cached_tiles = cJSON_Parse(tiles_config_json);
    if (!s_cached_tiles) {
        ESP_LOGW(TAG, "Failed to parse ha_tiles_config");
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

    /* Flatten rows[][] into a row-major (entity_id, attr) list. */
    cJSON *rows = cJSON_GetObjectItem(s_cached_tiles, "rows");
    if (cJSON_IsArray(rows)) {
        cJSON *row = NULL;
        cJSON_ArrayForEach(row, rows) {
            if (s_tile_count >= JSON_MAX_TILES) {
                break;
            }
            if (!cJSON_IsArray(row)) {
                continue;
            }
            cJSON *tile = NULL;
            cJSON_ArrayForEach(tile, row) {
                if (s_tile_count >= JSON_MAX_TILES) {
                    break;
                }
                cJSON *ent_item = cJSON_GetObjectItem(tile, "entity_id");
                const char *ent = NULL;
                if (cJSON_IsString(ent_item) && ent_item->valuestring[0] != '\0') {
                    ent = ent_item->valuestring;
                }
                cJSON *attr_item = cJSON_GetObjectItem(tile, "attr");
                const char *attr = NULL;
                if (cJSON_IsString(attr_item) && attr_item->valuestring[0] != '\0') {
                    attr = attr_item->valuestring;   /* NULL => "state" at resolve */
                }
                s_tile_entities[s_tile_count] = ent;   /* NULL => unresolved slot */
                s_tile_attrs[s_tile_count] = attr;
                s_tile_count++;
            }
        }
    }

    ESP_LOGD(TAG, "HA tiles config cache updated: %d tiles", s_tile_count);
    return s_tile_count;
}

// =============================================================================
// Value resolution -- state or attributes.<attr>
// =============================================================================

/**
 * Resolve @p attr against a parsed entity JSON into @p out.
 *  - attr NULL/""/"state" (case-insensitive) -> top-level "state".
 *  - otherwise -> attributes.<attr>.
 * Returns false (out set to "") when the node is missing/non-scalar, or when the
 * scalar equals "unavailable"/"unknown" (case-insensitive) -- caller marks the
 * tile unresolved so the renderer shows "--".
 */
static bool resolve_tile_value(cJSON *entity, const char *attr,
                               char *out, size_t out_len) {
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    if (!entity) {
        return false;
    }

    const cJSON *node = NULL;
    if (!attr || attr[0] == '\0' || strcasecmp(attr, "state") == 0) {
        node = cJSON_GetObjectItem(entity, "state");
    } else {
        const cJSON *attrs = cJSON_GetObjectItem(entity, "attributes");
        if (cJSON_IsObject(attrs)) {
            node = cJSON_GetObjectItem(attrs, attr);
        }
    }

    if (!cjson_scalar_str(node, out, out_len)) {
        return false;
    }

    if (strcasecmp(out, "unavailable") == 0 || strcasecmp(out, "unknown") == 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

// =============================================================================
// Single-entity fetch
// =============================================================================

/**
 * Fetch GET {base_url}/api/states/{entity_id} with Bearer auth. @p conn may be a
 * keep-alive slot (poll task) or NULL for a one-shot client (probe handler).
 * Returns the parsed entity JSON (caller cJSON_Delete) or NULL on failure.
 */
static cJSON *fetch_entity_core(const char *base_url, const char *token,
                                const char *entity_id, http_fetch_conn_t *conn) {
    if (!base_url || base_url[0] == '\0' || !entity_id || entity_id[0] == '\0') {
        return NULL;
    }

    /* Strip any trailing slash(es) from base so "{base}/api/states/{id}" never
     * produces a double slash. base_url is <=255 chars. */
    char base[256];
    size_t blen = strlen(base_url);
    if (blen >= sizeof(base)) {
        ESP_LOGW(TAG, "HA base_url too long");
        return NULL;
    }
    memcpy(base, base_url, blen + 1);
    while (blen > 0 && base[blen - 1] == '/') {
        base[--blen] = '\0';
    }

    /* url[512] covers base[256] + "/api/states/" + entity_id (<=127). */
    char url[512];
    int un = snprintf(url, sizeof(url), "%s/api/states/%s", base, entity_id);
    if (un <= 0 || un >= (int)sizeof(url)) {
        ESP_LOGW(TAG, "HA URL truncated for entity %s", entity_id);
        return NULL;
    }

    /* auth[288] covers "Authorization: Bearer " (22) + token[256]. */
    char auth[288];
    auth[0] = '\0';
    if (token && token[0] != '\0') {
        int an = snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
        if (an <= 0 || an >= (int)sizeof(auth)) {
            ESP_LOGW(TAG, "HA auth header truncated");
            auth[0] = '\0';
        }
    }

    bool is_https = (strncasecmp(url, "https", 5) == 0);

    http_fetch_opts_t opts = {
        .timeout_ms = HA_HTTP_TIMEOUT_MS,
        .use_tls_bundle = is_https,
        .max_redirects = 3,
        .max_attempts = 1,
        .max_response_bytes = HA_RESPONSE_BUF_SIZE,
        .extra_header = (auth[0] != '\0') ? auth : NULL,
        .conn = conn,
    };

    char *buffer = NULL;
    size_t total_read = 0;
    esp_err_t err = http_fetch_text(url, &opts, &buffer, &total_read);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HA fetch failed for %s: %s", entity_id, esp_err_to_name(err));
        return NULL;
    }
    if (total_read == 0) {
        ESP_LOGW(TAG, "HA: empty response for %s", entity_id);
        heap_caps_free(buffer);
        return NULL;
    }

    cJSON *json = cJSON_Parse(buffer);
    heap_caps_free(buffer);
    if (!json) {
        ESP_LOGW(TAG, "HA: failed to parse response for %s", entity_id);
        return NULL;
    }
    return json;
}

cJSON *ha_client_fetch_entity(const char *base_url, const char *token,
                              const char *entity_id) {
    /* One-shot (conn=NULL): safe to call from the httpd worker task. */
    return fetch_entity_core(base_url, token, entity_id, NULL);
}

// =============================================================================
// Public API -- poll all HA entities
// =============================================================================

void ha_client_poll(const char *base_url, const char *token,
                    const char *tiles_config_json, ha_data_t *data) {
    if (!base_url || base_url[0] == '\0' || !data) {
        return;
    }

    int count = get_tiles_config(tiles_config_json);
    if (count > JSON_MAX_TILES) {
        count = JSON_MAX_TILES;
    }
    if (count <= 0) {
        /* No tiles configured -- nothing to fetch. Publish connected=true so the
         * page adapter's overlay ordering (base -> !connected -> tile_count==0)
         * surfaces "No Tiles Configured" rather than "Cannot Reach". Mirrors
         * json_client leaving connected=true on a successful fetch of 0 tiles. */
        if (ha_client_lock(data, 100)) {
            data->connected = true;
            data->tile_count = 0;
            data->last_poll_ms = esp_timer_get_time() / 1000;
            ha_client_unlock(data);
        }
        return;
    }

    /* Lazily create the keep-alive slot on first poll. On failure, fall through
     * with s_conn == NULL -- http_fetch uses a one-shot client per fetch. */
    if (!s_conn) {
        s_conn = http_fetch_conn_create();
        if (!s_conn) {
            ESP_LOGW(TAG, "Failed to allocate HA keep-alive connection slot");
        }
    }

    /* De-dupe unique entity_ids (O(n^2), n<=JSON_MAX_TILES). Each unique entity is fetched
     * ONCE, sequentially, reusing s_conn; the parsed JSON is reused for every
     * tile that references it. */
    char   unique_ids[JSON_MAX_TILES][HA_ENTITY_ID_LEN];
    cJSON *unique_json[JSON_MAX_TILES];
    int    unique_count = 0;
    int    fetched_ok = 0;

    for (int i = 0; i < JSON_MAX_TILES; i++) {
        unique_json[i] = NULL;
    }

    for (int i = 0; i < count; i++) {
        const char *ent = s_tile_entities[i];
        if (!ent || ent[0] == '\0') {
            continue;
        }
        if (strlen(ent) >= HA_ENTITY_ID_LEN) {
            ESP_LOGW(TAG, "HA entity id too long, skipping: %s", ent);
            continue;
        }
        bool seen = false;
        for (int u = 0; u < unique_count; u++) {
            if (strcmp(unique_ids[u], ent) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        /* New unique entity -- fetch it now (sequential, keep-alive reuse). */
        snprintf(unique_ids[unique_count], HA_ENTITY_ID_LEN, "%s", ent);
        cJSON *ej = fetch_entity_core(base_url, token, ent, s_conn);
        unique_json[unique_count] = ej;   /* NULL on failure */
        if (ej) {
            fetched_ok++;
        }
        unique_count++;
    }

    /* Resolve every tile from the de-duped parsed entities into local scratch. */
    ha_data_t local;
    memset(&local, 0, sizeof(local));
    local.mutex = NULL;   /* local scratch -- never locked */

    for (int i = 0; i < count; i++) {
        const char *ent = s_tile_entities[i];
        if (!ent || ent[0] == '\0') {
            local.values[i][0] = '\0';
            local.resolved[i] = false;
            continue;
        }
        cJSON *ej = NULL;
        for (int u = 0; u < unique_count; u++) {
            if (strcmp(unique_ids[u], ent) == 0) {
                ej = unique_json[u];
                break;
            }
        }
        bool ok = resolve_tile_value(ej, s_tile_attrs[i],
                                     local.values[i], JSON_TILE_VALUE_LEN);
        local.resolved[i] = ok;
        if (!ok) {
            local.values[i][0] = '\0';
        }
    }
    local.tile_count = count;

    /* Release parsed entities. */
    for (int u = 0; u < unique_count; u++) {
        if (unique_json[u]) {
            cJSON_Delete(unique_json[u]);
        }
    }

    /* Publish under the mutex. connected = at least one entity fetched OK. */
    if (ha_client_lock(data, 200)) {
        data->connected = (fetched_ok > 0);
        data->last_poll_ms = esp_timer_get_time() / 1000;
        data->tile_count = local.tile_count;
        memcpy(data->values, local.values, sizeof(data->values));
        memcpy(data->resolved, local.resolved, sizeof(data->resolved));
        ha_client_unlock(data);
        ESP_LOGD(TAG, "HA poll: %d tiles, %d/%d entities OK",
                 count, fetched_ok, unique_count);
    } else {
        ESP_LOGW(TAG, "HA: failed to acquire mutex for data update");
    }
}
