/**
 * @file nina_tile_grid.c
 * @brief Source-agnostic row-based tile grid (extracted from nina_json.c).
 *
 * Vertical flex of row containers; each row is a horizontal flex whose tiles
 * flex-grow to fill the width; rows split the 720px height evenly. Each tile is
 * a bento box with an uppercase label, a large value, and an optional unit.
 * Values recolor by threshold (Number), value map (Text), or true/false state
 * (Boolean). No header band. Empty / unreachable states use nina_empty_state
 * as a full-coverage overlay (own FLOATING + 100% + opaque black backdrop).
 *
 * Modeled 1:1 on the original nina_json.c (bento styling, cached-color churn
 * avoidance, apply_theme / refresh_config lifecycle). All LVGL calls run under
 * the display lock held by the caller; this module never takes the lock itself.
 * The grid handle is source-agnostic: it knows nothing about JSON paths or HA
 * entities, only the display tiles-config keys and resolved value strings.
 */

#include "nina_tile_grid.h"
#include "nina_dashboard_internal.h"  /* current_theme, OUTER_PADDING, GRID_GAP, SCREEN_SIZE */
#include "nina_empty_state.h"
#include "app_config.h"
#include "themes.h"
#include "ui_styles.h"
#include "ui_helpers.h"
#include "display_defs.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "tile_grid";

/* Extended value-font ladder fonts (generated, compressed bpp4). The built-in
 * montserrat 14/16/22/28/36/48 are enabled via sdkconfig. */
LV_FONT_DECLARE(lv_font_montserrat_64);

/* ── Layout caps (match the mockup / tiles_config schema) ─────────────── */
#define TILE_GRID_MAX_ROWS      6
#define TILE_GRID_MAX_PER_ROW   4

/* Threshold color defaults taken from the on-device Default theme.
 * Used only as parse fallbacks; live colors come from the per-tile config. */
#define C_NORMAL   0xa3a3a3u
#define C_WARN     0xd97706u
#define C_CRIT     0xbe123cu
#define C_OK       0x059669u

/* ── Per-tile parsed config (row-major flatten order) ─────────────────── */

typedef enum { TG_NUMBER = 0, TG_TEXT = 1, TG_BOOL = 2 } tile_grid_type_t;

typedef struct {
    char     val[24];
    uint32_t color;
} tile_grid_val_map_t;

typedef struct {
    tile_grid_type_t type;
    char     label[32];
    /* number */
    char     unit[12];
    int      decimals;
    float    low;
    float    high;
    uint32_t c_low;
    uint32_t c_norm;
    uint32_t c_high;
    /* text */
    tile_grid_val_map_t maps[4];
    int      map_count;
    /* bool */
    char     t_text[24];
    char     f_text[24];
    uint32_t t_color;
    uint32_t f_color;
} tile_grid_tile_cfg_t;

/* ── Per-tile widget storage ──────────────────────────────────────────── */

typedef struct {
    lv_obj_t *tile;       /* bento box */
    lv_obj_t *lbl_label;
    lv_obj_t *lbl_value;
    lv_obj_t *lbl_unit;
    /* Cached value color — only restyle when changed (avoids full redraws). */
    uint32_t  cached_value_color;
    /* Row count at build time — used as the layout-not-ready fallback. */
    int       rows;
    /* Fit-to-tile font caches — only restyle when the chosen font changes. */
    const lv_font_t *cached_value_font;
    const lv_font_t *cached_unit_font;
    const lv_font_t *cached_label_font;
    /* Last value string measured for the fit; skip re-measuring when unchanged. */
    char      cached_fit_str[48];
} tile_grid_w_t;

/* Ascending font ladders with nominal point sizes for the proportional caps. */
typedef struct { const lv_font_t *font; int size; } tile_font_rung_t;

/* ── Opaque handle (all former file-static state lives here) ──────────── */

struct nina_tile_grid {
    lv_obj_t *root;          /* page root */
    lv_obj_t *rows_host;     /* flex-column holding the row containers */
    lv_obj_t *empty_overlay; /* full-coverage empty/error overlay */

