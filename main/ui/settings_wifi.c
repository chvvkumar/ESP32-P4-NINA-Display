/**
 * @file settings_wifi.c
 * @brief Panel Mode WiFi screens — home / scan / password / connect result.
 *
 * UI only: all radio work lives in wifi_join.c (worker task) and main.c's
 * wifi_manager helpers. This file makes ZERO esp_wifi / esp_netif calls.
 *
 * Every builder runs on the LVGL task with the display lock already held by
 * the caller (settings_hub_goto) — nothing here takes the lock. Worker
 * notifications arrive on the LVGL task via wifi_join's lv_async_call
 * trampoline and re-read state through wifi_join getters.
 *
 * Screen-scoped lv_timers and widget pointers are cleaned by an
 * LV_EVENT_DELETE callback on each screen root: the hub deletes our screen
 * object on every navigation, and the delete event is our teardown hook.
 */

#include "settings_hub.h"
#include "settings_wifi_internal.h"
#include "wifi_join.h"
#include "wifi_manager.h"
#include "app_config.h"
#include "ui_helpers.h"          /* current_theme, UI_THEME_COLOR, ui_label */
#include "nina_toast.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Layout (688 px settings root, matches settings_hub.c geometry) ──── */
#define WIFI_ROW_H        96
#define WIFI_FORGET_W    120
#define WIFI_FORGET_H     72
#define WIFI_RESCAN_W    140
#define WIFI_RESCAN_H     72
#define WIFI_EYE_SZ       72
#define WIFI_KB_H        380
#define WIFI_SCAN_MAX     20   /* backend dedupes and caps at 20 strongest */
#define WIFI_SLOT_COUNT    3   /* cfg->wifi_networks[3] */
#define WIFI_OK_COLOR    0x2FA84F   /* success green (no theme field for it) */
#define WIFI_DANGER_COLOR 0xB02A2A  /* matches settings_hub HUB_DANGER_COLOR */

/* ── Static state ────────────────────────────────────────────────────── */
/* Candidate network handed from scan -> password -> connect. 33 bytes: a
 * scanned SSID can be a full 32 chars. Empty ssid = hidden-network variant. */
static char s_cand_ssid[33];
static bool s_cand_secured = false;

/* Scan result snapshot (copied under the backend mutex). File-scope so row
 * callbacks can read it after any rebuild. */
static wifi_join_ap_t s_scan[WIFI_SCAN_MAX];
static int            s_scan_count = 0;

/* WiFi-home FORGET two-tap confirm */
static int         s_forget_armed = -1;    /* slot index, -1 = disarmed */
static lv_obj_t   *s_forget_lbl   = NULL;  /* label of the armed FORGET button */
static lv_timer_t *s_forget_timer = NULL;  /* 5 s disarm */

/* Connect-result auto-return */
static lv_timer_t *s_result_timer = NULL;  /* 2 s SUCCESS -> WiFi home */

/* Password screen. wifi_connect_obj and wifi_kb_obj are published to the round
 * fit pass through settings_wifi_internal.h (and so lose the s_ prefix, which
 * this codebase reserves for statics); the others stay file-local. */
static lv_obj_t *s_pw_ta   = NULL;
static lv_obj_t *s_ssid_ta = NULL;     /* hidden-network variant only */
static lv_obj_t *s_eye_lbl = NULL;
lv_obj_t *wifi_connect_obj = NULL;
lv_obj_t *wifi_kb_obj      = NULL;

/* Per-screen handles the round fit pass re-places. Cleared at the top of
 * settings_wifi_build() and again on screen delete; a screen that has no such
 * object leaves it NULL. The header row is not among them: every WiFi screen
 * that has one gets it from settings_hub_make_header(), which publishes
 * hub_header_obj itself. */
lv_obj_t *wifi_list_obj   = NULL;
lv_obj_t *wifi_prow_obj   = NULL;
lv_obj_t *wifi_srow_obj   = NULL;
lv_obj_t *wifi_rescan_obj = NULL;
bool      wifi_hidden_variant = false;

