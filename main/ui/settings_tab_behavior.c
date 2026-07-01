/**
 * @file settings_tab_behavior.c
 * @brief Behavior tab for the settings tabview.
 *
 * Contains these cards:
 *   - Power Mgmt:    screen sleep, deep sleep, WiFi power save, auto power off
 *   - Polling:       data rate, graph rate, connection timeout, toast duration
 *   - Notifications: border flash alerts, per-instance mutes
 *   - Idle Page:     idle-override target, indicator, stay-on-page grace
 *
 * Leaf on/off toggles (WiFi Power Save, Auto Power Off, Border Flash, per-node
 * mutes) render two-per-row; reveal toggles (Screen Sleep, Deep Sleep) stay
 * full-width. Screen rotation and backlight now live on the Display tab.
 */

#include "settings_tab_behavior.h"
#include "nina_settings_tabview.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "page_registry.h"
#include "app_config.h"
#include "themes.h"
#include "display_defs.h"
#include "lvgl.h"
#include "esp_wifi.h"

#include <stdio.h>
#include <string.h>

/* ── Tab root ────────────────────────────────────────────────────────── */
static lv_obj_t *tab_root = NULL;

/* ── Power management card ───────────────────────────────────────────── */
static lv_obj_t *sw_screen_sleep      = NULL;
static lv_obj_t *cont_sleep_opts      = NULL;   /* hidden when screen sleep off */
static lv_obj_t *lbl_sleep_timeout    = NULL;
static lv_obj_t *lbl_idle_poll        = NULL;
static lv_obj_t *sw_wifi_power_save   = NULL;
static lv_obj_t *sw_deep_sleep        = NULL;
static lv_obj_t *cont_deep_sleep_opts = NULL;   /* hidden when deep sleep off */
static lv_obj_t *lbl_wake_timer       = NULL;
static lv_obj_t *sw_auto_power_off    = NULL;

/* ── Polling card ────────────────────────────────────────────────────── */
static lv_obj_t *lbl_data_rate        = NULL;
static lv_obj_t *lbl_graph_rate       = NULL;
static lv_obj_t *lbl_conn_timeout     = NULL;
static lv_obj_t *lbl_toast_duration   = NULL;

/* ── Notifications card ──────────────────────────────────────────────── */
static lv_obj_t *sw_alert_flash       = NULL;
static lv_obj_t *sw_instance_mute[3]  = {NULL, NULL, NULL};

/* ── Idle Page card ─────────────────────────────────────────────────── */
static lv_obj_t *sw_idle_override        = NULL;
static lv_obj_t *dd_idle_target          = NULL;
static lv_obj_t *idle_target_container   = NULL;
static lv_obj_t *sw_idle_indicator       = NULL;
static lv_obj_t *lbl_nav_grace           = NULL;

/* Idle target dropdown <-> page_registry mapping.
 * The dropdown lists only targetable PAGE / IMAGE_SOURCE registry entries, so
 * the option index is NOT the page_ref id. idle_target_ids[option_index] holds
 * the page_ref id stored in idle_page_override_target for that option. */
static page_ref_t idle_target_ids[PAGE_REF_ID_MAX];
static int        idle_target_count = 0;

/* ════════════════════════════════════════════════════════════════════════
 *  Helper — format sleep timeout with appropriate unit
 * ════════════════════════════════════════════════════════════════════════ */

static void update_sleep_timeout_label(void) {
    app_config_t *cfg = app_config_get();
    if (cfg->screen_sleep_timeout_s >= 60) {
        lv_label_set_text_fmt(lbl_sleep_timeout, "%d min",
                              cfg->screen_sleep_timeout_s / 60);
    } else {
        lv_label_set_text_fmt(lbl_sleep_timeout, "%d s",
                              cfg->screen_sleep_timeout_s);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Power Management Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void screen_sleep_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bool on = lv_obj_has_state(sw_screen_sleep, LV_STATE_CHECKED);
    app_config_get()->screen_sleep_enabled = on;
    if (on) lv_obj_clear_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    settings_mark_dirty(false);
}

static void sleep_timeout_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->screen_sleep_timeout_s > 120 ? 30 : 10;
    int val = (int)cfg->screen_sleep_timeout_s - step;
    if (val < 10) val = 10;
    cfg->screen_sleep_timeout_s = (uint16_t)val;
    update_sleep_timeout_label();
    settings_mark_dirty(false);
}

