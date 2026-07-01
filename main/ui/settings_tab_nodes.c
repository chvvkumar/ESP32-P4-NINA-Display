/**
 * @file settings_tab_nodes.c
 * @brief Nodes & Data tab — instance enable strip + a segmented node picker
 *        with a single flex-grow detail panel per node (hostname, filter
 *        colors, and RMS/HFR thresholds).
 *
 * Part of the on-device settings tabview. Layout is master-detail:
 *   - Active Instances enable strip (3 toggles)
 *   - Segmented [Node 1][Node 2][Node 3] picker (buttonmatrix, seg_mode idiom)
 *   - One detail panel shown at a time (3 pre-built, others hidden):
 *       - Hostname text input
 *       - Filter color swatches (parsed from JSON config)
 *       - RMS threshold steppers and color swatches
 *       - HFR threshold steppers and color swatches
 *
 * Disabled nodes get a locked (dimmed, non-selectable) segment via
 * LV_BUTTONMATRIX_CTRL_DISABLED; all-disabled shows an empty-state hint.
 */

#include "settings_tab_nodes.h"
#include "nina_settings_tabview.h"
#include "settings_color_picker.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include "ui_styles.h"
#include "nina_client.h"  /* MAX_NINA_INSTANCES */
#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"

/* ── Constants ──────────────────────────────────────────────────────── */
#define NODE_MAX_FILTERS 12

#define SWATCH_SIZE       44
#define SWATCH_GAP        6
#define SWATCH_RADIUS     6

#define THRESH_SWATCH_SIZE 32
#define THRESH_SWATCH_RAD  6

#define RMS_STEP      0.1f
#define RMS_GOOD_MIN  0.1f
#define RMS_GOOD_MAX  5.0f
#define RMS_WARN_MIN  0.1f
#define RMS_WARN_MAX  10.0f

#define HFR_STEP      0.5f
#define HFR_GOOD_MIN  0.5f
#define HFR_GOOD_MAX  10.0f
#define HFR_WARN_MIN  0.5f
#define HFR_WARN_MAX  20.0f

/* ── Per-node widget/data structure ─────────────────────────────────── */
typedef struct {
    lv_obj_t *sw_enable;
    lv_obj_t *ta_hostname;
    lv_obj_t *filter_row;
    lv_obj_t *filter_swatches[NODE_MAX_FILTERS];
    char      filter_names[NODE_MAX_FILTERS][16];
    uint32_t  filter_colors_hex[NODE_MAX_FILTERS];
    int       filter_count;
    /* RMS thresholds */
    lv_obj_t *lbl_rms_good_val;
    lv_obj_t *lbl_rms_warn_val;
    lv_obj_t *swatch_rms_good;
    lv_obj_t *swatch_rms_warn;
    lv_obj_t *swatch_rms_bad;
    float     rms_good_max;
    float     rms_warn_max;
    uint32_t  rms_good_color;
    uint32_t  rms_warn_color;
    uint32_t  rms_bad_color;
    /* HFR thresholds */
    lv_obj_t *lbl_hfr_good_val;
    lv_obj_t *lbl_hfr_warn_val;
    lv_obj_t *swatch_hfr_good;
    lv_obj_t *swatch_hfr_warn;
    lv_obj_t *swatch_hfr_bad;
    float     hfr_good_max;
    float     hfr_warn_max;
    uint32_t  hfr_good_color;
    uint32_t  hfr_warn_color;
    uint32_t  hfr_bad_color;
} node_widgets_t;

static node_widgets_t nodes[MAX_NINA_INSTANCES];
static lv_obj_t *tab_root = NULL;

/* ── Master-detail layout state ─────────────────────────────────────── */
static lv_obj_t *node_seg = NULL;                          /* segmented picker */
static const char *node_seg_map[] = {"Node 1", "Node 2", "Node 3", ""};
static lv_obj_t *node_detail[MAX_NINA_INSTANCES] = {0};    /* 3 pre-built panels */
static lv_obj_t *empty_hint = NULL;                        /* all-disabled hint */
static int selected_node = 0;                              /* current selection */

/* ── Forward declarations ───────────────────────────────────────────── */
static void parse_filter_colors(int idx);
static void parse_rms_thresholds(int idx);
static void parse_hfr_thresholds(int idx);
static void rebuild_filter_json(int node_idx);
static void rebuild_rms_json(int node_idx);
static void rebuild_hfr_json(int node_idx);
static void create_filter_swatches(int idx);
static void create_threshold_section(lv_obj_t *parent, int idx, bool is_hfr);
static void build_node_detail(lv_obj_t *parent, int idx);
static void node_seg_changed_cb(lv_event_t *e);
static void nodes_sync_segments(void);
static void nodes_apply_enable_change(void);

/* ── Enable toggle callback ─────────────────────────────────────────── */
static void enable_toggle_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(nodes[idx].sw_enable, LV_STATE_CHECKED);
    app_config_get()->instance_enabled[idx] = on;
    settings_mark_dirty(false);
    /* Re-sync segment lock state, selection, and empty-state in one pass. */
    nodes_apply_enable_change();
}