/* ── Forward declarations ────────────────────────────────────────────── */
static void build_wifi_home(lv_obj_t *root);
static void build_wifi_scan(lv_obj_t *root);
static void build_wifi_password(lv_obj_t *root);
static void build_wifi_connect(lv_obj_t *root);
static lv_obj_t *wifi_make_btn(lv_obj_t *parent, const char *text, int w, int h,
                               lv_event_cb_t cb, void *user_data);
static lv_obj_t *wifi_make_spinner(lv_obj_t *parent);
static void wifi_ui_notify(void);
static void wifi_screen_delete_cb(lv_event_t *e);
static void wifi_saved_row_cb(lv_event_t *e);
static void wifi_forget_cb(lv_event_t *e);
static void wifi_forget_disarm_cb(lv_timer_t *t);
static void wifi_add_cb(lv_event_t *e);
static void wifi_rescan_cb(lv_event_t *e);
static void wifi_scan_row_cb(lv_event_t *e);
static void wifi_hidden_row_cb(lv_event_t *e);
static void wifi_ta_event_cb(lv_event_t *e);
static void wifi_eye_cb(lv_event_t *e);
static void wifi_connect_btn_cb(lv_event_t *e);
static void wifi_kb_ready_cb(lv_event_t *e);
static void wifi_do_connect(void);
static void wifi_connect_gate_update(void);
static void wifi_cancel_cb(lv_event_t *e);
static void wifi_try_again_cb(lv_event_t *e);
static void wifi_result_back_cb(lv_event_t *e);
static void wifi_result_timer_cb(lv_timer_t *t);

/* ════════════════════════════════════════════════════════════════════════
 *  Seam entry points (contract in settings_hub.h)
 * ════════════════════════════════════════════════════════════════════════ */

void settings_wifi_set_candidate(const char *ssid, bool secured)
{
    if (ssid) {
        strlcpy(s_cand_ssid, ssid, sizeof(s_cand_ssid));
    } else {
        s_cand_ssid[0] = '\0';
    }
    s_cand_secured = secured;
}

void settings_wifi_build(lv_obj_t *root, hub_screen_t which)
{
    /* Register on every WiFi-screen entry (idempotent); the hub deregisters
     * when the WiFi family is left or settings are destroyed. */
    wifi_join_set_notify_cb(wifi_ui_notify);

    /* Teardown hook: the hub deletes this root on every navigation. */
    lv_obj_add_event_cb(root, wifi_screen_delete_cb, LV_EVENT_DELETE, NULL);

    /* Round fit-pass handles: stale pointers here would re-place freed widgets. */
    wifi_list_obj   = NULL;
    wifi_prow_obj   = NULL;
    wifi_srow_obj   = NULL;
    wifi_rescan_obj = NULL;

    switch (which) {
    case HUB_SCREEN_WIFI_SCAN:
        build_wifi_scan(root);
        break;
    case HUB_SCREEN_WIFI_PASSWORD:
        build_wifi_password(root);
        break;
    case HUB_SCREEN_WIFI_CONNECT:
        build_wifi_connect(root);
        break;
    case HUB_SCREEN_WIFI_HOME:
    default:
        build_wifi_home(root);
        break;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Shared plumbing
 * ════════════════════════════════════════════════════════════════════════ */

/* Runs on the LVGL task (delivered through wifi_join's lv_async_call
 * trampoline). Re-render the state-driven screens; home and password screens
 * ignore join transitions (typing must never be clobbered). */
static void wifi_ui_notify(void)
{
    hub_screen_t cur = settings_hub_current();
    if (cur == HUB_SCREEN_WIFI_SCAN || cur == HUB_SCREEN_WIFI_CONNECT) {
        settings_hub_goto(cur);   /* rebuild reads wifi_join_get_state() */
    }
}

/* LV_EVENT_DELETE on a screen root: kill screen-scoped timers and NULL every
 * per-screen widget pointer so nothing fires into freed widgets. */
static void wifi_screen_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_forget_timer) {
        lv_timer_delete(s_forget_timer);
        s_forget_timer = NULL;
    }
    if (s_result_timer) {
        lv_timer_delete(s_result_timer);
        s_result_timer = NULL;
    }
    s_forget_armed = -1;
    s_forget_lbl   = NULL;
    s_pw_ta        = NULL;
    s_ssid_ta      = NULL;
    wifi_connect_obj = NULL;
    s_eye_lbl      = NULL;
    wifi_kb_obj    = NULL;

    wifi_list_obj   = NULL;
    wifi_prow_obj   = NULL;
    wifi_srow_obj   = NULL;
    wifi_rescan_obj = NULL;
}

