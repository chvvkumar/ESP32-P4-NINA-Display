/* Host test for main/settings_table.c (X-macro-driven defaults/clamp for the
 * "simple" app_config_t fields). Style follows test/host/test_session_stats.c:
 * main() with numbered sections, CHECK macro, running failure counter.
 *
 * themes_get_count() is the only runtime count_expr used by the table
 * (theme_index row). It is not stubbed anywhere else in test/host, so a
 * minimal local stub is provided here (not linking the real ui/themes.c,
 * which pulls in LVGL).
 */
#include "../../main/app_config.h"
#include "../../main/settings_table.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond, fmt, ...) \
    do { \
        int _ok = (cond); \
        printf("  %s: " fmt "\n", _ok ? "OK  " : "FAIL", ##__VA_ARGS__); \
        if (!_ok) fails++; \
    } while (0)

/* Minimal stand-in for main/ui/themes.c — only themes_get_count() is needed
 * by the table (theme_index's ENUM row). Matches the real theme count
 * (9 built-in dark themes) so the row's default/clamp bound is realistic,
 * but the exact number is not load-bearing for this test. */
int themes_get_count(void) { return 9; }

int main(void)
{
    /* ── 1. settings_defaults_apply() is idempotent under settings_clamp_apply() ──
     * A freshly-defaulted config must already be "clean": running the clamp
     * pass over it must report no fix and must not change any migrated
     * field. This is the core defaults/clamp consistency guarantee. */
    printf("1. defaults -> clamp is a no-op\n");
    {
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        settings_defaults_apply(&cfg);

        app_config_t before = cfg;
        bool fixed = settings_clamp_apply(&cfg);

        CHECK(fixed == false, "settings_clamp_apply() reports no fix on fresh defaults");
        CHECK(memcmp(&before, &cfg, sizeof(cfg)) == 0,
              "settings_clamp_apply() did not mutate a freshly-defaulted config");
    }

    /* ── 2. Per-row out-of-range behavior, generated from the X-macro ──
     * For every INT/INT_RESET/FLT/FLT_RESET/ENUM row: push the field below
     * min and above max, then confirm settings_clamp_apply() lands on the
     * value the row's kind promises (nearest bound for a true clamp, `def`
     * for a reset). BOOL/STR rows are covered by sections 1/3 instead. */
    printf("2. per-row range enforcement\n");

#define ROW_BOOL(field, json_key, def)                        /* covered by section 1 */
#define ROW_STR(field, json_key, def)                          /* covered by section 3 */
#define ROW_STR_RESET(field, json_key, def)                    /* covered by section 3 */

/* Below-min is only meaningful/testable when min > 0: every INT/INT_RESET
 * row's field is an unsigned integer type (uint8/16/32_t) except theme_index
 * (handled by ROW_ENUM) — for a row with min == 0, (min - 1) would wrap
 * around to the type's max value instead of underflowing below 0, which
 * is not what "below-min" is supposed to exercise. Guarding on (min) > 0
 * skips that sub-check for the (many) upper-bound-only rows, matching the
 * fact that validate_config() never had a lower-bound check to replicate
 * for those fields in the first place. */
#define ROW_INT(field, json_key, def, min, max) \
    do { \
        app_config_t cfg; \
        memset(&cfg, 0, sizeof(cfg)); \
        settings_defaults_apply(&cfg); \
        __typeof__(cfg.field) _type_max = (__typeof__(cfg.field))~(__typeof__(cfg.field))0; \
        if ((min) > 0) { \
            cfg.field = (__typeof__(cfg.field))((min) - 1); \
            settings_clamp_apply(&cfg); \
            CHECK(cfg.field == (min), "%s: below-min clamps to min (%ld)", json_key, (long)(min)); \
        } \
        if ((unsigned long)(max) < (unsigned long)_type_max) { \
            cfg.field = (__typeof__(cfg.field))((max) + 1); \
            settings_clamp_apply(&cfg); \
            CHECK(cfg.field == (max), "%s: above-max clamps to max (%ld)", json_key, (long)(max)); \
        } \
    } while (0);

#define ROW_INT_RESET(field, json_key, def, min, max) \
    do { \
        app_config_t cfg; \
        memset(&cfg, 0, sizeof(cfg)); \
        settings_defaults_apply(&cfg); \
        __typeof__(cfg.field) _type_max = (__typeof__(cfg.field))~(__typeof__(cfg.field))0; \
        if ((min) > 0) { \
            cfg.field = (__typeof__(cfg.field))((min) - 1); \
            settings_clamp_apply(&cfg); \
            CHECK(cfg.field == (def), "%s: below-min resets to def (%ld)", json_key, (long)(def)); \
        } \
        if ((unsigned long)(max) < (unsigned long)_type_max) { \
            cfg.field = (__typeof__(cfg.field))((max) + 1); \
            settings_clamp_apply(&cfg); \
            CHECK(cfg.field == (def), "%s: above-max resets to def (%ld)", json_key, (long)(def)); \
        } \
    } while (0);

#define ROW_FLT(field, json_key, def, min, max) \
    do { \
        app_config_t cfg; \
        memset(&cfg, 0, sizeof(cfg)); \
        settings_defaults_apply(&cfg); \
        cfg.field = (min) - 1.0f; \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field == (float)(min), "%s: below-min clamps to min (%f)", json_key, (double)(min)); \
        cfg.field = (max) + 1.0f; \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field == (float)(max), "%s: above-max clamps to max (%f)", json_key, (double)(max)); \
    } while (0);