/* ── Hostname defocus callback ──────────────────────────────────────── */
static void hostname_defocus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const char *text = lv_textarea_get_text(nodes[idx].ta_hostname);

    char url[128];
    snprintf(url, sizeof(url), "http://%s/api", text);
    snprintf(app_config_get()->api_url[idx], sizeof(app_config_get()->api_url[0]), "%s", url);

    settings_mark_dirty(false);
    settings_hide_keyboard();
}

/* ── Hostname focus callback — show keyboard ────────────────────────── */
static void hostname_focus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    settings_show_keyboard(nodes[idx].ta_hostname);
}

/* ── Filter swatch tap callback ─────────────────────────────────────── */
static void filter_color_selected_cb(uint32_t color, void *user_data);

static void filter_swatch_cb(lv_event_t *e)
{
    intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    int node_idx   = (int)(packed / NODE_MAX_FILTERS);
    int filt_idx   = (int)(packed % NODE_MAX_FILTERS);

    uint32_t current_hex = nodes[node_idx].filter_colors_hex[filt_idx];
    color_picker_show(current_hex, filter_color_selected_cb, (void *)packed);
}

static void filter_color_selected_cb(uint32_t color, void *user_data)
{
    intptr_t packed = (intptr_t)user_data;
    int node_idx   = (int)(packed / NODE_MAX_FILTERS);
    int filt_idx   = (int)(packed % NODE_MAX_FILTERS);

    if (node_idx < 0 || node_idx >= MAX_NINA_INSTANCES) return;
    if (filt_idx < 0 || filt_idx >= nodes[node_idx].filter_count) return;

    nodes[node_idx].filter_colors_hex[filt_idx] = color;

    /* Update swatch visual */
    if (nodes[node_idx].filter_swatches[filt_idx]) {
        lv_obj_set_style_bg_color(nodes[node_idx].filter_swatches[filt_idx],
                                  lv_color_hex(color), 0);
    }

    /* Rebuild JSON and mark dirty */
    rebuild_filter_json(node_idx);
    settings_mark_dirty(false);
}

/* ── Threshold color swatch callbacks ───────────────────────────────── */

/** Packed user_data encoding for threshold swatches:
 *  bits [7:0]  = node index
 *  bits [15:8] = threshold kind: 0=rms_good, 1=rms_warn, 2=rms_bad,
 *                                 3=hfr_good, 4=hfr_warn, 5=hfr_bad
 */
#define THRESH_PACK(node, kind)  ((void *)(intptr_t)(((kind) << 8) | (node)))
#define THRESH_NODE(packed)      ((int)((intptr_t)(packed) & 0xFF))
#define THRESH_KIND(packed)      ((int)(((intptr_t)(packed) >> 8) & 0xFF))

static void thresh_color_selected_cb(uint32_t color, void *user_data);

static void thresh_swatch_cb(lv_event_t *e)
{
    void *packed = lv_event_get_user_data(e);
    int node = THRESH_NODE(packed);
    int kind = THRESH_KIND(packed);

    uint32_t current = 0;
    switch (kind) {
        case 0: current = nodes[node].rms_good_color; break;
        case 1: current = nodes[node].rms_warn_color; break;
        case 2: current = nodes[node].rms_bad_color;  break;
        case 3: current = nodes[node].hfr_good_color; break;
        case 4: current = nodes[node].hfr_warn_color; break;
        case 5: current = nodes[node].hfr_bad_color;  break;
    }
    color_picker_show(current, thresh_color_selected_cb, packed);
}

static void thresh_color_selected_cb(uint32_t color, void *user_data)
{
    int node = THRESH_NODE(user_data);
    int kind = THRESH_KIND(user_data);
    if (node < 0 || node >= MAX_NINA_INSTANCES) return;

    lv_obj_t *swatch = NULL;

    switch (kind) {
        case 0: nodes[node].rms_good_color = color; swatch = nodes[node].swatch_rms_good; break;
        case 1: nodes[node].rms_warn_color = color; swatch = nodes[node].swatch_rms_warn; break;
        case 2: nodes[node].rms_bad_color  = color; swatch = nodes[node].swatch_rms_bad;  break;
        case 3: nodes[node].hfr_good_color = color; swatch = nodes[node].swatch_hfr_good; break;
        case 4: nodes[node].hfr_warn_color = color; swatch = nodes[node].swatch_hfr_warn; break;
        case 5: nodes[node].hfr_bad_color  = color; swatch = nodes[node].swatch_hfr_bad;  break;
    }

    if (swatch) {
        lv_obj_set_style_bg_color(swatch, lv_color_hex(color), 0);
    }

    /* Rebuild appropriate JSON */
    if (kind <= 2) {
        rebuild_rms_json(node);
    } else {
        rebuild_hfr_json(node);
    }
    settings_mark_dirty(false);
}

/* ── RMS stepper callbacks ──────────────────────────────────────────── */
static void rms_good_minus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].rms_good_max -= RMS_STEP;
    if (nodes[idx].rms_good_max < RMS_GOOD_MIN) nodes[idx].rms_good_max = RMS_GOOD_MIN;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].rms_good_max);
    lv_label_set_text(nodes[idx].lbl_rms_good_val, buf);
    rebuild_rms_json(idx);
    settings_mark_dirty(false);
}