/* Solid button in the hub's style: bento_border fill, progress on press. */
static lv_obj_t *wifi_make_btn(lv_obj_t *parent, const char *text, int w, int h,
                               lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_t *lbl = ui_label(btn, text, &lv_font_montserrat_28, UI_THEME_COLOR(text_color));
    lv_obj_center(lbl);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static lv_obj_t *wifi_make_spinner(lv_obj_t *parent)
{
    lv_obj_t *spinner = lv_spinner_create(parent);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_size(spinner, 96, 96);
    lv_obj_set_style_arc_width(spinner, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 10, LV_PART_INDICATOR);
    if (current_theme) {
        lv_obj_set_style_arc_color(spinner, lv_color_hex(current_theme->bento_border), LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    }
    return spinner;
}

/* 96 px list-row shell shared by the saved and scan lists. */
static lv_obj_t *wifi_make_row(lv_obj_t *parent, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), WIFI_ROW_H);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(row, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(current_theme->bento_border), LV_STATE_PRESSED);
    }
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 24, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    if (cb) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }
    return row;
}

/* ════════════════════════════════════════════════════════════════════════
 *  4a. WiFi home — connected row, 3 saved slots, ADD NETWORK
 * ════════════════════════════════════════════════════════════════════════ */

static void wifi_saved_row_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    const app_config_t *cfg = app_config_get();
    if (slot < 0 || slot >= WIFI_SLOT_COUNT) {
        return;
    }
    if (cfg->wifi_networks[slot].ssid[0] == '\0') {
        return;
    }
    if (wifi_join_active()) {
        /* Scan/join owns the radio right now — refuse rather than race it. */
        nina_toast_show_fmt(TOAST_INFO, "Busy, try again");
        return;
    }
    /* Existing manual-switch flow in main.c — no join machinery involved. */
    nina_toast_show_fmt(TOAST_INFO, "Switching to %.31s", cfg->wifi_networks[slot].ssid);
    wifi_switch_to_network(slot);
}

static void wifi_forget_disarm_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_forget_timer = NULL;   /* repeat_count 1: LVGL deletes the timer itself */
    s_forget_armed = -1;
    if (s_forget_lbl) {
        lv_label_set_text(s_forget_lbl, "FORGET");
    }
    s_forget_lbl = NULL;
}

static void wifi_forget_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= WIFI_SLOT_COUNT) {
        return;
    }

    if (s_forget_armed != slot) {
        /* First tap (or a different row's FORGET): arm a 5 s confirm. */
        if (s_forget_lbl) {
            lv_label_set_text(s_forget_lbl, "FORGET");   /* disarm the other row */
        }
        s_forget_armed = slot;
        lv_obj_t *btn = lv_event_get_target_obj(e);
        s_forget_lbl = lv_obj_get_child(btn, 0);
        if (s_forget_lbl) {
            lv_label_set_text(s_forget_lbl, "SURE?");
        }
        if (s_forget_timer) {
            lv_timer_delete(s_forget_timer);
        }
        s_forget_timer = lv_timer_create(wifi_forget_disarm_cb, 5000, NULL);
        lv_timer_set_repeat_count(s_forget_timer, 1);
        return;
    }

    /* Second tap within 5 s: forget the slot. Synchronous save — a forgotten
     * network must survive an instant power cut. */
    if (s_forget_timer) {
        lv_timer_delete(s_forget_timer);
        s_forget_timer = NULL;
    }
    s_forget_armed = -1;
    s_forget_lbl   = NULL;

    app_config_t *snap = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (snap) {
        app_config_get_snapshot_into(snap);
        memset(&snap->wifi_networks[slot], 0, sizeof(snap->wifi_networks[slot]));
        app_config_save(snap);
        heap_caps_free(snap);
    }
    /* LAST statement: the rebuild deletes this button — no widget access after. */
    settings_hub_goto(HUB_SCREEN_WIFI_HOME);
}