    tile_grid_tile_cfg_t tile_cfg[JSON_MAX_TILES];
    tile_grid_w_t        tile_w[JSON_MAX_TILES];
    int                  tile_count;                     /* flattened tile count */
    int                  row_count;                      /* number of rows */
    int                  row_tile_counts[TILE_GRID_MAX_ROWS]; /* tiles per row */
};

/* ── Forward declarations of static helpers ───────────────────────────── */

static inline uint32_t theme_text_color(int gb);
static inline uint32_t theme_label_color(int gb);
static inline void set_text_if_changed(lv_obj_t *lbl, const char *text);
static inline void set_color_if_changed(lv_obj_t *obj, uint32_t *cached, uint32_t color);
static uint32_t parse_hex_color(const char *str, uint32_t fallback);
static const char *obj_str(const cJSON *o, const char *key);
static bool ci_equal(const char *a, const char *b);
static bool truthy(const char *raw);
static const lv_font_t *value_font_for_rows(int rows);
static const lv_font_t *label_font_for_rows(int rows);
static inline void set_font_if_changed(lv_obj_t *obj, const lv_font_t **cached,
                                       const lv_font_t *font);
static const lv_font_t *unit_font_for_value_size(int value_size);
static void fit_tile_fonts(tile_grid_w_t *tw, const tile_grid_tile_cfg_t *tc,
                           const char *value_str, const char *unit_str);
static void parse_one_tile(const cJSON *t, tile_grid_tile_cfg_t *tc);
static void parse_tiles_config(nina_tile_grid_t *g, const char *tiles_config_json);
static void build_tile(tile_grid_w_t *tw, lv_obj_t *parent,
                       const tile_grid_tile_cfg_t *tc, int rows, int gb);
static void build_rows(nina_tile_grid_t *g);
static void render_tile(tile_grid_w_t *tw, const tile_grid_tile_cfg_t *tc,
                        const char *raw, bool resolved, int gb);

/* ── Small helpers ────────────────────────────────────────────────────── */

static inline uint32_t theme_text_color(int gb) {
    return app_config_apply_brightness(current_theme->text_color, gb);
}
static inline uint32_t theme_label_color(int gb) {
    return app_config_apply_brightness(current_theme->label_color, gb);
}

