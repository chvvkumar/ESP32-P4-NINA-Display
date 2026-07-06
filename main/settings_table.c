#include "settings_table.h"
#include "app_config.h"
#include "themes.h"
#include "cJSON.h"
#include <string.h>

/* -- settings_defaults_apply() ------------------------------------------- */

#define DEF_BOOL(field, json_key, def) \
    cfg->field = (def);
#define DEF_INT(field, json_key, def, min, max) \
    cfg->field = (def);
#define DEF_INT_RESET(field, json_key, def, min, max) \
    cfg->field = (def);
#define DEF_FLT(field, json_key, def, min, max) \
    cfg->field = (def);
#define DEF_FLT_RESET(field, json_key, def, min, max) \
    cfg->field = (def);
#define DEF_ENUM(field, json_key, def, count_expr) \
    cfg->field = (def);
#define DEF_STR(field, json_key, def) \
    do { \
        strncpy(cfg->field, (def), sizeof(cfg->field) - 1); \
        cfg->field[sizeof(cfg->field) - 1] = '\0'; \
    } while (0);
#define DEF_STR_RESET(field, json_key, def) \
    do { \
        strncpy(cfg->field, (def), sizeof(cfg->field) - 1); \
        cfg->field[sizeof(cfg->field) - 1] = '\0'; \
    } while (0);

void settings_defaults_apply(app_config_t *cfg) {
    SETTINGS_TABLE(DEF_BOOL, DEF_INT, DEF_INT_RESET, DEF_FLT, DEF_FLT_RESET,
                   DEF_ENUM, DEF_STR, DEF_STR_RESET)
}

#undef DEF_BOOL
#undef DEF_INT
#undef DEF_INT_RESET
#undef DEF_FLT
#undef DEF_FLT_RESET
#undef DEF_ENUM
#undef DEF_STR
#undef DEF_STR_RESET

/* -- Shared CLAMP_* macros -------------------------------------------------
 * Single source of the bound/reset code text for each row kind. Used by
 * BOTH settings_clamp_apply() (below) and settings_json_parse() (further
 * down, via the PARSE_* macros) so the bound-check logic for a given kind
 * exists exactly once in this file. Each macro references a local `fixed`
 * bool in its caller's scope; settings_clamp_apply() threads it through as
 * its return value, while settings_json_parse()'s PARSE_* wrappers declare
 * a throwaway per-field `fixed` (the parse path doesn't need to report
 * "was anything out of range", only settings_clamp_apply() does).
 *
 * Cast to `long` before comparing against `min`/`max`: several rows use an
 * unsigned field type with a literal min of 0 (e.g. deep_sleep_wake_timer_s,
 * uint32_t), where `cfg->field < 0` is always false and would otherwise trip
 * -Wtype-limits. All table values fit comfortably in a (32-bit) long. */
#define CLAMP_BOOL(field, json_key, def) \
    /* deliberate no-op: see settings_table.h kind comment */
#define CLAMP_INT(field, json_key, def, min, max) \
    if ((long)cfg->field < (long)(min)) { cfg->field = (min); fixed = true; } \
    else if ((long)cfg->field > (long)(max)) { cfg->field = (max); fixed = true; }
#define CLAMP_INT_RESET(field, json_key, def, min, max) \
    if ((long)cfg->field < (long)(min) || (long)cfg->field > (long)(max)) { cfg->field = (def); fixed = true; }
#define CLAMP_FLT(field, json_key, def, min, max) \
    if (cfg->field < (min)) { cfg->field = (min); fixed = true; } \
    else if (cfg->field > (max)) { cfg->field = (max); fixed = true; }
#define CLAMP_FLT_RESET(field, json_key, def, min, max) \
    if (cfg->field < (min) || cfg->field > (max)) { cfg->field = (def); fixed = true; }
#define CLAMP_ENUM(field, json_key, def, count_expr) \
    if ((int)cfg->field < 0 || (int)cfg->field >= (count_expr)) { cfg->field = (def); fixed = true; }
#define CLAMP_STR(field, json_key, def) \
    cfg->field[sizeof(cfg->field) - 1] = '\0';
#define CLAMP_STR_RESET(field, json_key, def) \
    cfg->field[sizeof(cfg->field) - 1] = '\0'; \
    if (cfg->field[0] == '\0') { \
        strncpy(cfg->field, (def), sizeof(cfg->field) - 1); \
        cfg->field[sizeof(cfg->field) - 1] = '\0'; \
        fixed = true; \
    }

/* -- settings_clamp_apply() ----------------------------------------------
 * `fixed` (declared below) accumulates whether any row's value was out of
 * range and got changed, mirroring the `fixed` bool that validate_config()
 * already threads through the rest of its per-field checks. */
bool settings_clamp_apply(app_config_t *cfg) {
    bool fixed = false;
    SETTINGS_TABLE(CLAMP_BOOL, CLAMP_INT, CLAMP_INT_RESET, CLAMP_FLT, CLAMP_FLT_RESET,
                   CLAMP_ENUM, CLAMP_STR, CLAMP_STR_RESET)
    return fixed;
}

/* -- settings_json_serialize() -------------------------------------------
 * One cJSON_Add*ToObject call per row, keyed by json_key. */

#define SER_BOOL(field, json_key, def) \
    cJSON_AddBoolToObject(root, json_key, cfg->field);
#define SER_INT(field, json_key, def, min, max) \
    cJSON_AddNumberToObject(root, json_key, (double)cfg->field);