static void wifi_add_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* Fresh scan on every ADD entry; false = already busy, build renders it. */
    wifi_join_start_scan();
    settings_hub_goto(HUB_SCREEN_WIFI_SCAN);
}

static void build_wifi_home(lv_obj_t *root)
{
    const app_config_t *cfg = app_config_get();

    /* Tidy up a terminal join state the user walked away from (e.g. exited
     * settings during a connect). ack is not navigation — safe inside build. */
    wifi_join_state_t st = wifi_join_get_state();
    if (st == WIFI_JOIN_SUCCESS || st == WIFI_JOIN_FAIL_AUTH ||
        st == WIFI_JOIN_FAIL_NO_AP || st == WIFI_JOIN_FAIL_TIMEOUT) {
        wifi_join_ack_result();
    }

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 6, 0);

    settings_hub_make_header(root, "WIFI");

    /* ── CONNECTED row (current slot per the WiFi manager; RSSI lives on the
     *    hub tile and More screen — this file makes no esp_wifi calls). ── */
    ui_label(root, "CONNECTED", &lv_font_montserrat_24, UI_THEME_COLOR(label_color));

    int cur = wifi_get_current_network_index();
    bool have_cur = (cur >= 0 && cur < WIFI_SLOT_COUNT &&
                     cfg->wifi_networks[cur].ssid[0] != '\0');

    lv_obj_t *conn = wifi_make_row(root, NULL, NULL);
    lv_obj_t *dot = lv_obj_create(conn);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    if (have_cur) {
        lv_obj_set_style_bg_color(dot, lv_color_hex(WIFI_OK_COLOR), 0);
    } else if (current_theme) {
        lv_obj_set_style_bg_color(dot, lv_color_hex(current_theme->bento_border), 0);
    }
    lv_obj_t *conn_lbl = ui_label(conn, have_cur ? cfg->wifi_networks[cur].ssid : "Not connected",
                                  &lv_font_montserrat_32,
                                  have_cur ? UI_THEME_COLOR(text_color) : UI_THEME_COLOR(label_color));
    lv_obj_set_flex_grow(conn_lbl, 1);
    lv_label_set_long_mode(conn_lbl, LV_LABEL_LONG_DOT);

    /* ── SAVED slots (all three; tap switches, FORGET is a two-tap confirm) ── */
    ui_label(root, "SAVED", &lv_font_montserrat_24, UI_THEME_COLOR(label_color));

    int used = 0;
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        bool empty = (cfg->wifi_networks[i].ssid[0] == '\0');
        lv_obj_t *row = wifi_make_row(root, empty ? NULL : wifi_saved_row_cb,
                                      (void *)(intptr_t)i);

        lv_obj_t *ssid = ui_label(row, empty ? "(empty slot)" : cfg->wifi_networks[i].ssid,
                                  &lv_font_montserrat_32,
                                  empty ? UI_THEME_COLOR(label_color) : UI_THEME_COLOR(text_color));
        lv_obj_set_flex_grow(ssid, 1);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);

        if (empty) {
            lv_obj_add_state(row, LV_STATE_DISABLED);
        } else {
            used++;
            lv_obj_t *forget = wifi_make_btn(row, "FORGET", WIFI_FORGET_W, WIFI_FORGET_H,
                                             wifi_forget_cb, (void *)(intptr_t)i);
            /* Child click does not bubble to the row: no accidental switch. */
            lv_obj_set_style_text_font(lv_obj_get_child(forget, 0), &lv_font_montserrat_24, 0);
        }
    }

    /* ── ADD NETWORK ── */
    lv_obj_t *add = wifi_make_btn(root, LV_SYMBOL_PLUS "  ADD NETWORK",
                                  LV_PCT(100), WIFI_ROW_H, wifi_add_cb, NULL);
    if (used >= WIFI_SLOT_COUNT) {
        lv_obj_add_state(add, LV_STATE_DISABLED);
        ui_label(root, "All 3 slots full - forget one first",
                 &lv_font_montserrat_24, UI_THEME_COLOR(label_color));
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  4b. Scan list — spinner, sorted rows, RESCAN, failure state
 * ════════════════════════════════════════════════════════════════════════ */

static void wifi_rescan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (wifi_join_start_scan()) {
        /* LAST statement: rebuild shows the spinner. */
        settings_hub_goto(HUB_SCREEN_WIFI_SCAN);
    }
}