#define ROW_FLT_RESET(field, json_key, def, min, max) \
    do { \
        app_config_t cfg; \
        memset(&cfg, 0, sizeof(cfg)); \
        settings_defaults_apply(&cfg); \
        cfg.field = (min) - 1.0f; \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field == (float)(def), "%s: below-min resets to def (%f)", json_key, (double)(def)); \
        cfg.field = (max) + 1.0f; \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field == (float)(def), "%s: above-max resets to def (%f)", json_key, (double)(def)); \
    } while (0);

#define ROW_ENUM(field, json_key, def, count_expr) \
    do { \
        app_config_t cfg; \
        memset(&cfg, 0, sizeof(cfg)); \
        settings_defaults_apply(&cfg); \
        cfg.field = (__typeof__(cfg.field))(-1); \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field == (def), "%s: negative resets to def (%ld)", json_key, (long)(def)); \
        cfg.field = (__typeof__(cfg.field))(count_expr); \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field == (def), "%s: >= count resets to def (%ld)", json_key, (long)(def)); \
    } while (0);

    SETTINGS_TABLE(ROW_BOOL, ROW_INT, ROW_INT_RESET, ROW_FLT, ROW_FLT_RESET,
                   ROW_ENUM, ROW_STR, ROW_STR_RESET)

#undef ROW_BOOL
#undef ROW_INT
#undef ROW_INT_RESET
#undef ROW_FLT
#undef ROW_FLT_RESET
#undef ROW_ENUM
#undef ROW_STR
#undef ROW_STR_RESET

    /* ── 3. STR / STR_RESET rows: NUL-termination and empty-reset ── */
    printf("3. string rows: NUL-termination + empty-reset\n");

#define ROW2_BOOL(field, json_key, def)
#define ROW2_INT(field, json_key, def, min, max)
#define ROW2_INT_RESET(field, json_key, def, min, max)
#define ROW2_FLT(field, json_key, def, min, max)
#define ROW2_FLT_RESET(field, json_key, def, min, max)
#define ROW2_ENUM(field, json_key, def, count_expr)

#define ROW2_STR(field, json_key, def) \
    do { \
        app_config_t cfg; \
        memset(&cfg, 0, sizeof(cfg)); \
        settings_defaults_apply(&cfg); \
        memset(cfg.field, 'A', sizeof(cfg.field)); /* unterminated garbage */ \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field[sizeof(cfg.field) - 1] == '\0', "%s: force-NUL-terminated", json_key); \
    } while (0);

#define ROW2_STR_RESET(field, json_key, def) \
    do { \
        app_config_t cfg; \
        memset(&cfg, 0, sizeof(cfg)); \
        settings_defaults_apply(&cfg); \
        memset(cfg.field, 'A', sizeof(cfg.field)); /* unterminated garbage */ \
        settings_clamp_apply(&cfg); \
        CHECK(cfg.field[sizeof(cfg.field) - 1] == '\0', "%s: force-NUL-terminated", json_key); \
        cfg.field[0] = '\0'; /* now empty */ \
        settings_clamp_apply(&cfg); \
        CHECK(strcmp(cfg.field, (def)) == 0, "%s: empty string resets to def", json_key); \
    } while (0);

    SETTINGS_TABLE(ROW2_BOOL, ROW2_INT, ROW2_INT_RESET, ROW2_FLT, ROW2_FLT_RESET,
                   ROW2_ENUM, ROW2_STR, ROW2_STR_RESET)