static void sleep_timeout_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->screen_sleep_timeout_s >= 120 ? 30 : 10;
    int val = (int)cfg->screen_sleep_timeout_s + step;
    if (val > 3600) val = 3600;
    cfg->screen_sleep_timeout_s = (uint16_t)val;
    update_sleep_timeout_label();
    settings_mark_dirty(false);
}

static void idle_poll_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->idle_poll_interval_s > 5) {
        cfg->idle_poll_interval_s -= 5;
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);
        settings_mark_dirty(false);
    }
}

static void idle_poll_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->idle_poll_interval_s < 120) {
        cfg->idle_poll_interval_s += 5;
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);
        settings_mark_dirty(false);
    }
}

static void wifi_power_save_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bool on = lv_obj_has_state(sw_wifi_power_save, LV_STATE_CHECKED);
    app_config_get()->wifi_power_save = on;
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    settings_mark_dirty(false);
}

static void deep_sleep_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bool on = lv_obj_has_state(sw_deep_sleep, LV_STATE_CHECKED);
    app_config_get()->deep_sleep_enabled = on;
    if (on) lv_obj_clear_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    settings_mark_dirty(false);
}

static void wake_timer_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
    if (hours > 0) {
        hours--;
        cfg->deep_sleep_wake_timer_s = hours * 3600;
        lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
        settings_mark_dirty(false);
    }
}

static void wake_timer_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
    if (hours < 72) {
        hours++;
        cfg->deep_sleep_wake_timer_s = hours * 3600;
        lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
        settings_mark_dirty(false);
    }
}

static void auto_power_off_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_get()->deep_sleep_on_idle =
        lv_obj_has_state(sw_auto_power_off, LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Polling Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void data_rate_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->update_rate_s > 1) {
        cfg->update_rate_s--;
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);
        settings_mark_dirty(false);
    }
}

static void data_rate_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->update_rate_s < 10) {
        cfg->update_rate_s++;
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);
        settings_mark_dirty(false);
    }
}

static void graph_rate_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->graph_update_interval_s > 2) {
        cfg->graph_update_interval_s--;
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);
        settings_mark_dirty(false);
    }
}

static void graph_rate_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->graph_update_interval_s < 30) {
        cfg->graph_update_interval_s++;
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);
        settings_mark_dirty(false);
    }
}

static void conn_timeout_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->connection_timeout_s > 2) {
        cfg->connection_timeout_s--;
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);
        settings_mark_dirty(false);
    }
}

static void conn_timeout_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->connection_timeout_s < 30) {
        cfg->connection_timeout_s++;
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);
        settings_mark_dirty(false);
    }
}

static void toast_duration_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->toast_duration_s > 3) {
        cfg->toast_duration_s--;
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
        settings_mark_dirty(false);
    }
}