static void wifi_scan_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_scan_count) {
        return;
    }
    if (s_scan[idx].secured) {
        settings_wifi_set_candidate(s_scan[idx].ssid, true);
        settings_hub_goto(HUB_SCREEN_WIFI_PASSWORD);
    } else {
        /* Open network: skip the password screen, connect straight away.
         * s_scan is file-scope — the pointer stays valid past the rebuild. */
        settings_wifi_set_candidate(s_scan[idx].ssid, false);
        wifi_join_start_connect(s_scan[idx].ssid, "");
        settings_hub_goto(HUB_SCREEN_WIFI_CONNECT);
    }
}

static void wifi_hidden_row_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    settings_wifi_set_candidate("", true);   /* empty SSID = hidden variant */
    settings_hub_goto(HUB_SCREEN_WIFI_PASSWORD);
}

/* 4-bar RSSI glyph: filled count from thresholds -55/-67/-75 (int math only). */
static void wifi_make_rssi_bars(lv_obj_t *parent, int rssi)
{
    int filled;
    if (rssi >= -55) {
        filled = 4;
    } else if (rssi >= -67) {
        filled = 3;
    } else if (rssi >= -75) {
        filled = 2;
    } else {
        filled = 1;
    }

    lv_obj_t *bars = lv_obj_create(parent);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, 52, 34);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(bars, 4, 0);
    lv_obj_clear_flag(bars, LV_OBJ_FLAG_SCROLLABLE);

    static const int heights[4] = {12, 19, 26, 34};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(bars);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 10, heights[i]);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        if (current_theme) {
            lv_obj_set_style_bg_color(bar,
                lv_color_hex(i < filled ? current_theme->progress_color
                                        : current_theme->bento_border), 0);
        }
    }
}

static void build_wifi_scan(lv_obj_t *root)
{
    /* Kick a scan only from a cold state. SCANNING = one is in flight;
     * SCAN_DONE/SCAN_FAILED = render the result (a notify-rebuild lands here
     * — restarting would loop scan -> notify -> rebuild -> scan forever). */
    wifi_join_state_t st = wifi_join_get_state();
    if (st != WIFI_JOIN_SCANNING && st != WIFI_JOIN_SCAN_DONE &&
        st != WIFI_JOIN_SCAN_FAILED) {
        if (wifi_join_start_scan()) {
            st = wifi_join_get_state();
        }
    }

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 8, 0);

    lv_obj_t *header = settings_hub_make_header(root, "CHOOSE NETWORK");
    lv_obj_t *rescan = wifi_make_btn(header, "RESCAN", WIFI_RESCAN_W, WIFI_RESCAN_H,
                                     wifi_rescan_cb, NULL);
    lv_obj_align(rescan, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(rescan, 0), &lv_font_montserrat_24, 0);
    wifi_rescan_obj = rescan;

    if (st == WIFI_JOIN_SCANNING) {
        lv_obj_t *wrap = lv_obj_create(root);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_width(wrap, LV_PCT(100));
        lv_obj_set_flex_grow(wrap, 1);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(wrap, 24, 0);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        wifi_make_spinner(wrap);
        ui_label(wrap, "Scanning...", &lv_font_montserrat_32, UI_THEME_COLOR(text_color));
        return;
    }

    if (st == WIFI_JOIN_SCAN_FAILED) {
        lv_obj_t *wrap = lv_obj_create(root);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_width(wrap, LV_PCT(100));
        lv_obj_set_flex_grow(wrap, 1);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(wrap, 24, 0);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *warn = ui_label(wrap, LV_SYMBOL_WARNING, &lv_font_montserrat_48, UI_COLOR_NONE);
        lv_obj_set_style_text_color(warn, lv_color_hex(WIFI_DANGER_COLOR), 0);
        ui_label(wrap, "Scan failed", &lv_font_montserrat_32, UI_THEME_COLOR(text_color));
        wifi_make_btn(wrap, "RETRY", 240, WIFI_ROW_H, wifi_rescan_cb, NULL);
        return;
    }

    /* SCAN_DONE (or a stale busy-connect state, which renders an empty list):
     * the one permitted scroller in Panel Mode. */
    s_scan_count = wifi_join_get_scan_results(s_scan, WIFI_SCAN_MAX);

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    wifi_list_obj = list;

    if (s_scan_count == 0) {
        ui_label(list, "No networks found", &lv_font_montserrat_28, UI_THEME_COLOR(label_color));
    }

    for (int i = 0; i < s_scan_count; i++) {
        if (strlen(s_scan[i].ssid) > 31) {
            /* Cannot be persisted into wifi_network_t.ssid[32]; a truncated
             * save would poison the reconnect walk — don't offer it. */
            continue;
        }
        lv_obj_t *row = wifi_make_row(list, wifi_scan_row_cb, (void *)(intptr_t)i);
        lv_obj_t *ssid = ui_label(row, s_scan[i].ssid, &lv_font_montserrat_32,
                                  UI_THEME_COLOR(text_color));
        lv_obj_set_flex_grow(ssid, 1);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        wifi_make_rssi_bars(row, (int)s_scan[i].rssi);
        if (s_scan[i].secured) {
            /* Secured marker (LVGL ships no padlock glyph). */
            ui_label(row, LV_SYMBOL_WIFI, &lv_font_montserrat_28, UI_THEME_COLOR(label_color));
        }
    }

    /* Tail row: manual SSID entry for hidden networks. */
    lv_obj_t *hidden = wifi_make_row(list, wifi_hidden_row_cb, NULL);
    ui_label(hidden, "Hidden network...", &lv_font_montserrat_32, UI_THEME_COLOR(label_color));
}