static void rms_good_plus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].rms_good_max += RMS_STEP;
    if (nodes[idx].rms_good_max > RMS_GOOD_MAX) nodes[idx].rms_good_max = RMS_GOOD_MAX;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].rms_good_max);
    lv_label_set_text(nodes[idx].lbl_rms_good_val, buf);
    rebuild_rms_json(idx);
    settings_mark_dirty(false);
}

static void rms_warn_minus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].rms_warn_max -= RMS_STEP;
    if (nodes[idx].rms_warn_max < RMS_WARN_MIN) nodes[idx].rms_warn_max = RMS_WARN_MIN;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].rms_warn_max);
    lv_label_set_text(nodes[idx].lbl_rms_warn_val, buf);
    rebuild_rms_json(idx);
    settings_mark_dirty(false);
}

static void rms_warn_plus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].rms_warn_max += RMS_STEP;
    if (nodes[idx].rms_warn_max > RMS_WARN_MAX) nodes[idx].rms_warn_max = RMS_WARN_MAX;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].rms_warn_max);
    lv_label_set_text(nodes[idx].lbl_rms_warn_val, buf);
    rebuild_rms_json(idx);
    settings_mark_dirty(false);
}

/* ── HFR stepper callbacks ──────────────────────────────────────────── */
static void hfr_good_minus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].hfr_good_max -= HFR_STEP;
    if (nodes[idx].hfr_good_max < HFR_GOOD_MIN) nodes[idx].hfr_good_max = HFR_GOOD_MIN;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].hfr_good_max);
    lv_label_set_text(nodes[idx].lbl_hfr_good_val, buf);
    rebuild_hfr_json(idx);
    settings_mark_dirty(false);
}

static void hfr_good_plus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].hfr_good_max += HFR_STEP;
    if (nodes[idx].hfr_good_max > HFR_GOOD_MAX) nodes[idx].hfr_good_max = HFR_GOOD_MAX;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].hfr_good_max);
    lv_label_set_text(nodes[idx].lbl_hfr_good_val, buf);
    rebuild_hfr_json(idx);
    settings_mark_dirty(false);
}

static void hfr_warn_minus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].hfr_warn_max -= HFR_STEP;
    if (nodes[idx].hfr_warn_max < HFR_WARN_MIN) nodes[idx].hfr_warn_max = HFR_WARN_MIN;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].hfr_warn_max);
    lv_label_set_text(nodes[idx].lbl_hfr_warn_val, buf);
    rebuild_hfr_json(idx);
    settings_mark_dirty(false);
}

static void hfr_warn_plus_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodes[idx].hfr_warn_max += HFR_STEP;
    if (nodes[idx].hfr_warn_max > HFR_WARN_MAX) nodes[idx].hfr_warn_max = HFR_WARN_MAX;
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", nodes[idx].hfr_warn_max);
    lv_label_set_text(nodes[idx].lbl_hfr_warn_val, buf);
    rebuild_hfr_json(idx);
    settings_mark_dirty(false);
}

/* ── JSON rebuild helpers ───────────────────────────────────────────── */
static void rebuild_filter_json(int node_idx)
{
    if (nodes[node_idx].filter_count == 0) {
        app_config_get()->filter_colors[node_idx][0] = '\0';
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    for (int i = 0; i < nodes[node_idx].filter_count; i++) {
        char hex_str[8];
        snprintf(hex_str, sizeof(hex_str), "#%06x",
                 (unsigned)nodes[node_idx].filter_colors_hex[i]);
        cJSON_AddStringToObject(root, nodes[node_idx].filter_names[i], hex_str);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(app_config_get()->filter_colors[node_idx], json_str,
                sizeof(app_config_get()->filter_colors[0]) - 1);
        app_config_get()->filter_colors[node_idx][sizeof(app_config_get()->filter_colors[0]) - 1] = '\0';
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
}

static void rebuild_rms_json(int node_idx)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"good_max\":%.1f,\"ok_max\":%.1f,"
        "\"good_color\":\"#%06x\",\"ok_color\":\"#%06x\",\"bad_color\":\"#%06x\"}",
        nodes[node_idx].rms_good_max, nodes[node_idx].rms_warn_max,
        (unsigned)nodes[node_idx].rms_good_color,
        (unsigned)nodes[node_idx].rms_warn_color,
        (unsigned)nodes[node_idx].rms_bad_color);
    snprintf(app_config_get()->rms_thresholds[node_idx],
             sizeof(app_config_get()->rms_thresholds[0]), "%s", buf);
}

static void rebuild_hfr_json(int node_idx)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"good_max\":%.1f,\"ok_max\":%.1f,"
        "\"good_color\":\"#%06x\",\"ok_color\":\"#%06x\",\"bad_color\":\"#%06x\"}",
        nodes[node_idx].hfr_good_max, nodes[node_idx].hfr_warn_max,
        (unsigned)nodes[node_idx].hfr_good_color,
        (unsigned)nodes[node_idx].hfr_warn_color,
        (unsigned)nodes[node_idx].hfr_bad_color);
    snprintf(app_config_get()->hfr_thresholds[node_idx],
             sizeof(app_config_get()->hfr_thresholds[0]), "%s", buf);
}