static inline void set_text_if_changed(lv_obj_t *lbl, const char *text) {
    if (strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

static inline void set_color_if_changed(lv_obj_t *obj, uint32_t *cached, uint32_t color) {
    if (*cached != color) {
        *cached = color;
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    }
}

static uint32_t parse_hex_color(const char *str, uint32_t fallback) {
    if (!str || str[0] == '\0') {
        return fallback;
    }
    const char *p = str;
    if (*p == '#') {
        p++;
    }
    char *end;
    unsigned long val = strtoul(p, &end, 16);
    if (end == p) {
        return fallback;
    }
    return (uint32_t)val;
}

static const char *obj_str(const cJSON *o, const char *key) {
    cJSON *j = cJSON_GetObjectItem(o, key);
    return (j && cJSON_IsString(j)) ? j->valuestring : NULL;
}

/* Case-insensitive full-string equality (avoids strcasecmp portability). */
static bool ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Truthiness — false set: false,0,no,off,unsafe,closed,"". */
static bool truthy(const char *raw) {
    /* Left-trim, copy into a bounded buffer, right-trim, lowercase. */
    const char *p = raw;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char t[32];
    snprintf(t, sizeof(t), "%s", p);
    size_t l = strlen(t);
    while (l > 0 && (t[l - 1] == ' ' || t[l - 1] == '\t')) {
        t[--l] = '\0';
    }
    for (size_t i = 0; i < l; i++) {
        t[i] = (char)tolower((unsigned char)t[i]);
    }
    if (l == 0) {
        return false;
    }
    static const char *falses[] = { "false", "0", "no", "off", "unsafe", "closed" };
    for (int i = 0; i < 6; i++) {
        if (strcmp(t, falses[i]) == 0) {
            return false;
        }
    }
    return true;
}

/* Value/label font scales down as the row count grows so many rows never
 * overflow vertically (each row splits 720px evenly). */
static const lv_font_t *value_font_for_rows(int rows) {
    if (rows <= 2) {
        return &lv_font_montserrat_48;
    }
    if (rows == 3) {
        return &lv_font_montserrat_36;
    }
    if (rows == 4) {
        return &lv_font_montserrat_28;
    }
    return &lv_font_montserrat_22;
}
static const lv_font_t *label_font_for_rows(int rows) {
    return (rows >= 5) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
}

/* Ascending value ladder — the fit picks the largest rung that fits the tile. */
static const tile_font_rung_t VALUE_LADDER[] = {
    { &lv_font_montserrat_16, 16 },
    { &lv_font_montserrat_22, 22 },
    { &lv_font_montserrat_28, 28 },
    { &lv_font_montserrat_36, 36 },
    { &lv_font_montserrat_48, 48 },
    { &lv_font_montserrat_64, 64 },
};
#define VALUE_LADDER_N ((int)(sizeof(VALUE_LADDER) / sizeof(VALUE_LADDER[0])))

/* Ascending label ladder — kept smaller so labels never dominate the value. */
static const tile_font_rung_t LABEL_LADDER[] = {
    { &lv_font_montserrat_14, 14 },
    { &lv_font_montserrat_16, 16 },
    { &lv_font_montserrat_22, 22 },
    { &lv_font_montserrat_28, 28 },
};
#define LABEL_LADDER_N ((int)(sizeof(LABEL_LADDER) / sizeof(LABEL_LADDER[0])))

/* Width headroom: leave ~8% slack so glyphs never touch the tile edge. */
#define FIT_WIDTH_PCT 92
/* Sum of inter-line gaps in the label/value/unit stack (2 * pad_row of 4). */
#define FIT_STACK_GAP 8

static inline void set_font_if_changed(lv_obj_t *obj, const lv_font_t **cached,
                                       const lv_font_t *font) {
    if (*cached != font) {
        *cached = font;
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

/* Scale the unit proportionally to the chosen value font (simple switch). */
static const lv_font_t *unit_font_for_value_size(int value_size) {
    if (value_size >= 64) {
        return &lv_font_montserrat_28;
    }
    if (value_size >= 48) {
        return &lv_font_montserrat_22;
    }
    return &lv_font_montserrat_16;
}

/* Measure width of a string in a given font (letter_space matches the label
 * style so the fit is accurate). Returns pixel width. */
static int fit_text_width(const char *str, const lv_font_t *font, int letter_space) {
    lv_point_t sz;
    lv_text_get_size(&sz, str, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)sz.x;
}

/* Pick the largest value font that fits both the tile's inner width and the
 * combined line-height budget; scale unit + label proportionally. Caches the
 * chosen fonts and the source value string to avoid LVGL invalidation churn. */
static void fit_tile_fonts(tile_grid_w_t *tw, const tile_grid_tile_cfg_t *tc,
                           const char *value_str, const char *unit_str) {
    if (!tw->tile || !tw->lbl_value) {
        return;
    }

    int cw = lv_obj_get_content_width(tw->tile);
    int ch = lv_obj_get_content_height(tw->tile);

    /* Layout not ready on the first paint — fall back to the row-based fonts
     * and leave the cache open so a later pass re-fits with real dimensions. */
    if (cw <= 0 || ch <= 0) {
        set_font_if_changed(tw->lbl_value, &tw->cached_value_font,
                            value_font_for_rows(tw->rows));
        set_font_if_changed(tw->lbl_label, &tw->cached_label_font,
                            label_font_for_rows(tw->rows));
        set_font_if_changed(tw->lbl_unit, &tw->cached_unit_font,
                            &lv_font_montserrat_16);
        tw->cached_fit_str[0] = '\0';
        return;
    }

    /* Same value string, already fitted with valid dims → nothing to do. */
    if (tw->cached_value_font && strcmp(tw->cached_fit_str, value_str) == 0) {
        return;
    }

    int avail_w = (cw * FIT_WIDTH_PCT) / 100;
    bool has_unit = (unit_str && unit_str[0] != '\0');
    bool has_label = (tc->label[0] != '\0');

    /* Base label line-height for the height budget: widest label rung that fits
     * the width. Capped again below once the value size is known. */
    const tile_font_rung_t *lbl_base = &LABEL_LADDER[0];
    for (int i = LABEL_LADDER_N - 1; i >= 0; i--) {
        if (fit_text_width(tc->label, LABEL_LADDER[i].font, 2) <= avail_w) {
            lbl_base = &LABEL_LADDER[i];
            break;
        }
    }
    int label_lh = has_label ? (int)lv_font_get_line_height(lbl_base->font) : 0;

    /* Largest value rung whose width fits AND whose stacked height fits. */
    const tile_font_rung_t *val = &VALUE_LADDER[0];   /* montserrat_16 floor */
    for (int i = VALUE_LADDER_N - 1; i >= 0; i--) {
        const lv_font_t *vf = VALUE_LADDER[i].font;
        int w = fit_text_width(value_str, vf, 0);
        if (w > avail_w) {
            continue;
        }
        int value_lh = (int)lv_font_get_line_height(vf);
        int unit_lh = has_unit
                        ? (int)lv_font_get_line_height(
                              unit_font_for_value_size(VALUE_LADDER[i].size))
                        : 0;
        int total_h = value_lh + label_lh + unit_lh + FIT_STACK_GAP;
        if (total_h <= ch) {
            val = &VALUE_LADDER[i];
            break;
        }
    }

    const lv_font_t *value_font = val->font;
    const lv_font_t *unit_font  = unit_font_for_value_size(val->size);

    /* Label capped at ~40% of value size, then width-fit within that cap. */
    int label_cap = (val->size * 40) / 100;
    const tile_font_rung_t *lbl = &LABEL_LADDER[0];   /* montserrat_14 floor */
    for (int i = LABEL_LADDER_N - 1; i >= 0; i--) {
        if (LABEL_LADDER[i].size > label_cap) {
            continue;
        }
        if (fit_text_width(tc->label, LABEL_LADDER[i].font, 2) <= avail_w) {
            lbl = &LABEL_LADDER[i];
            break;
        }
    }

    set_font_if_changed(tw->lbl_value, &tw->cached_value_font, value_font);
    set_font_if_changed(tw->lbl_unit,  &tw->cached_unit_font,  unit_font);
    set_font_if_changed(tw->lbl_label, &tw->cached_label_font, lbl->font);

    snprintf(tw->cached_fit_str, sizeof(tw->cached_fit_str), "%s", value_str);
}

/* ── Config parsing (tiles_config → tile_cfg[] + row structure) ────────── */

static void parse_one_tile(const cJSON *t, tile_grid_tile_cfg_t *tc) {
    memset(tc, 0, sizeof(*tc));

    /* Defaults (mockup convertType). */
    tc->type     = TG_NUMBER;
    tc->decimals = 1;
    tc->low      = 0.0f;
    tc->high     = 100.0f;
    tc->c_low    = C_NORMAL;
    tc->c_norm   = C_NORMAL;
    tc->c_high   = C_WARN;
    tc->t_color  = C_OK;
    tc->f_color  = C_CRIT;

    const char *label = obj_str(t, "label");
    if (label) {
        snprintf(tc->label, sizeof(tc->label), "%s", label);
        /* Typography hierarchy: labels render uppercase (display-only; the
         * saved config keeps the user's original casing). */
        for (char *p = tc->label; *p != '\0'; p++) {
            *p = (char)toupper((unsigned char)*p);
        }
    }

    const char *ty = obj_str(t, "type");
    if (ty && strcmp(ty, "text") == 0) {
        tc->type = TG_TEXT;
    } else if (ty && strcmp(ty, "bool") == 0) {
        tc->type = TG_BOOL;
    } else {
        tc->type = TG_NUMBER;
    }

    if (tc->type == TG_NUMBER) {
        const char *unit = obj_str(t, "unit");
        if (unit) {
            snprintf(tc->unit, sizeof(tc->unit), "%s", unit);
        }
        cJSON *j_dec = cJSON_GetObjectItem(t, "decimals");
        if (cJSON_IsNumber(j_dec)) {
            tc->decimals = j_dec->valueint;
        }
        if (tc->decimals < 0) {
            tc->decimals = 0;
        }
        if (tc->decimals > 4) {
            tc->decimals = 4;
        }
        cJSON *j_low = cJSON_GetObjectItem(t, "low");
        if (cJSON_IsNumber(j_low)) {
            tc->low = (float)j_low->valuedouble;
        }
        cJSON *j_high = cJSON_GetObjectItem(t, "high");
        if (cJSON_IsNumber(j_high)) {
            tc->high = (float)j_high->valuedouble;
        }
        tc->c_low  = parse_hex_color(obj_str(t, "cLow"),  C_NORMAL);
        tc->c_norm = parse_hex_color(obj_str(t, "cNorm"), C_NORMAL);
        tc->c_high = parse_hex_color(obj_str(t, "cHigh"), C_WARN);
    } else if (tc->type == TG_TEXT) {
        cJSON *maps = cJSON_GetObjectItem(t, "maps");
        int mc = 0;
        if (cJSON_IsArray(maps)) {
            cJSON *m;
            cJSON_ArrayForEach(m, maps) {
                if (mc >= 4) {
                    break;
                }
                if (!cJSON_IsObject(m)) {
                    continue;
                }
                const char *v = obj_str(m, "val");
                snprintf(tc->maps[mc].val, sizeof(tc->maps[mc].val), "%s", v ? v : "");
                tc->maps[mc].color = parse_hex_color(obj_str(m, "color"), C_NORMAL);
                mc++;
            }
        }
        tc->map_count = mc;
    } else { /* TG_BOOL */
        const char *tt = obj_str(t, "tText");
        const char *ft = obj_str(t, "fText");
        snprintf(tc->t_text, sizeof(tc->t_text), "%s", tt ? tt : "TRUE");
        snprintf(tc->f_text, sizeof(tc->f_text), "%s", ft ? ft : "FALSE");
        tc->t_color = parse_hex_color(obj_str(t, "tColor"), C_OK);
        tc->f_color = parse_hex_color(obj_str(t, "fColor"), C_CRIT);
    }
}

/* Walk rows[][] row-major (identical order to the client's value list) into
 * tile_cfg[], recording per-row counts. Caps: 6 rows, 4/row, JSON_MAX_TILES total. */
static void parse_tiles_config(nina_tile_grid_t *g, const char *tiles_config_json) {
    memset(g->tile_cfg, 0, sizeof(g->tile_cfg));
    memset(g->row_tile_counts, 0, sizeof(g->row_tile_counts));
    g->tile_count = 0;
    g->row_count  = 0;

    if (!tiles_config_json) {
        return;
    }

    cJSON *root = cJSON_Parse(tiles_config_json);
    if (!root) {
        return;
    }
    cJSON *rows = cJSON_GetObjectItem(root, "rows");
    if (!cJSON_IsArray(rows)) {
        cJSON_Delete(root);
        return;
    }

    int ri = 0;
    cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        if (ri >= TILE_GRID_MAX_ROWS) {
            break;
        }
        if (!cJSON_IsArray(row)) {
            continue;
        }
        int in_row = 0;
        cJSON *tile;
        cJSON_ArrayForEach(tile, row) {
            if (g->tile_count >= JSON_MAX_TILES || in_row >= TILE_GRID_MAX_PER_ROW) {
                break;
            }
            if (!cJSON_IsObject(tile)) {
                continue;
            }
            parse_one_tile(tile, &g->tile_cfg[g->tile_count]);
            g->tile_count++;
            in_row++;
        }
        g->row_tile_counts[ri] = in_row;
        ri++;
    }
    g->row_count = ri;

    cJSON_Delete(root);
}

/* ── Widget construction ──────────────────────────────────────────────── */

static void build_tile(tile_grid_w_t *tw, lv_obj_t *parent,
                       const tile_grid_tile_cfg_t *tc, int rows, int gb) {
    memset(tw, 0, sizeof(*tw));
    tw->rows = rows;

    tw->tile = create_bento_box(parent);
    lv_obj_set_flex_grow(tw->tile, 1);            /* fill row width evenly */
    lv_obj_set_height(tw->tile, LV_PCT(100));
    lv_obj_clear_flag(tw->tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tw->tile, LV_FLEX_FLOW_COLUMN);
    /* Structural anchoring: pin the label to the top and the unit to the
     * bottom, letting the value occupy (and define) the middle of the tile. */
    lv_obj_set_flex_align(tw->tile, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tw->tile, 12, 0);
    lv_obj_set_style_pad_row(tw->tile, 4, 0);

    /* Label (uppercase-styled via letter spacing; config text used verbatim). */
    tw->lbl_label = lv_label_create(tw->tile);
    lv_obj_set_style_text_font(tw->lbl_label, label_font_for_rows(rows), 0);
    tw->cached_label_font = label_font_for_rows(rows);
    lv_obj_set_style_text_letter_space(tw->lbl_label, 2, 0);
    lv_obj_set_style_text_align(tw->lbl_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(tw->lbl_label, LV_PCT(100));
    lv_label_set_long_mode(tw->lbl_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(tw->lbl_label, tc->label);
    if (current_theme) {
        lv_obj_set_style_text_color(tw->lbl_label, lv_color_hex(theme_label_color(gb)), 0);
    }

    /* Value. */
    tw->lbl_value = lv_label_create(tw->tile);
    lv_obj_set_style_text_font(tw->lbl_value, value_font_for_rows(rows), 0);
    tw->cached_value_font = value_font_for_rows(rows);
    lv_obj_set_style_text_align(tw->lbl_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(tw->lbl_value, LV_PCT(100));
    lv_label_set_long_mode(tw->lbl_value, LV_LABEL_LONG_DOT);
    lv_label_set_text(tw->lbl_value, "--");
    tw->cached_value_color = UINT32_MAX;   /* force first recolor */
    if (current_theme) {
        lv_obj_set_style_text_color(tw->lbl_value, lv_color_hex(theme_text_color(gb)), 0);
    }

    /* Unit (empty for text/bool tiles). */
    tw->lbl_unit = lv_label_create(tw->tile);
    lv_obj_set_style_text_font(tw->lbl_unit, &lv_font_montserrat_16, 0);
    tw->cached_unit_font = &lv_font_montserrat_16;
    lv_obj_set_style_text_align(tw->lbl_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(tw->lbl_unit, LV_PCT(100));
    lv_label_set_long_mode(tw->lbl_unit, LV_LABEL_LONG_DOT);
    lv_label_set_text(tw->lbl_unit, "");
    if (current_theme) {
        lv_obj_set_style_text_color(tw->lbl_unit, lv_color_hex(theme_label_color(gb)), 0);
    }
}

static void build_rows(nina_tile_grid_t *g) {
    int gb = app_config_get()->color_brightness;

    g->rows_host = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->rows_host);
    lv_obj_clear_flag(g->rows_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(g->rows_host, LV_PCT(100));
    lv_obj_set_flex_grow(g->rows_host, 1);
    lv_obj_set_flex_flow(g->rows_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g->rows_host, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g->rows_host, GRID_GAP, 0);

    memset(g->tile_w, 0, sizeof(g->tile_w));

    int flat = 0;
    for (int r = 0; r < g->row_count; r++) {
        lv_obj_t *rowc = lv_obj_create(g->rows_host);
        lv_obj_remove_style_all(rowc);
        lv_obj_clear_flag(rowc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(rowc, LV_PCT(100));
        lv_obj_set_flex_grow(rowc, 1);          /* even split of the 720px height */
        lv_obj_set_flex_flow(rowc, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rowc, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(rowc, GRID_GAP, 0);

        for (int c = 0; c < g->row_tile_counts[r]; c++) {
            if (flat >= JSON_MAX_TILES) {
                break;
            }
            build_tile(&g->tile_w[flat], rowc, &g->tile_cfg[flat], g->row_count, gb);
            flat++;
        }
    }
}

/* ── Per-tile render ──────────────────────────────────────────────────── */

static void render_tile(tile_grid_w_t *tw, const tile_grid_tile_cfg_t *tc,
                        const char *raw, bool resolved, int gb) {
    if (!tw->lbl_value) {
        return;
    }

    char buf[48];
    const char *disp = "--";        /* value string to display */
    const char *unit_txt = "";      /* only Number tiles carry a unit */
    uint32_t color = theme_text_color(gb);

    if (!resolved) {
        /* Unresolved / missing → "--" in theme text color (still scales up). */
        disp = "--";
    } else if (tc->type == TG_NUMBER) {
        char *end = NULL;
        float v = strtof(raw, &end);
        if (end == raw) {
            /* Non-numeric string — treat as missing. */
            disp = "--";
        } else {
            int dec = tc->decimals;
            if (dec < 0) {
                dec = 0;
            }
            if (dec > 4) {
                dec = 4;
            }
            snprintf(buf, sizeof(buf), "%.*f", dec, (double)v);
            disp = buf;
            uint32_t c;
            if (v < tc->low) {
                c = tc->c_low;
            } else if (v > tc->high) {
                c = tc->c_high;
            } else {
                c = tc->c_norm;
            }
            color = app_config_apply_brightness(c, gb);
            unit_txt = tc->unit;
        }
    } else if (tc->type == TG_TEXT) {
        snprintf(buf, sizeof(buf), "%s", raw);
        disp = buf;
        for (int m = 0; m < tc->map_count; m++) {
            if (tc->maps[m].val[0] != '\0' && ci_equal(tc->maps[m].val, raw)) {
                color = app_config_apply_brightness(tc->maps[m].color, gb);
                break;
            }
        }
    } else { /* TG_BOOL */
        bool on = truthy(raw);
        const char *txt = on ? tc->t_text : tc->f_text;
        if (txt[0] == '\0') {
            txt = on ? "TRUE" : "FALSE";
        }
        disp = txt;
        color = app_config_apply_brightness(on ? tc->t_color : tc->f_color, gb);
    }

    set_text_if_changed(tw->lbl_value, disp);
    set_color_if_changed(tw->lbl_value, &tw->cached_value_color, color);
    set_text_if_changed(tw->lbl_unit, unit_txt);

    /* Re-fit the value/unit/label fonts to the tile whenever the value text
     * changes (fit_tile_fonts short-circuits when the string is unchanged). */
    fit_tile_fonts(tw, tc, disp, unit_txt);
}

/* ── Public API ───────────────────────────────────────────────────────── */

nina_tile_grid_t *nina_tile_grid_create(lv_obj_t *parent,
                                        const char *tiles_config_json,
                                        const char *icon,
                                        const char *empty_title,
                                        const char *empty_remedy) {
    if (!parent) {
        return NULL;
    }

    nina_tile_grid_t *g = heap_caps_malloc(sizeof(*g), MALLOC_CAP_SPIRAM);
    if (!g) {
        ESP_LOGE(TAG, "Failed to allocate tile grid handle");
        return NULL;
    }
    memset(g, 0, sizeof(*g));

    parse_tiles_config(g, tiles_config_json);

    g->root = lv_obj_create(parent);
    lv_obj_remove_style_all(g->root);
    lv_obj_set_size(g->root, SCREEN_SIZE - 2 * OUTER_PADDING,
                    SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_clear_flag(g->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g->root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g->root, GRID_GAP, 0);

    build_rows(g);

    /* Full-coverage empty/error overlay (own FLOATING + 100% + opaque black
     * backdrop, per nina_empty_state usage contract). Mirrors nina_spotify. */
    g->empty_overlay = nina_empty_state_create(g->root, icon,
                                               empty_title, empty_remedy, 0);
    if (g->empty_overlay) {
        lv_obj_move_foreground(g->empty_overlay);
        lv_obj_add_flag(g->empty_overlay, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(g->empty_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(g->empty_overlay, 0, 0);
        lv_obj_set_style_bg_color(g->empty_overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(g->empty_overlay, LV_OPA_COVER, 0);
    }

    return g;
}

lv_obj_t *nina_tile_grid_get_root(nina_tile_grid_t *g) {
    return g ? g->root : NULL;
}

void nina_tile_grid_update(nina_tile_grid_t *g,
                           const char values[][JSON_TILE_VALUE_LEN],
                           const bool *resolved, int count) {
    if (!g || !g->root || !values || !resolved) {
        return;
    }

    int gb = app_config_get()->color_brightness;

    /* Resolve the flex layout once so per-tile content_width/height reflect the
     * actual laid-out geometry before the fit-to-tile font pass runs. */
    lv_obj_update_layout(g->root);

    for (int i = 0; i < g->tile_count && i < JSON_MAX_TILES && i < count; i++) {
        tile_grid_w_t *tw = &g->tile_w[i];
        if (!tw->lbl_value) {
            continue;
        }
        const char *raw = values[i];
        bool r = resolved[i] && raw[0] != '\0';
        render_tile(tw, &g->tile_cfg[i], raw, r, gb);
    }
}

void nina_tile_grid_show_overlay(nina_tile_grid_t *g, const char *title) {
    if (!g || !g->empty_overlay) {
        return;
    }
    nina_empty_state_set_title(g->empty_overlay, title);  /* no-op when unchanged */
    nina_empty_state_show(g->empty_overlay);              /* idempotent */
}

void nina_tile_grid_hide_overlay(nina_tile_grid_t *g) {
    if (g && g->empty_overlay) {
        nina_empty_state_hide(g->empty_overlay);
    }
}

void nina_tile_grid_apply_theme(nina_tile_grid_t *g) {
    if (!g || !g->root || !current_theme) {
        return;
    }
    int gb = app_config_get()->color_brightness;

    for (int i = 0; i < g->tile_count && i < JSON_MAX_TILES; i++) {
        tile_grid_w_t *tw = &g->tile_w[i];
        if (tw->lbl_label) {
            lv_obj_set_style_text_color(tw->lbl_label, lv_color_hex(theme_label_color(gb)), 0);
        }
        if (tw->lbl_unit) {
            lv_obj_set_style_text_color(tw->lbl_unit, lv_color_hex(theme_label_color(gb)), 0);
        }
        /* Reset value to theme text now; force a full recolor on next update. */
        if (tw->lbl_value) {
            lv_obj_set_style_text_color(tw->lbl_value, lv_color_hex(theme_text_color(gb)), 0);
        }
        tw->cached_value_color = UINT32_MAX;
    }

    if (g->empty_overlay) {
        nina_empty_state_apply_theme(g->empty_overlay, current_theme, gb);
    }

    lv_obj_invalidate(g->root);
}

void nina_tile_grid_refresh_config(nina_tile_grid_t *g, const char *tiles_config_json) {
    if (!g || !g->root) {
        return;
    }

    parse_tiles_config(g, tiles_config_json);

    /* Tear down the existing rows (deletes child tiles too), then rebuild.
     * empty_overlay is a separate FLOATING child of root and survives. */
    if (g->rows_host) {
        lv_obj_delete(g->rows_host);
        g->rows_host = NULL;
    }
    memset(g->tile_w, 0, sizeof(g->tile_w));

    build_rows(g);

    /* Keep the overlay above the freshly rebuilt rows. */
    if (g->empty_overlay) {
        lv_obj_move_foreground(g->empty_overlay);
    }

    ESP_LOGI(TAG, "Tile grid rebuilt: %d rows, %d tiles", g->row_count, g->tile_count);
}