/* ════════════════════════════════════════════════════════════════════════
 *  4c. Password entry — textarea, eye toggle, CONNECT gate, keyboard
 * ════════════════════════════════════════════════════════════════════════ */

/* Enable CONNECT only when the input can possibly work: SSID present (typed,
 * for the hidden variant) and >= 8 password chars unless the network is open. */
static void wifi_connect_gate_update(void)
{
    if (!wifi_connect_obj) {
        return;
    }
    bool ok = true;
    if (s_cand_ssid[0] == '\0') {
        const char *sid = s_ssid_ta ? lv_textarea_get_text(s_ssid_ta) : "";
        if (sid[0] == '\0') {
            ok = false;
        }
    }
    if (s_cand_secured) {
        const char *pw = s_pw_ta ? lv_textarea_get_text(s_pw_ta) : "";
        if (strlen(pw) < 8) {
            ok = false;
        }
    }
    if (ok) {
        lv_obj_remove_state(wifi_connect_obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(wifi_connect_obj, LV_STATE_DISABLED);
    }
}

/* FOCUSED: dock the keyboard onto the focused textarea. VALUE_CHANGED:
 * re-evaluate the CONNECT gate. */
static void wifi_ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        if (wifi_kb_obj) {
            lv_keyboard_set_textarea(wifi_kb_obj, lv_event_get_target_obj(e));
        }
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        wifi_connect_gate_update();
    }
}

static void wifi_eye_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!s_pw_ta) {
        return;
    }
    bool now_hidden = !lv_textarea_get_password_mode(s_pw_ta);
    lv_textarea_set_password_mode(s_pw_ta, now_hidden);
    if (s_eye_lbl) {
        lv_label_set_text(s_eye_lbl, now_hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }
}

static void wifi_do_connect(void)
{
    if (s_cand_ssid[0] == '\0') {
        /* Hidden-network variant: take the typed SSID. */
        if (!s_ssid_ta) {
            return;
        }
        char typed[33];
        strlcpy(typed, lv_textarea_get_text(s_ssid_ta), sizeof(typed));
        if (typed[0] == '\0') {
            return;
        }
        strlcpy(s_cand_ssid, typed, sizeof(s_cand_ssid));
    }

    char pw[65];
    pw[0] = '\0';
    if (s_pw_ta) {
        strlcpy(pw, lv_textarea_get_text(s_pw_ta), sizeof(pw));
    }
    if (s_cand_secured && strlen(pw) < 8) {
        return;   /* gate should have blocked this; defensive */
    }

    wifi_join_start_connect(s_cand_ssid, pw);
    /* LAST statement: deletes the textareas this function just read. */
    settings_hub_goto(HUB_SCREEN_WIFI_CONNECT);
}