/* ── JSON parsers ───────────────────────────────────────────────────── */
static void parse_filter_colors(int idx)
{
    const char *json = app_config_get()->filter_colors[idx];
    if (!json || json[0] == '\0') {
        nodes[idx].filter_count = 0;
        return;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        nodes[idx].filter_count = 0;
        return;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (count >= NODE_MAX_FILTERS) break;
        strncpy(nodes[idx].filter_names[count], item->string, 15);
        nodes[idx].filter_names[count][15] = '\0';
        const char *hex_str = item->valuestring;
        if (hex_str && hex_str[0] == '#') {
            nodes[idx].filter_colors_hex[count] = (uint32_t)strtol(hex_str + 1, NULL, 16);
        } else {
            nodes[idx].filter_colors_hex[count] = 0x787878;
        }
        count++;
    }
    nodes[idx].filter_count = count;
    cJSON_Delete(root);
}

static uint32_t parse_hex_color(const char *str, uint32_t fallback)
{
    if (!str || str[0] != '#') return fallback;
    return (uint32_t)strtol(str + 1, NULL, 16);
}

static void parse_rms_thresholds(int idx)
{
    /* Defaults */
    nodes[idx].rms_good_max  = 0.5f;
    nodes[idx].rms_warn_max  = 1.0f;
    nodes[idx].rms_good_color = 0x15803d;
    nodes[idx].rms_warn_color = 0xca8a04;
    nodes[idx].rms_bad_color  = 0xb91c1c;

    const char *json = app_config_get()->rms_thresholds[idx];
    if (!json || json[0] == '\0') return;

    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *val;
    val = cJSON_GetObjectItem(root, "good_max");
    if (val && cJSON_IsNumber(val)) nodes[idx].rms_good_max = (float)val->valuedouble;
    val = cJSON_GetObjectItem(root, "ok_max");
    if (val && cJSON_IsNumber(val)) nodes[idx].rms_warn_max = (float)val->valuedouble;
    val = cJSON_GetObjectItem(root, "good_color");
    if (val && cJSON_IsString(val)) nodes[idx].rms_good_color = parse_hex_color(val->valuestring, 0x15803d);
    val = cJSON_GetObjectItem(root, "ok_color");
    if (val && cJSON_IsString(val)) nodes[idx].rms_warn_color = parse_hex_color(val->valuestring, 0xca8a04);
    val = cJSON_GetObjectItem(root, "bad_color");
    if (val && cJSON_IsString(val)) nodes[idx].rms_bad_color = parse_hex_color(val->valuestring, 0xb91c1c);

    cJSON_Delete(root);
}

static void parse_hfr_thresholds(int idx)
{
    /* Defaults */
    nodes[idx].hfr_good_max  = 2.0f;
    nodes[idx].hfr_warn_max  = 4.0f;
    nodes[idx].hfr_good_color = 0x15803d;
    nodes[idx].hfr_warn_color = 0xca8a04;
    nodes[idx].hfr_bad_color  = 0xb91c1c;

    const char *json = app_config_get()->hfr_thresholds[idx];
    if (!json || json[0] == '\0') return;

    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *val;
    val = cJSON_GetObjectItem(root, "good_max");
    if (val && cJSON_IsNumber(val)) nodes[idx].hfr_good_max = (float)val->valuedouble;
    val = cJSON_GetObjectItem(root, "ok_max");
    if (val && cJSON_IsNumber(val)) nodes[idx].hfr_warn_max = (float)val->valuedouble;
    val = cJSON_GetObjectItem(root, "good_color");
    if (val && cJSON_IsString(val)) nodes[idx].hfr_good_color = parse_hex_color(val->valuestring, 0x15803d);
    val = cJSON_GetObjectItem(root, "ok_color");
    if (val && cJSON_IsString(val)) nodes[idx].hfr_warn_color = parse_hex_color(val->valuestring, 0xca8a04);
    val = cJSON_GetObjectItem(root, "bad_color");
    if (val && cJSON_IsString(val)) nodes[idx].hfr_bad_color = parse_hex_color(val->valuestring, 0xb91c1c);

    cJSON_Delete(root);
}

/* ── Color swatch factory ───────────────────────────────────────────── */
static lv_obj_t *make_color_swatch(lv_obj_t *parent, int size, uint32_t color,
                                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *sw = lv_obj_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, size, size);
    lv_obj_set_style_bg_color(sw, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sw, (size == SWATCH_SIZE) ? SWATCH_RADIUS : THRESH_SWATCH_RAD, 0);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_CLICKED, user_data);
    }
    return sw;
}

/* ── Filter swatches creation ───────────────────────────────────────── */
static void create_filter_swatches(int idx)
{
    if (!nodes[idx].filter_row) return;

    /* Remove old swatches */
    lv_obj_clean(nodes[idx].filter_row);
    memset(nodes[idx].filter_swatches, 0, sizeof(nodes[idx].filter_swatches));

    if (nodes[idx].filter_count == 0) {
        /* Show placeholder label */
        lv_obj_t *lbl = lv_label_create(nodes[idx].filter_row);
        lv_label_set_text(lbl, "No filters configured");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }
        return;
    }

    for (int f = 0; f < nodes[idx].filter_count; f++) {
        /* Container for swatch + label */
        lv_obj_t *col = lv_obj_create(nodes[idx].filter_row);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, SWATCH_SIZE, SWATCH_SIZE + 18);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 2, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        intptr_t packed = (intptr_t)(idx * NODE_MAX_FILTERS + f);
        lv_obj_t *sw = make_color_swatch(col, SWATCH_SIZE,
                                          nodes[idx].filter_colors_hex[f],
                                          filter_swatch_cb, (void *)packed);
        nodes[idx].filter_swatches[f] = sw;

        /* Filter name label below swatch (interactive-swatch label >=18px, SETRD-07) */
        lv_obj_t *lbl = lv_label_create(col);
        lv_label_set_text(lbl, nodes[idx].filter_names[f]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }
    }
}