#undef ROW2_BOOL
#undef ROW2_INT
#undef ROW2_INT_RESET
#undef ROW2_FLT
#undef ROW2_FLT_RESET
#undef ROW2_ENUM
#undef ROW2_STR
#undef ROW2_STR_RESET

    /* ── 4. settings_json_serialize() + settings_json_parse() round-trip ──
     * Perturb every row to a value that differs from its default (still
     * in-range, so the perturbation itself would survive settings_clamp_apply
     * unmodified), serialize that config to JSON, parse the JSON into a
     * SEPARATE defaults-initialized config, and confirm every SETTINGS_TABLE
     * field made it across unchanged. X-macro-generated so newly added rows
     * are covered automatically. */
    printf("4. serialize -> parse round-trip\n");
    {
        app_config_t src;
        memset(&src, 0, sizeof(src));
        settings_defaults_apply(&src);

#define PERTURB_BOOL(field, json_key, def) src.field = !(def);
#define PERTURB_INT(field, json_key, def, min, max) \
        src.field = (__typeof__(src.field))(((max) != (def)) ? (max) : (min));
#define PERTURB_INT_RESET(field, json_key, def, min, max) \
        src.field = (__typeof__(src.field))(((max) != (def)) ? (max) : (min));
#define PERTURB_FLT(field, json_key, def, min, max) \
        src.field = (((max) != (def)) ? (max) : (min));
#define PERTURB_FLT_RESET(field, json_key, def, min, max) \
        src.field = (((max) != (def)) ? (max) : (min));
#define PERTURB_ENUM(field, json_key, def, count_expr) \
        src.field = (__typeof__(src.field))((count_expr) - 1);
#define PERTURB_STR(field, json_key, def) \
        strncpy(src.field, "roundtrip", sizeof(src.field) - 1);
#define PERTURB_STR_RESET(field, json_key, def) \
        strncpy(src.field, "roundtrip", sizeof(src.field) - 1);

        SETTINGS_TABLE(PERTURB_BOOL, PERTURB_INT, PERTURB_INT_RESET, PERTURB_FLT, PERTURB_FLT_RESET,
                       PERTURB_ENUM, PERTURB_STR, PERTURB_STR_RESET)

#undef PERTURB_BOOL
#undef PERTURB_INT
#undef PERTURB_INT_RESET
#undef PERTURB_FLT
#undef PERTURB_FLT_RESET
#undef PERTURB_ENUM
#undef PERTURB_STR
#undef PERTURB_STR_RESET

        cJSON *root = cJSON_CreateObject();
        settings_json_serialize(&src, root);

        app_config_t dst;
        memset(&dst, 0, sizeof(dst));
        settings_defaults_apply(&dst);   /* dst starts at defaults: differs from src on every row */
        settings_json_parse(root, &dst);
        cJSON_Delete(root);

#define COMPARE_BOOL(field, json_key, def) \
        CHECK(dst.field == src.field, "%s: round-trips", json_key);
#define COMPARE_INT(field, json_key, def, min, max) \
        CHECK(dst.field == src.field, "%s: round-trips", json_key);
#define COMPARE_INT_RESET(field, json_key, def, min, max) \
        CHECK(dst.field == src.field, "%s: round-trips", json_key);
#define COMPARE_FLT(field, json_key, def, min, max) \
        CHECK(dst.field == src.field, "%s: round-trips", json_key);
#define COMPARE_FLT_RESET(field, json_key, def, min, max) \
        CHECK(dst.field == src.field, "%s: round-trips", json_key);
#define COMPARE_ENUM(field, json_key, def, count_expr) \
        CHECK(dst.field == src.field, "%s: round-trips", json_key);
#define COMPARE_STR(field, json_key, def) \
        CHECK(strcmp(dst.field, src.field) == 0, "%s: round-trips", json_key);
#define COMPARE_STR_RESET(field, json_key, def) \
        CHECK(strcmp(dst.field, src.field) == 0, "%s: round-trips", json_key);

        SETTINGS_TABLE(COMPARE_BOOL, COMPARE_INT, COMPARE_INT_RESET, COMPARE_FLT, COMPARE_FLT_RESET,
                       COMPARE_ENUM, COMPARE_STR, COMPARE_STR_RESET)

#undef COMPARE_BOOL
#undef COMPARE_INT
#undef COMPARE_INT_RESET
#undef COMPARE_FLT
#undef COMPARE_FLT_RESET
#undef COMPARE_ENUM
#undef COMPARE_STR
#undef COMPARE_STR_RESET
    }

    /* ── 5. settings_json_parse(): out-of-range JSON values get clamped/reset ──
     * Spot-checks across kinds (per the task's own examples: brightness,
     * mqtt_port), not a full X-macro sweep — a few representative rows are
     * enough to prove settings_json_parse() actually invokes the same
     * CLAMP_* logic as settings_clamp_apply() (already exhaustively swept
     * in section 2 above) rather than skipping it. */
    printf("5. settings_json_parse(): out-of-range values clamped/reset\n");
    {
        /* brightness: INT_RESET, def 50, range 0..100. int field (32-bit) —
         * 9999 does not wrap on cast, so this exercises a genuine reset. */
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        settings_defaults_apply(&cfg);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "brightness", 9999);
        settings_json_parse(root, &cfg);
        CHECK(cfg.brightness == 50, "brightness: 9999 resets to def (50) via JSON parse");
        cJSON_Delete(root);
    }
    {
        /* mqtt_port: INT_RESET, def 1883, range 1..65535 (the full uint16_t
         * domain except 0). A too-large JSON number just wraps back into
         * range on cast to uint16_t, so use 0 -- the one value that casts
         * to something below min(1) -- to trigger a genuine reset. */
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        settings_defaults_apply(&cfg);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "mqtt_port", 0);
        settings_json_parse(root, &cfg);
        CHECK(cfg.mqtt_port == 1883, "mqtt_port: 0 resets to def (1883) via JSON parse");
        cJSON_Delete(root);
    }
    {
        /* weather_lat: FLT (true clamp, not reset), def 0.0, range -90..90. */
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        settings_defaults_apply(&cfg);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "weather_lat", 200.0);
        settings_json_parse(root, &cfg);
        CHECK(cfg.weather_lat == 90.0f, "weather_lat: 200 clamps to max (90) via JSON parse");
        cJSON_Delete(root);
    }
    {
        /* theme_index: ENUM, def 0, count themes_get_count()=9 (stub above). */
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        settings_defaults_apply(&cfg);
        cfg.theme_index = 3;   /* pre-set to a non-default in-range value */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "theme_index", 99);
        settings_json_parse(root, &cfg);
        CHECK(cfg.theme_index == 0, "theme_index: 99 (>= count) resets to def (0) via JSON parse");
        cJSON_Delete(root);
    }

    /* ── 6. settings_json_parse(): missing keys leave fields untouched ── */
    printf("6. missing keys leave fields untouched\n");
    {
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        settings_defaults_apply(&cfg);
        cfg.brightness = 77;                 /* sentinel: not the default, still in-range */
        strncpy(cfg.hostname, "sentinel-host", sizeof(cfg.hostname) - 1);
        cfg.mqtt_enabled = true;             /* sentinel: not the default (false) */

        cJSON *root = cJSON_CreateObject();   /* deliberately empty */
        settings_json_parse(root, &cfg);
        cJSON_Delete(root);

        CHECK(cfg.brightness == 77, "brightness: missing key leaves field untouched");
        CHECK(strcmp(cfg.hostname, "sentinel-host") == 0, "hostname: missing key leaves field untouched");
        CHECK(cfg.mqtt_enabled == true, "mqtt_enabled: missing key leaves field untouched");
    }

    /* ── 7. settings_json_parse(): wrong-type JSON values are ignored ── */
    printf("7. wrong-type values ignored (field left untouched)\n");
    {
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        settings_defaults_apply(&cfg);
        cfg.brightness = 77;
        strncpy(cfg.hostname, "sentinel-host", sizeof(cfg.hostname) - 1);
        cfg.mqtt_enabled = false;   /* sentinel chosen to differ from the wrong-type value's content ("true"),
                                     * so a bug that coerced the string instead of ignoring it would be caught */

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "brightness", "not_a_number");   /* wrong type for INT_RESET */
        cJSON_AddNumberToObject(root, "hostname", 12345);              /* wrong type for STR */
        cJSON_AddStringToObject(root, "mqtt_enabled", "true");         /* wrong type for BOOL (string, not JSON bool) */
        settings_json_parse(root, &cfg);
        cJSON_Delete(root);

        CHECK(cfg.brightness == 77, "brightness: wrong-type value ignored");
        CHECK(strcmp(cfg.hostname, "sentinel-host") == 0, "hostname: wrong-type value ignored");
        CHECK(cfg.mqtt_enabled == false, "mqtt_enabled: wrong-type value ignored");
    }

    printf("\n%s: %d failure(s)\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