static void toast_duration_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->toast_duration_s < 30) {
        cfg->toast_duration_s++;
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
        settings_mark_dirty(false);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Notifications Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void alert_flash_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_get()->alert_flash_enabled =
        lv_obj_has_state(sw_alert_flash, LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

static void instance_mute_toggle_cb(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= 3) return;
    app_config_get()->toast_instance_muted[index] =
        !lv_obj_has_state(sw_instance_mute[index], LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Idle Page Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void idle_override_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    cfg->idle_page_override_enabled = lv_obj_has_state(sw_idle_override, LV_STATE_CHECKED);
    if (cfg->idle_page_override_enabled) {
        cfg->auto_rotate_enabled = false;          /* but normalize makes auto-rotate win */
        lv_obj_clear_flag(idle_target_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(idle_target_container, LV_OBJ_FLAG_HIDDEN);
    }
    app_config_normalize_nav_exclusivity(cfg);
    /* Reflect the normalized result back into the switch in case auto-rotate won.
     * The display tab's seg_mode picks up auto_rotate_enabled on its next refresh. */
    if (cfg->idle_page_override_enabled != lv_obj_has_state(sw_idle_override, LV_STATE_CHECKED)) {
        if (cfg->idle_page_override_enabled) lv_obj_add_state(sw_idle_override, LV_STATE_CHECKED);
        else                                 lv_obj_remove_state(sw_idle_override, LV_STATE_CHECKED);
        /* Keep the target container visibility consistent with the corrected state. */
        if (cfg->idle_page_override_enabled) lv_obj_clear_flag(idle_target_container, LV_OBJ_FLAG_HIDDEN);
        else                                 lv_obj_add_flag(idle_target_container, LV_OBJ_FLAG_HIDDEN);
    }
    settings_mark_dirty(false);
}

/* Build the idle-target dropdown options string from the page registry and
 * populate idle_target_ids[]. Only targetable PAGE / IMAGE_SOURCE entries are
 * listed, in registry order. @p out must hold at least @p out_sz bytes. */
static void idle_target_build_options(char *out, size_t out_sz) {
    size_t pos = 0;
    int n = page_ref_count();
    idle_target_count = 0;
    out[0] = '\0';
    for (int i = 0; i < n && idle_target_count < PAGE_REF_ID_MAX; i++) {
        const page_ref_entry_t *ent = page_ref_get(i);
        if (ent == NULL) continue;
        if (!ent->targetable) continue;
        if (ent->kind != PAGE_REF_KIND_PAGE && ent->kind != PAGE_REF_KIND_IMAGE_SOURCE) continue;
        const char *sep = (idle_target_count > 0) ? "\n" : "";
        int w = snprintf(out + pos, out_sz - pos, "%s%s", sep, ent->label);
        if (w < 0 || (size_t)w >= out_sz - pos) break;
        pos += (size_t)w;
        idle_target_ids[idle_target_count] = ent->id;
        idle_target_count++;
    }
}

/* Return the dropdown option index whose mapped id == @p id, or the option
 * index for Summary (falling back to 0) if @p id is not in the list. */
static uint32_t idle_target_index_for_id(page_ref_t id) {
    int summary_idx = 0;
    for (int i = 0; i < idle_target_count; i++) {
        if (idle_target_ids[i] == id) return (uint32_t)i;
        if (idle_target_ids[i] == PAGE_REF_SUMMARY) summary_idx = i;
    }
    return (uint32_t)summary_idx;
}

static void idle_target_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    uint32_t sel = lv_dropdown_get_selected(dd_idle_target);
    if (sel < (uint32_t)idle_target_count) {
        cfg->idle_page_override_target = (int8_t)idle_target_ids[sel];
    }
    settings_mark_dirty(false);
}

static void idle_indicator_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_get()->idle_indicator_enabled =
        lv_obj_has_state(sw_idle_indicator, LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

static void nav_grace_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int val = (int)cfg->nav_grace_s - 10;
    if (val < 10) val = 10;
    cfg->nav_grace_s = (uint16_t)val;
    lv_label_set_text_fmt(lbl_nav_grace, "%d s", val);
    settings_mark_dirty(false);
}

static void nav_grace_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int val = (int)cfg->nav_grace_s + 10;
    if (val > 300) val = 300;
    cfg->nav_grace_s = (uint16_t)val;
    lv_label_set_text_fmt(lbl_nav_grace, "%d s", val);
    settings_mark_dirty(false);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Helper — create a labeled stepper row inside a card
 * ════════════════════════════════════════════════════════════════════════ */

static void make_labeled_stepper(lv_obj_t *card, const char *text,
                                 lv_event_cb_t minus_cb, lv_event_cb_t plus_cb,
                                 lv_obj_t **out_label)
{
    lv_obj_t *row = settings_make_row(card);

    int gb = app_config_get()->color_brightness;
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    lv_obj_t *btn_m = NULL, *lbl_val = NULL, *btn_p = NULL;
    settings_make_stepper(row, &btn_m, &lbl_val, &btn_p);

    lv_obj_add_event_cb(btn_m, minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_p, plus_cb,  LV_EVENT_CLICKED, NULL);

    if (out_label) *out_label = lbl_val;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Helper — 2-column leaf-toggle grid
 *
 *  Pure on/off leaf toggles are laid two-per-row in a ROW_WRAP grid. Each
 *  cell is lv_pct(48) — deliberately sub-50% so 48%+48%+gap does not overflow
 *  the row and wrap the second cell (RESEARCH Open Risk #4). settings_make_
 *  toggle_row() builds a LV_PCT(100) SPACE_BETWEEN row; setting its width to
 *  the cell percentage makes it a grid cell directly (no extra container),
 *  preserving the label-left / switch-right layout inside each cell.
 * ════════════════════════════════════════════════════════════════════════ */

#define LEAF_CELL_PCT 48   /* sub-50% so two cells + gap fit per row */

/* Create the ROW_WRAP grid container for leaf toggles inside @p card. */
static lv_obj_t *make_toggle_grid(lv_obj_t *card)
{
    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    return grid;
}

/* Add one leaf toggle cell (label + switch) to @p grid, returning the switch
 * via @p out_sw. The cell width is sub-50% so exactly two fit per row. */
static void add_toggle_cell(lv_obj_t *grid, const char *text, lv_obj_t **out_sw)
{
    lv_obj_t *cell = settings_make_toggle_row(grid, text, out_sw);
    lv_obj_set_width(cell, LV_PCT(LEAF_CELL_PCT));
}

/* ════════════════════════════════════════════════════════════════════════
 *  Tab Creation
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_behavior_destroy(void) {
    tab_root = NULL;
    sw_screen_sleep = NULL;
    cont_sleep_opts = NULL;
    lbl_sleep_timeout = NULL;
    lbl_idle_poll = NULL;
    sw_wifi_power_save = NULL;
    sw_deep_sleep = NULL;
    cont_deep_sleep_opts = NULL;
    lbl_wake_timer = NULL;
    sw_auto_power_off = NULL;
    lbl_data_rate = NULL;
    lbl_graph_rate = NULL;
    lbl_conn_timeout = NULL;
    lbl_toast_duration = NULL;
    sw_alert_flash = NULL;
    for (int i = 0; i < 3; i++) sw_instance_mute[i] = NULL;
    sw_idle_override = NULL;
    dd_idle_target = NULL;
    idle_target_container = NULL;
    sw_idle_indicator = NULL;
    lbl_nav_grace = NULL;
}

void settings_tab_behavior_create(lv_obj_t *parent) {
    tab_root = parent;
    app_config_t *cfg = app_config_get();
    int gb = cfg->color_brightness;

    /* ── Power Management Card ───────────────────────────────────────── */
    {
        lv_obj_t *card = settings_make_card(parent, "POWER MANAGEMENT");

        /* Screen sleep toggle — reveal toggle, full-width */
        settings_make_toggle_row(card, "Screen Sleep", &sw_screen_sleep);
        if (cfg->screen_sleep_enabled) {
            lv_obj_add_state(sw_screen_sleep, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_screen_sleep, screen_sleep_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* Collapsible sleep options container */
        cont_sleep_opts = lv_obj_create(card);
        lv_obj_remove_style_all(cont_sleep_opts);
        lv_obj_set_width(cont_sleep_opts, LV_PCT(100));
        lv_obj_set_height(cont_sleep_opts, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cont_sleep_opts, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cont_sleep_opts, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cont_sleep_opts, 4, 0);
        if (!cfg->screen_sleep_enabled) {
            lv_obj_add_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }

        /* Sleep timeout stepper */
        make_labeled_stepper(cont_sleep_opts, "Timeout",
                             sleep_timeout_minus_cb, sleep_timeout_plus_cb,
                             &lbl_sleep_timeout);
        update_sleep_timeout_label();

        settings_make_divider(card);

        /* Deep sleep toggle — reveal toggle, full-width */
        settings_make_toggle_row(card, "Deep Sleep", &sw_deep_sleep);
        if (cfg->deep_sleep_enabled) {
            lv_obj_add_state(sw_deep_sleep, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_deep_sleep, deep_sleep_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* Collapsible deep sleep options container (wake timer only; Auto Power
         * Off is a leaf toggle and lives in the 2-column grid below). */
        cont_deep_sleep_opts = lv_obj_create(card);
        lv_obj_remove_style_all(cont_deep_sleep_opts);
        lv_obj_set_width(cont_deep_sleep_opts, LV_PCT(100));
        lv_obj_set_height(cont_deep_sleep_opts, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cont_deep_sleep_opts, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cont_deep_sleep_opts, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cont_deep_sleep_opts, 4, 0);
        if (!cfg->deep_sleep_enabled) {
            lv_obj_add_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }

        /* Wake timer stepper (hours) */
        {
            uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
            make_labeled_stepper(cont_deep_sleep_opts, "Wake Timer",
                                 wake_timer_minus_cb, wake_timer_plus_cb,
                                 &lbl_wake_timer);
            lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
        }

        settings_make_divider(card);

        /* Leaf toggles two-per-row: WiFi Power Save + Auto Power Off. Auto Power
         * Off is re-parented out of the deep-sleep reveal container into this
         * leaf grid per CONTEXT §Behavior (its callback is unchanged). */
        {
            lv_obj_t *grid = make_toggle_grid(card);

            add_toggle_cell(grid, "WiFi Power Save", &sw_wifi_power_save);
            if (cfg->wifi_power_save) {
                lv_obj_add_state(sw_wifi_power_save, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(sw_wifi_power_save, wifi_power_save_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

            add_toggle_cell(grid, "Auto Power Off", &sw_auto_power_off);
            if (cfg->deep_sleep_on_idle) {
                lv_obj_add_state(sw_auto_power_off, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(sw_auto_power_off, auto_power_off_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* ── Polling Card ────────────────────────────────────────────────── */
    {
        lv_obj_t *card = settings_make_card(parent, "POLLING");

        /* Data rate stepper */
        make_labeled_stepper(card, "Data Rate",
                             data_rate_minus_cb, data_rate_plus_cb,
                             &lbl_data_rate);
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);

        settings_make_divider(card);

        /* Idle poll rate stepper (offline rig re-check; idle_poll_interval_s) */
        make_labeled_stepper(card, "Idle Poll",
                             idle_poll_minus_cb, idle_poll_plus_cb,
                             &lbl_idle_poll);
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);

        settings_make_divider(card);

        /* Graph rate stepper */
        make_labeled_stepper(card, "Graph Rate",
                             graph_rate_minus_cb, graph_rate_plus_cb,
                             &lbl_graph_rate);
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);

        settings_make_divider(card);

        /* Connection timeout stepper */
        make_labeled_stepper(card, "Conn Timeout",
                             conn_timeout_minus_cb, conn_timeout_plus_cb,
                             &lbl_conn_timeout);
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);

        settings_make_divider(card);

        /* Toast duration stepper */
        make_labeled_stepper(card, "Toast Duration",
                             toast_duration_minus_cb, toast_duration_plus_cb,
                             &lbl_toast_duration);
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
    }

    /* ── Notifications Card ──────────────────────────────────────────── */
    {
        lv_obj_t *card = settings_make_card(parent, "NOTIFICATIONS");

        /* Leaf toggles two-per-row: Border Flash + per-instance mutes. */
        lv_obj_t *grid = make_toggle_grid(card);

        add_toggle_cell(grid, "Border Flash", &sw_alert_flash);
        if (cfg->alert_flash_enabled) {
            lv_obj_add_state(sw_alert_flash, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_alert_flash, alert_flash_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* Per-instance mute toggles — only show for enabled instances */
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            if (!cfg->instance_enabled[i]) continue;

            /* Try to extract hostname from URL for label */
            char label[48];
            const char *url = app_config_get_instance_url(i);
            if (url && url[0]) {
                const char *host = strstr(url, "://");
                host = host ? host + 3 : url;
                int len = 0;
                while (host[len] && host[len] != ':' && host[len] != '/' && len < 30) len++;
                snprintf(label, sizeof(label), "%.*s Alerts", len, host);
            } else {
                snprintf(label, sizeof(label), "Instance %d Alerts", i + 1);
            }

            add_toggle_cell(grid, label, &sw_instance_mute[i]);
            if (!cfg->toast_instance_muted[i]) {
                lv_obj_add_state(sw_instance_mute[i], LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(sw_instance_mute[i], instance_mute_toggle_cb,
                                LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        }
    }

    /* ── Idle Page Card ─────────────────────────────────────────────── */
    {
        lv_obj_t *idle_card = settings_make_card(parent, "IDLE PAGE");

        settings_make_toggle_row(idle_card, "Switch page when idle", &sw_idle_override);
        if (cfg->idle_page_override_enabled) {
            lv_obj_add_state(sw_idle_override, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_idle_override, idle_override_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* Target dropdown (in a container we can show/hide) */
        idle_target_container = lv_obj_create(idle_card);
        lv_obj_remove_style_all(idle_target_container);
        lv_obj_set_width(idle_target_container, LV_PCT(100));
        lv_obj_set_height(idle_target_container, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(idle_target_container, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(idle_target_container, 8, 0);
        lv_obj_clear_flag(idle_target_container, LV_OBJ_FLAG_SCROLLABLE);

        /* "Target page" label */
        {
            lv_obj_t *row = settings_make_row(idle_target_container);

            lv_obj_t *target_lbl = lv_label_create(row);
            lv_label_set_text(target_lbl, "Target page");
            lv_obj_set_style_text_font(target_lbl, &lv_font_montserrat_20, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(target_lbl,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            dd_idle_target = lv_dropdown_create(row);
            lv_obj_set_width(dd_idle_target, 200);

            /* Options come from the page registry (single source of truth):
             * targetable PAGE / IMAGE_SOURCE entries in registry order. The
             * option index is mapped to a page_ref id via idle_target_ids[]. */
            char idle_opts[PAGE_REF_ID_MAX * 24];
            idle_target_build_options(idle_opts, sizeof(idle_opts));
            lv_dropdown_set_options(dd_idle_target, idle_opts);

            /* Select the option whose mapped id == the stored target id;
             * fall back to Summary if the stored id is not in the list. */
            lv_dropdown_set_selected(dd_idle_target,
                idle_target_index_for_id((page_ref_t)cfg->idle_page_override_target));

            if (current_theme) {
                lv_obj_set_style_text_color(dd_idle_target,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
                lv_obj_set_style_bg_color(dd_idle_target, lv_color_hex(current_theme->bento_bg), 0);
                lv_obj_set_style_bg_opa(dd_idle_target, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(dd_idle_target, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_border_width(dd_idle_target, 1, 0);
            }

            lv_obj_add_event_cb(dd_idle_target, idle_target_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        /* Show idle indicator toggle */
        settings_make_toggle_row(idle_target_container, "Show idle indicator", &sw_idle_indicator);
        if (cfg->idle_indicator_enabled) {
            lv_obj_add_state(sw_idle_indicator, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_idle_indicator, idle_indicator_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        if (!cfg->idle_page_override_enabled) {
            lv_obj_add_flag(idle_target_container, LV_OBJ_FLAG_HIDDEN);
        }

        settings_make_divider(idle_card);

        /* Manual-nav grace window stepper (10-300s, step 10).
         * After a user navigation, lower-priority sources are held off for this
         * window before the navigation arbiter resolves back to the home/idle page. */
        make_labeled_stepper(idle_card, "Stay On Page",
                             nav_grace_minus_cb, nav_grace_plus_cb,
                             &lbl_nav_grace);
        lv_label_set_text_fmt(lbl_nav_grace, "%d s", cfg->nav_grace_s);

        /* Hint label */
        lv_obj_t *idle_hint = lv_label_create(idle_card);
        lv_label_set_text(idle_hint, "Disabled while auto-rotate is active");
        lv_obj_set_style_text_font(idle_hint, &lv_font_montserrat_14, 0);
        if (theme_is_red_night(current_theme)) {
            lv_obj_set_style_text_color(idle_hint, lv_color_hex(current_theme->label_color), 0);
        } else {
            lv_obj_set_style_text_color(idle_hint, lv_color_hex(0x888888), 0);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Refresh — re-read config and update all widget states
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_behavior_refresh(void) {
    if (!tab_root) return;
    app_config_t *cfg = app_config_get();

    /* Screen sleep */
    if (sw_screen_sleep) {
        if (cfg->screen_sleep_enabled) {
            lv_obj_add_state(sw_screen_sleep, LV_STATE_CHECKED);
            lv_obj_clear_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(sw_screen_sleep, LV_STATE_CHECKED);
            lv_obj_add_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_sleep_timeout) {
        update_sleep_timeout_label();
    }
    if (lbl_idle_poll) {
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);
    }

    /* WiFi power save */
    if (sw_wifi_power_save) {
        if (cfg->wifi_power_save)
            lv_obj_add_state(sw_wifi_power_save, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_wifi_power_save, LV_STATE_CHECKED);
    }

    /* Deep sleep */
    if (sw_deep_sleep) {
        if (cfg->deep_sleep_enabled) {
            lv_obj_add_state(sw_deep_sleep, LV_STATE_CHECKED);
            lv_obj_clear_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(sw_deep_sleep, LV_STATE_CHECKED);
            lv_obj_add_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_wake_timer) {
        uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
        lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
    }
    if (sw_auto_power_off) {
        if (cfg->deep_sleep_on_idle)
            lv_obj_add_state(sw_auto_power_off, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_auto_power_off, LV_STATE_CHECKED);
    }

    /* Polling */
    if (lbl_data_rate) {
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);
    }
    if (lbl_graph_rate) {
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);
    }
    if (lbl_conn_timeout) {
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);
    }
    if (lbl_toast_duration) {
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
    }

    /* Notifications */
    if (sw_alert_flash) {
        if (cfg->alert_flash_enabled)
            lv_obj_add_state(sw_alert_flash, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_alert_flash, LV_STATE_CHECKED);
    }
    for (int i = 0; i < 3; i++) {
        if (sw_instance_mute[i]) {
            if (!cfg->toast_instance_muted[i])
                lv_obj_add_state(sw_instance_mute[i], LV_STATE_CHECKED);
            else
                lv_obj_remove_state(sw_instance_mute[i], LV_STATE_CHECKED);
        }
    }

    /* Idle page override */
    if (sw_idle_override) {
        if (cfg->idle_page_override_enabled) {
            lv_obj_add_state(sw_idle_override, LV_STATE_CHECKED);
            if (idle_target_container) lv_obj_clear_flag(idle_target_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(sw_idle_override, LV_STATE_CHECKED);
            if (idle_target_container) lv_obj_add_flag(idle_target_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (dd_idle_target) {
        lv_dropdown_set_selected(dd_idle_target,
            idle_target_index_for_id((page_ref_t)cfg->idle_page_override_target));
    }
    if (sw_idle_indicator) {
        if (cfg->idle_indicator_enabled)
            lv_obj_add_state(sw_idle_indicator, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_idle_indicator, LV_STATE_CHECKED);
    }
    if (lbl_nav_grace) {
        lv_label_set_text_fmt(lbl_nav_grace, "%d s", cfg->nav_grace_s);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Apply Theme — walk all children recursively
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_behavior_apply_theme(void) {
    if (tab_root) settings_apply_theme_recursive(tab_root);
}