/* ── Threshold section builder ──────────────────────────────────────── */
static void create_threshold_section(lv_obj_t *parent, int idx, bool is_hfr)
{
    int gb = app_config_get()->color_brightness;
    const char *title = is_hfr ? "HFR Thresholds" : "RMS Thresholds";

    /* Section title */
    lv_obj_t *lbl_title = lv_label_create(parent);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_title,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_set_style_pad_top(lbl_title, 8, 0);

    /* === Good row === */
    lv_obj_t *row_good = settings_make_row_lg(parent);
    {
        lv_obj_t *lbl = lv_label_create(row_good);
        lv_label_set_text(lbl, "Good <");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }

        /* Stepper */
        lv_obj_t *btn_minus, *lbl_val, *btn_plus;
        settings_make_stepper(row_good, &btn_minus, &lbl_val, &btn_plus);

        char buf[8];
        float val = is_hfr ? nodes[idx].hfr_good_max : nodes[idx].rms_good_max;
        snprintf(buf, sizeof(buf), "%.1f", val);
        lv_label_set_text(lbl_val, buf);

        if (is_hfr) {
            nodes[idx].lbl_hfr_good_val = lbl_val;
            lv_obj_add_event_cb(btn_minus, hfr_good_minus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
            lv_obj_add_event_cb(btn_plus, hfr_good_plus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
        } else {
            nodes[idx].lbl_rms_good_val = lbl_val;
            lv_obj_add_event_cb(btn_minus, rms_good_minus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
            lv_obj_add_event_cb(btn_plus, rms_good_plus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
        }

        /* Color swatch */
        uint32_t c = is_hfr ? nodes[idx].hfr_good_color : nodes[idx].rms_good_color;
        int kind = is_hfr ? 3 : 0;
        lv_obj_t *sw = make_color_swatch(row_good, THRESH_SWATCH_SIZE, c,
                                          thresh_swatch_cb, THRESH_PACK(idx, kind));
        if (is_hfr) nodes[idx].swatch_hfr_good = sw;
        else        nodes[idx].swatch_rms_good = sw;
    }

    /* === Warn row === */
    lv_obj_t *row_warn = settings_make_row_lg(parent);
    {
        lv_obj_t *lbl = lv_label_create(row_warn);
        lv_label_set_text(lbl, "Warn <");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }

        /* Stepper */
        lv_obj_t *btn_minus, *lbl_val, *btn_plus;
        settings_make_stepper(row_warn, &btn_minus, &lbl_val, &btn_plus);

        char buf[8];
        float val = is_hfr ? nodes[idx].hfr_warn_max : nodes[idx].rms_warn_max;
        snprintf(buf, sizeof(buf), "%.1f", val);
        lv_label_set_text(lbl_val, buf);

        if (is_hfr) {
            nodes[idx].lbl_hfr_warn_val = lbl_val;
            lv_obj_add_event_cb(btn_minus, hfr_warn_minus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
            lv_obj_add_event_cb(btn_plus, hfr_warn_plus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
        } else {
            nodes[idx].lbl_rms_warn_val = lbl_val;
            lv_obj_add_event_cb(btn_minus, rms_warn_minus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
            lv_obj_add_event_cb(btn_plus, rms_warn_plus_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
        }

        /* Color swatch */
        uint32_t c = is_hfr ? nodes[idx].hfr_warn_color : nodes[idx].rms_warn_color;
        int kind = is_hfr ? 4 : 1;
        lv_obj_t *sw = make_color_swatch(row_warn, THRESH_SWATCH_SIZE, c,
                                          thresh_swatch_cb, THRESH_PACK(idx, kind));
        if (is_hfr) nodes[idx].swatch_hfr_warn = sw;
        else        nodes[idx].swatch_rms_warn = sw;
    }

    /* === Bad row (color only, no stepper) === */
    lv_obj_t *row_bad = settings_make_row_lg(parent);
    {
        lv_obj_t *lbl = lv_label_create(row_bad);
        char bad_text[32];
        float warn_val = is_hfr ? nodes[idx].hfr_warn_max : nodes[idx].rms_warn_max;
        snprintf(bad_text, sizeof(bad_text), "Bad >= %.1f", warn_val);
        lv_label_set_text(lbl, bad_text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }

        /* Color swatch */
        uint32_t c = is_hfr ? nodes[idx].hfr_bad_color : nodes[idx].rms_bad_color;
        int kind = is_hfr ? 5 : 2;
        lv_obj_t *sw = make_color_swatch(row_bad, THRESH_SWATCH_SIZE, c,
                                          thresh_swatch_cb, THRESH_PACK(idx, kind));
        if (is_hfr) nodes[idx].swatch_hfr_bad = sw;
        else        nodes[idx].swatch_rms_bad = sw;
    }
}

/* ── Build one node detail panel (body content only, no picker header) ──
 * `parent` is the pre-built detail container (node_detail[idx]); this fills
 * it with hostname / filters / RMS / HFR. Per-node widget refs (nodes[idx].*)
 * are populated so settings_tab_nodes_refresh keeps working unchanged. */
static void build_node_detail(lv_obj_t *parent, int idx)
{
    app_config_t *cfg = app_config_get();
    int gb = cfg->color_brightness;

    /* Parse stored JSON into working data */
    parse_filter_colors(idx);
    parse_rms_thresholds(idx);
    parse_hfr_thresholds(idx);

    /* Extract hostname from URL for the textarea */
    char hostname[64] = "";
    extract_host_from_url(cfg->api_url[idx], hostname, sizeof(hostname));

    /* ── Hostname input ─────────────────────────────────────────────── */
    lv_obj_t *ta_host = NULL;
    settings_make_textarea_row(parent, "Hostname:port", "e.g. 192.168.1.100:1888",
                                false, &ta_host);
    if (ta_host) {
        lv_textarea_set_text(ta_host, hostname);
        lv_textarea_set_one_line(ta_host, true);
        lv_obj_add_event_cb(ta_host, hostname_defocus_cb, LV_EVENT_DEFOCUSED,
                             (void *)(intptr_t)idx);
        lv_obj_add_event_cb(ta_host, hostname_focus_cb, LV_EVENT_FOCUSED,
                             (void *)(intptr_t)idx);
    }
    nodes[idx].ta_hostname = ta_host;

    settings_make_divider(parent);

    /* ── Filter colors section ──────────────────────────────────────── */
    lv_obj_t *lbl_filt = lv_label_create(parent);
    lv_label_set_text(lbl_filt, "Filter Colors");
    lv_obj_set_style_text_font(lbl_filt, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_filt,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* Scrollable row for filter swatches */
    lv_obj_t *filt_row = lv_obj_create(parent);
    lv_obj_remove_style_all(filt_row);
    lv_obj_set_width(filt_row, LV_PCT(100));
    lv_obj_set_height(filt_row, SWATCH_SIZE + 24);
    lv_obj_set_flex_flow(filt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filt_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(filt_row, SWATCH_GAP, 0);
    lv_obj_set_scroll_dir(filt_row, LV_DIR_HOR);
    lv_obj_clear_flag(filt_row, LV_OBJ_FLAG_SCROLL_ELASTIC);
    nodes[idx].filter_row = filt_row;

    create_filter_swatches(idx);

    settings_make_divider(parent);

    /* ── RMS Thresholds ─────────────────────────────────────────────── */
    create_threshold_section(parent, idx, false);

    settings_make_divider(parent);

    /* ── HFR Thresholds ─────────────────────────────────────────────── */
    create_threshold_section(parent, idx, true);
}

/* ── Segment picker: select callback + lock/enable-change passes ─────── */

/* Swap the visible detail panel to `sel` (hide all others). */
static void node_show_detail(int sel)
{
    for (int k = 0; k < MAX_NINA_INSTANCES; k++) {
        if (!node_detail[k]) continue;
        if (k == sel) {
            lv_obj_clear_flag(node_detail[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(node_detail[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Manual single-select on the buttonmatrix (mirrors seg_mode idiom). */
static void node_seg_select(int sel)
{
    if (!node_seg) return;
    for (int k = 0; k < MAX_NINA_INSTANCES; k++) {
        lv_buttonmatrix_clear_button_ctrl(node_seg, k, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    if (sel >= 0 && sel < MAX_NINA_INSTANCES) {
        lv_buttonmatrix_set_button_ctrl(node_seg, sel, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

static void node_seg_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!node_seg) return;
    uint32_t sel = lv_buttonmatrix_get_selected_button(node_seg);
    if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;
    if ((int)sel >= MAX_NINA_INSTANCES) return;
    /* Belt-and-braces: DISABLED segments emit no VALUE_CHANGED, but guard anyway. */
    if (!app_config_get()->instance_enabled[sel]) return;

    node_seg_select((int)sel);
    node_show_detail((int)sel);
    selected_node = (int)sel;
}

/* Set/clear DISABLED on every segment to match the enable flags. A DISABLED
 * buttonmatrix button renders dimmed (LV_STATE_DISABLED) and is inert. */
static void nodes_sync_segments(void)
{
    if (!node_seg) return;
    app_config_t *cfg = app_config_get();
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (cfg->instance_enabled[i]) {
            lv_buttonmatrix_clear_button_ctrl(node_seg, i, LV_BUTTONMATRIX_CTRL_DISABLED);
        } else {
            lv_buttonmatrix_set_button_ctrl(node_seg, i, LV_BUTTONMATRIX_CTRL_DISABLED);
        }
    }
}

/* One ordered pass covering every enable-change case (RESEARCH Open Risk #3):
 *   (a) sync DISABLED flags on all segments
 *   (b) find first enabled node
 *   (c) if none: clear all CHECKED, hide all panels, show empty-state hint
 *   (d) else: hide hint; if selected is now disabled/invalid, move CHECKED
 *       + visible panel to first enabled; else keep selection but ensure its
 *       CHECKED flag + panel are set. */
static void nodes_apply_enable_change(void)
{
    if (!node_seg) return;
    app_config_t *cfg = app_config_get();

    /* (a) */
    nodes_sync_segments();

    /* (b) */
    int first = -1;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (cfg->instance_enabled[i]) {
            first = i;
            break;
        }
    }

    /* (c) all disabled */
    if (first == -1) {
        node_seg_select(-1);            /* clear CHECKED on all */
        node_show_detail(-1);           /* hide every panel */
        if (empty_hint) {
            lv_obj_clear_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* (d) at least one enabled */
    if (empty_hint) {
        lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
    }

    bool sel_ok = (selected_node >= 0 && selected_node < MAX_NINA_INSTANCES &&
                   cfg->instance_enabled[selected_node]);
    if (!sel_ok) {
        /* Move selection to the first enabled node. */
        selected_node = first;
    }
    node_seg_select(selected_node);
    node_show_detail(selected_node);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

void settings_tab_nodes_destroy(void) {
    tab_root = NULL;
    node_seg = NULL;
    empty_hint = NULL;
    selected_node = 0;
    memset(node_detail, 0, sizeof(node_detail));
    memset(nodes, 0, sizeof(nodes));
}

void settings_tab_nodes_create(lv_obj_t *parent)
{
    tab_root = parent;
    node_seg = NULL;
    empty_hint = NULL;
    selected_node = 0;
    memset(node_detail, 0, sizeof(node_detail));
    memset(nodes, 0, sizeof(nodes));

    app_config_t *cfg = app_config_get();

    /* Make parent scrollable column */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 10, 0);
    lv_obj_set_style_pad_all(parent, 12, 0);

    /* ── Active Instances card ──────────────────────────────────────── */
    lv_obj_t *card_enable = settings_make_card(parent, "Active Instances");

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        char hostname[64] = "";
        extract_host_from_url(cfg->api_url[i], hostname, sizeof(hostname));

        char label[80];
        snprintf(label, sizeof(label), "Node %d: %s", i + 1,
                 hostname[0] ? hostname : "(not set)");

        lv_obj_t *sw = NULL;
        settings_make_toggle_row(card_enable, label, &sw);

        if (sw) {
            if (cfg->instance_enabled[i]) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(sw, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(sw, enable_toggle_cb, LV_EVENT_VALUE_CHANGED,
                                 (void *)(intptr_t)i);
        }
        nodes[i].sw_enable = sw;
    }

    /* ── Node Configuration card: segmented picker + one detail panel ── */
    lv_obj_t *card_nodes = settings_make_card(parent, "Node Configuration");
    /* Fill the remaining viewport so the flex_grow=1 detail panel below has a
     * concrete-height parent to expand into. Without this the card is
     * LV_SIZE_CONTENT and every detail panel collapses to 0px (RESEARCH Q5). */
    lv_obj_set_flex_grow(card_nodes, 1);
    int gb = cfg->color_brightness;

    /* Segmented node picker (buttonmatrix, seg_mode idiom copied verbatim).
     * Manual single-select — do NOT use lv_buttonmatrix_set_one_checked(),
     * it auto-checks button 0 and the clear loop re-triggers it. */
    node_seg = lv_buttonmatrix_create(card_nodes);
    lv_obj_remove_style_all(node_seg);
    lv_buttonmatrix_set_map(node_seg, node_seg_map);
    lv_obj_set_width(node_seg, LV_PCT(100));
    lv_obj_set_height(node_seg, 48);

    if (current_theme) {
        /* Main container (the background strip) */
        lv_obj_set_style_bg_color(node_seg, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_opa(node_seg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(node_seg, 10, 0);
        lv_obj_set_style_border_width(node_seg, 0, 0);
        lv_obj_set_style_outline_width(node_seg, 0, 0);
        lv_obj_set_style_pad_all(node_seg, 4, 0);
        lv_obj_set_style_pad_gap(node_seg, 4, 0);
        lv_obj_set_style_text_font(node_seg, &lv_font_montserrat_20, 0);

        /* Unchecked items — transparent bg, theme text color */
        lv_obj_set_style_bg_opa(node_seg, LV_OPA_TRANSP, LV_PART_ITEMS);
        lv_obj_set_style_border_width(node_seg, 0, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(node_seg, 0, LV_PART_ITEMS);
        lv_obj_set_style_outline_width(node_seg, 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(node_seg, 8, LV_PART_ITEMS);
        lv_obj_set_style_text_color(node_seg,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), LV_PART_ITEMS);
        lv_obj_set_style_text_font(node_seg, &lv_font_montserrat_20, LV_PART_ITEMS);

        /* Checked item — progress_color bg, theme-aware text */
        lv_obj_set_style_bg_color(node_seg, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(node_seg, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(node_seg,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
            LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(node_seg, 0, LV_PART_ITEMS | LV_STATE_CHECKED);

        /* Suppress all other visual states that could show a false highlight */
        lv_obj_set_style_bg_opa(node_seg, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(node_seg, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(node_seg, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(node_seg, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(node_seg, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(node_seg, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_opa(node_seg, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(node_seg, 0, LV_PART_ITEMS | LV_STATE_PRESSED);

        /* Disabled item — dimmed text so a locked (disabled-node) segment is
         * visibly greyed. remove_style_all cleared the theme's default
         * disabled opacity, so set it explicitly (SETRD-02 "dimmed"). */
        lv_obj_set_style_text_opa(node_seg, LV_OPA_40, LV_PART_ITEMS | LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(node_seg, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    }

    lv_obj_add_event_cb(node_seg, node_seg_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Empty-state hint (hidden unless zero nodes enabled) */
    empty_hint = lv_label_create(card_nodes);
    lv_label_set_text(empty_hint, "No nodes enabled - enable a node above");
    lv_obj_set_style_text_font(empty_hint, &lv_font_montserrat_18, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(empty_hint,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
    lv_obj_set_style_pad_top(empty_hint, 8, 0);
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    /* Build all three detail panels once; only the selected one is shown.
     * Each carries flex_grow=1 so the visible panel fills the viewport; a
     * hidden flex child occupies zero space, so grow on hidden panels is
     * inert (SETRD-02 single flex-grow detail panel). */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        lv_obj_t *panel = lv_obj_create(card_nodes);
        lv_obj_remove_style_all(panel);
        lv_obj_set_width(panel, LV_PCT(100));
        lv_obj_set_height(panel, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_grow(panel, 1);
        lv_obj_set_style_pad_left(panel, 8, 0);
        lv_obj_set_style_pad_right(panel, 8, 0);
        lv_obj_set_style_pad_row(panel, 6, 0);
        lv_obj_set_style_pad_top(panel, 8, 0);
        lv_obj_set_style_pad_bottom(panel, 12, 0);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);   /* shown by apply-enable below */
        node_detail[i] = panel;
        build_node_detail(panel, i);
    }

    /* Establish initial segment lock state + selection (first enabled node,
     * or empty state if none) so the tab opens correctly. */
    nodes_apply_enable_change();
}

void settings_tab_nodes_refresh(void)
{
    if (!tab_root) return;

    app_config_t *cfg = app_config_get();

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        /* Refresh enable toggle state */
        if (nodes[i].sw_enable) {
            if (cfg->instance_enabled[i]) {
                lv_obj_add_state(nodes[i].sw_enable, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(nodes[i].sw_enable, LV_STATE_CHECKED);
            }
        }

        /* Re-parse JSON data */
        parse_filter_colors(i);
        parse_rms_thresholds(i);
        parse_hfr_thresholds(i);

        /* Refresh hostname */
        if (nodes[i].ta_hostname) {
            char hostname[64] = "";
            extract_host_from_url(cfg->api_url[i], hostname, sizeof(hostname));
            lv_textarea_set_text(nodes[i].ta_hostname, hostname);
        }

        /* Rebuild filter swatches */
        create_filter_swatches(i);

        /* Refresh RMS threshold widgets */
        if (nodes[i].lbl_rms_good_val) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%.1f", nodes[i].rms_good_max);
            lv_label_set_text(nodes[i].lbl_rms_good_val, buf);
        }
        if (nodes[i].lbl_rms_warn_val) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%.1f", nodes[i].rms_warn_max);
            lv_label_set_text(nodes[i].lbl_rms_warn_val, buf);
        }
        if (nodes[i].swatch_rms_good)
            lv_obj_set_style_bg_color(nodes[i].swatch_rms_good, lv_color_hex(nodes[i].rms_good_color), 0);
        if (nodes[i].swatch_rms_warn)
            lv_obj_set_style_bg_color(nodes[i].swatch_rms_warn, lv_color_hex(nodes[i].rms_warn_color), 0);
        if (nodes[i].swatch_rms_bad)
            lv_obj_set_style_bg_color(nodes[i].swatch_rms_bad, lv_color_hex(nodes[i].rms_bad_color), 0);

        /* Refresh HFR threshold widgets */
        if (nodes[i].lbl_hfr_good_val) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%.1f", nodes[i].hfr_good_max);
            lv_label_set_text(nodes[i].lbl_hfr_good_val, buf);
        }
        if (nodes[i].lbl_hfr_warn_val) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%.1f", nodes[i].hfr_warn_max);
            lv_label_set_text(nodes[i].lbl_hfr_warn_val, buf);
        }
        if (nodes[i].swatch_hfr_good)
            lv_obj_set_style_bg_color(nodes[i].swatch_hfr_good, lv_color_hex(nodes[i].hfr_good_color), 0);
        if (nodes[i].swatch_hfr_warn)
            lv_obj_set_style_bg_color(nodes[i].swatch_hfr_warn, lv_color_hex(nodes[i].hfr_warn_color), 0);
        if (nodes[i].swatch_hfr_bad)
            lv_obj_set_style_bg_color(nodes[i].swatch_hfr_bad, lv_color_hex(nodes[i].hfr_bad_color), 0);
    }

    /* Re-sync segment lock state, selection, and empty-state hint so a config
     * change from elsewhere (e.g. web UI enabling a node) is reflected. */
    nodes_sync_segments();
    nodes_apply_enable_change();
}

void settings_tab_nodes_apply_theme(void)
{
    if (tab_root) {
        settings_apply_theme_recursive(tab_root);
    }
}