static void wifi_connect_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    wifi_do_connect();
}

/* Keyboard checkmark key. */
static void wifi_kb_ready_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    wifi_do_connect();
}

/* One-line themed textarea for the password screen. */
static lv_obj_t *wifi_make_ta(lv_obj_t *parent, const char *placeholder,
                              int h, uint32_t max_len)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_height(ta, h);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_32, 0);
    lv_obj_set_style_radius(ta, 14, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(ta, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(current_theme->text_color), 0);
    }
    lv_obj_add_event_cb(ta, wifi_ta_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, wifi_ta_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return ta;
}

static void build_wifi_password(lv_obj_t *root)
{
    bool hidden_variant = (s_cand_ssid[0] == '\0');
    wifi_hidden_variant = hidden_variant;
    /* Above-keyboard budget is 308 px (688 root - 380 keyboard):
     * normal 72+88+96+2x6 = 268; hidden 72+72+72+72+3x6 = 306. */
    int row_h     = hidden_variant ? 72 : 88;
    int connect_h = hidden_variant ? 72 : WIFI_ROW_H;

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 6, 0);

    lv_obj_t *header = settings_hub_make_header(root,
        hidden_variant ? "HIDDEN NETWORK" : s_cand_ssid);
    /* An SSID can be 32 chars — keep the centered title off the BACK button. */
    lv_obj_t *title = lv_obj_get_child(header, 1);
    if (title) {
        lv_obj_set_width(title, 420);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    }

    if (hidden_variant) {
        lv_obj_t *srow = lv_obj_create(root);
        lv_obj_remove_style_all(srow);
        lv_obj_set_size(srow, LV_PCT(100), row_h);
        lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(srow, LV_OBJ_FLAG_SCROLLABLE);
        /* 31 max: wifi_network_t.ssid[32] must hold it NUL-terminated. */
        s_ssid_ta = wifi_make_ta(srow, "Network name", row_h, 31);
        wifi_srow_obj = srow;
    }

    /* Password row: textarea + 72x72 eye toggle */
    lv_obj_t *prow = lv_obj_create(root);
    lv_obj_remove_style_all(prow);
    lv_obj_set_size(prow, LV_PCT(100), row_h);
    lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(prow, 8, 0);
    lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);
    wifi_prow_obj = prow;

    s_pw_ta = wifi_make_ta(prow, "Password", row_h, 63);
    lv_textarea_set_password_mode(s_pw_ta, true);

    lv_obj_t *eye = wifi_make_btn(prow, LV_SYMBOL_EYE_OPEN, WIFI_EYE_SZ, WIFI_EYE_SZ,
                                  wifi_eye_cb, NULL);
    s_eye_lbl = lv_obj_get_child(eye, 0);

    /* CONNECT — disabled until the gate passes */
    wifi_connect_obj = wifi_make_btn(root, "CONNECT", LV_PCT(100), connect_h,
                                  wifi_connect_btn_cb, NULL);
    wifi_connect_gate_update();

    /* Keyboard: bottom-docked, floating out of the flex flow. */
    wifi_kb_obj = lv_keyboard_create(root);
    lv_obj_add_flag(wifi_kb_obj, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(wifi_kb_obj, LV_PCT(100), WIFI_KB_H);
    lv_obj_align(wifi_kb_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(wifi_kb_obj, &lv_font_montserrat_28, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(wifi_kb_obj, lv_color_hex(current_theme->bg_main), 0);
        lv_obj_set_style_bg_opa(wifi_kb_obj, LV_OPA_COVER, 0);
    }
    lv_keyboard_set_textarea(wifi_kb_obj, hidden_variant ? s_ssid_ta : s_pw_ta);
    lv_obj_add_event_cb(wifi_kb_obj, wifi_kb_ready_cb, LV_EVENT_READY, NULL);
}

/* ════════════════════════════════════════════════════════════════════════
 *  4d. Connect result — chrome-free, driven by wifi_join_get_state()
 * ════════════════════════════════════════════════════════════════════════ */

static void wifi_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    wifi_join_cancel();   /* backend rejoins the previous network */
    settings_hub_goto(HUB_SCREEN_WIFI_HOME);
}

