#pragma once

/**
 * @file json_get.h
 * @brief Guard-and-assign helpers for reading scalars out of a cJSON object.
 *
 * Protocol-agnostic twin of the JSON_TO_* macros in web_server_internal.h,
 * which stay web-handler-only. Used by the data clients (NINA fetchers,
 * weather, sequence, WebSocket) that previously hand-rolled the same
 * "fetch item, guard, assign" block hundreds of times.
 *
 * Every macro leaves @p dest untouched when the key is absent or of the wrong
 * type, so a partial payload never clobbers a previously good value.
 *
 * Type guards are cJSON_IsString / cJSON_IsNumber / cJSON_IsBool. That is
 * deliberately stricter than the `item && item->valuestring` checks these
 * replaced: a JSON null, or a field whose type changed, is now ignored instead
 * of read through.
 *
 * @p obj may be NULL -- cJSON_GetObjectItem() is NULL-safe -- so a nested
 * lookup can be written inline:
 *     JSON_GET_FLOAT(cJSON_GetObjectItem(rms, "Total"), "Arcseconds", x);
 *
 * JSON_GET_STR takes sizeof(dest), so @p dest must be a char ARRAY. Passing a
 * char pointer copies at most sizeof(void *) - 1 bytes.
 */

#include <string.h>
#include "cJSON.h"

/* Copy a string field into a fixed char array; always NUL-terminated. */
#define JSON_GET_STR(obj, key, dest) do { \
    cJSON *_jg = cJSON_GetObjectItem(obj, key); \
    if (cJSON_IsString(_jg)) { \
        strncpy(dest, _jg->valuestring, sizeof(dest) - 1); \
        (dest)[sizeof(dest) - 1] = '\0'; \
    } \
} while (0)

/* As JSON_GET_STR, but an empty string is also ignored -- for fields where the
 * source sends "" to mean "unknown" and the previous value is worth keeping. */
#define JSON_GET_STR_NONEMPTY(obj, key, dest) do { \
    cJSON *_jg = cJSON_GetObjectItem(obj, key); \
    if (cJSON_IsString(_jg) && _jg->valuestring[0] != '\0') { \
        strncpy(dest, _jg->valuestring, sizeof(dest) - 1); \
        (dest)[sizeof(dest) - 1] = '\0'; \
    } \
} while (0)

#define JSON_GET_INT(obj, key, dest) do { \
    cJSON *_jg = cJSON_GetObjectItem(obj, key); \
    if (cJSON_IsNumber(_jg)) { \
        dest = _jg->valueint; \
    } \
} while (0)

#define JSON_GET_FLOAT(obj, key, dest) do { \
    cJSON *_jg = cJSON_GetObjectItem(obj, key); \
    if (cJSON_IsNumber(_jg)) { \
        dest = (float)_jg->valuedouble; \
    } \
} while (0)

#define JSON_GET_BOOL(obj, key, dest) do { \
    cJSON *_jg = cJSON_GetObjectItem(obj, key); \
    if (cJSON_IsBool(_jg)) { \
        dest = cJSON_IsTrue(_jg); \
    } \
} while (0)

/* Value form, for the "read with a fallback" sites the statement macros cannot
 * express (`x = item ? item->value : default`). */
static inline float json_num_or(const cJSON *obj, const char *key, float def) {
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : def;
}

static inline int json_int_or(const cJSON *obj, const char *key, int def) {
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(it) ? it->valueint : def;
}