#define SER_INT_RESET(field, json_key, def, min, max) \
    cJSON_AddNumberToObject(root, json_key, (double)cfg->field);
#define SER_FLT(field, json_key, def, min, max) \
    cJSON_AddNumberToObject(root, json_key, (double)cfg->field);
#define SER_FLT_RESET(field, json_key, def, min, max) \
    cJSON_AddNumberToObject(root, json_key, (double)cfg->field);
#define SER_ENUM(field, json_key, def, count_expr) \
    cJSON_AddNumberToObject(root, json_key, (double)cfg->field);
#define SER_STR(field, json_key, def) \
    cJSON_AddStringToObject(root, json_key, cfg->field);
#define SER_STR_RESET(field, json_key, def) \
    cJSON_AddStringToObject(root, json_key, cfg->field);

void settings_json_serialize(const app_config_t *cfg, cJSON *root) {
    SETTINGS_TABLE(SER_BOOL, SER_INT, SER_INT_RESET, SER_FLT, SER_FLT_RESET,
                   SER_ENUM, SER_STR, SER_STR_RESET)
}

#undef SER_BOOL
#undef SER_INT
#undef SER_INT_RESET
#undef SER_FLT
#undef SER_FLT_RESET
#undef SER_ENUM
#undef SER_STR
#undef SER_STR_RESET

/* -- settings_json_parse() ------------------------------------------------
 * For each row: look up json_key in `root`; if present and its JSON type
 * matches the row's kind, assign into cfg->field and then apply the same
 * clamp/reset rule as settings_clamp_apply() by invoking the CLAMP_* macro
 * for that kind (still in scope from above — this is the "reuse the
 * existing CLAMP_* macros directly inside PARSE_*" approach, so the
 * bound-check code text is not duplicated). If the key is absent or the
 * wrong JSON type, cfg->field is left completely untouched — the clamp is
 * only ever applied to a value that was actually just assigned from JSON. */

#define PARSE_BOOL(field, json_key, def) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsBool(_it)) { \
            bool fixed = false; (void)fixed; \
            cfg->field = cJSON_IsTrue(_it); \
            CLAMP_BOOL(field, json_key, def) \
        } \
    } while (0);
#define PARSE_INT(field, json_key, def, min, max) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsNumber(_it)) { \
            bool fixed = false; (void)fixed; \
            cfg->field = (__typeof__(cfg->field))_it->valueint; \
            CLAMP_INT(field, json_key, def, min, max) \
        } \
    } while (0);
#define PARSE_INT_RESET(field, json_key, def, min, max) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsNumber(_it)) { \
            bool fixed = false; (void)fixed; \
            cfg->field = (__typeof__(cfg->field))_it->valueint; \
            CLAMP_INT_RESET(field, json_key, def, min, max) \
        } \
    } while (0);
#define PARSE_FLT(field, json_key, def, min, max) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsNumber(_it)) { \
            bool fixed = false; (void)fixed; \
            cfg->field = (float)_it->valuedouble; \
            CLAMP_FLT(field, json_key, def, min, max) \
        } \
    } while (0);
#define PARSE_FLT_RESET(field, json_key, def, min, max) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsNumber(_it)) { \
            bool fixed = false; (void)fixed; \
            cfg->field = (float)_it->valuedouble; \
            CLAMP_FLT_RESET(field, json_key, def, min, max) \
        } \
    } while (0);
#define PARSE_ENUM(field, json_key, def, count_expr) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsNumber(_it)) { \
            bool fixed = false; (void)fixed; \
            cfg->field = (__typeof__(cfg->field))_it->valueint; \
            CLAMP_ENUM(field, json_key, def, count_expr) \
        } \
    } while (0);
#define PARSE_STR(field, json_key, def) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsString(_it)) { \
            bool fixed = false; (void)fixed; \
            strncpy(cfg->field, _it->valuestring, sizeof(cfg->field) - 1); \
            cfg->field[sizeof(cfg->field) - 1] = '\0'; \
            CLAMP_STR(field, json_key, def) \
        } \
    } while (0);
#define PARSE_STR_RESET(field, json_key, def) \
    do { \
        cJSON *_it = cJSON_GetObjectItem(root, json_key); \
        if (cJSON_IsString(_it)) { \
            bool fixed = false; (void)fixed; \
            strncpy(cfg->field, _it->valuestring, sizeof(cfg->field) - 1); \
            cfg->field[sizeof(cfg->field) - 1] = '\0'; \
            CLAMP_STR_RESET(field, json_key, def) \
        } \
    } while (0);

void settings_json_parse(const cJSON *root, app_config_t *cfg) {
    SETTINGS_TABLE(PARSE_BOOL, PARSE_INT, PARSE_INT_RESET, PARSE_FLT, PARSE_FLT_RESET,
                   PARSE_ENUM, PARSE_STR, PARSE_STR_RESET)
}

#undef PARSE_BOOL
#undef PARSE_INT
#undef PARSE_INT_RESET
#undef PARSE_FLT
#undef PARSE_FLT_RESET
#undef PARSE_ENUM
#undef PARSE_STR
#undef PARSE_STR_RESET

#undef CLAMP_BOOL
#undef CLAMP_INT
#undef CLAMP_INT_RESET
#undef CLAMP_FLT
#undef CLAMP_FLT_RESET
#undef CLAMP_ENUM
#undef CLAMP_STR
#undef CLAMP_STR_RESET