static void wifi_try_again_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    wifi_join_ack_result();
    if (!s_cand_secured) {
        /* Open network never had a password screen — retry directly. */
        wifi_join_start_connect(s_cand_ssid, "");
        settings_hub_goto(HUB_SCREEN_WIFI_CONNECT);
    } else {
        /* Password screen rebuilds with a cleared field. */
        settings_hub_goto(HUB_SCREEN_WIFI_PASSWORD);
    }
}

static void wifi_result_back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    wifi_join_ack_result();
    settings_hub_goto(HUB_SCREEN_WIFI_HOME);
}

static void wifi_result_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    /* NULL first: the goto below deletes the screen, whose delete-cb would
     * otherwise lv_timer_delete this timer while LVGL still owns it
     * (repeat_count 1 — LVGL auto-deletes after this callback returns). */
    s_result_timer = NULL;
    wifi_join_ack_result();
    settings_hub_goto(HUB_SCREEN_WIFI_HOME);
}

/* TRY AGAIN + BACK pair for the failure states. */
static void wifi_make_fail_buttons(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(90), WIFI_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    wifi_make_btn(row, "TRY AGAIN", 280, WIFI_ROW_H, wifi_try_again_cb, NULL);
    wifi_make_btn(row, "BACK", 200, WIFI_ROW_H, wifi_result_back_cb, NULL);
}

static void build_wifi_connect(lv_obj_t *root)
{
    wifi_join_state_t st = wifi_join_get_state();
    char buf[64];

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(root, 28, 0);

    if (st == WIFI_JOIN_CONNECTING || st == WIFI_JOIN_REJOINING) {
        wifi_make_spinner(root);
        if (st == WIFI_JOIN_CONNECTING) {
            snprintf(buf, sizeof(buf), "Connecting to %.32s...", s_cand_ssid);
        } else {
            snprintf(buf, sizeof(buf), "Restoring previous network...");
        }
        lv_obj_t *lbl = ui_label(root, buf, &lv_font_montserrat_32, UI_THEME_COLOR(text_color));
        lv_obj_set_width(lbl, LV_PCT(90));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        /* BACK during connect = cancel; the backend rejoins the old network. */
        wifi_make_btn(root, "BACK", 200, WIFI_ROW_H, wifi_cancel_cb, NULL);
        return;
    }

    if (st == WIFI_JOIN_SUCCESS) {
        lv_obj_t *check = ui_label(root, LV_SYMBOL_OK, &lv_font_montserrat_48, UI_COLOR_NONE);
        lv_obj_set_style_text_color(check, lv_color_hex(WIFI_OK_COLOR), 0);
        snprintf(buf, sizeof(buf), "Connected, %d dBm", (int)wifi_join_success_rssi());
        ui_label(root, buf, &lv_font_montserrat_32, UI_THEME_COLOR(text_color));
        /* Slot already saved by the backend; auto-return after 2 s. */
        if (s_result_timer) {
            lv_timer_delete(s_result_timer);
        }
        s_result_timer = lv_timer_create(wifi_result_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_result_timer, 1);
        return;
    }

    if (st == WIFI_JOIN_FAIL_AUTH) {
        lv_obj_t *warn = ui_label(root, LV_SYMBOL_WARNING, &lv_font_montserrat_48, UI_COLOR_NONE);
        lv_obj_set_style_text_color(warn, lv_color_hex(WIFI_DANGER_COLOR), 0);
        ui_label(root, "Wrong password", &lv_font_montserrat_32, UI_THEME_COLOR(text_color));
        wifi_make_fail_buttons(root);
        return;
    }

    /* FAIL_NO_AP / FAIL_TIMEOUT — and any unexpected state, defensively.
     * The previous network was already rejoined by the backend. */
    lv_obj_t *warn = ui_label(root, LV_SYMBOL_WARNING, &lv_font_montserrat_48, UI_COLOR_NONE);
    lv_obj_set_style_text_color(warn, lv_color_hex(WIFI_DANGER_COLOR), 0);
    ui_label(root, "Could not connect", &lv_font_montserrat_32, UI_THEME_COLOR(text_color));
    ui_label(root, "Your previous network was restored",
             &lv_font_montserrat_24, UI_THEME_COLOR(label_color));
    wifi_make_fail_buttons(root);
}
