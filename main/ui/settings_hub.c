/**
 * @file settings_hub.c
 * @brief Panel Mode settings — launcher hub of six oversized tiles plus the
 *        Theme / Brightness / Pages / More full-screen sub-pages.
 *
 * Replaces the 4-tab settings tabview. Every change applies instantly and
 * persists through the PSRAM-snapshot + app_config_save_deferred() path;
 * there is no Save button, no dirty tracking, no config baseline.
 *
 * Lifecycle contract (owned by nina_dashboard.c, unchanged from the tabview):
 * created lazily on show, destroyed on hide, modal_open/close handled by the
 * dashboard. Every entry point here runs on the LVGL task with the display
 * lock already held by the caller — nothing in this file takes the lock.
 *
 * The four WiFi screens are built by settings_wifi.c through the
 * settings_wifi_build() seam declared in settings_hub.h.
 */

#include "settings_hub.h"
#include "settings_hub_internal.h"     /* layout seam: published handles + factories */
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"   /* SYSINFO_PAGE_IDX, page_count, total_page_count */
#include "page_registry.h"             /* Home Page roller options + page_ref_navigate */
#include "app_config.h"
#include "themes.h"
#include "ui_helpers.h"
#include "ui_styles.h"
#include "nina_toast.h"                /* demo-mode state feedback */
#include "display_defs.h"              /* screen_size(), display_rotation_apply */
#include "wifi_join.h"                 /* notify-cb deregistration on leaving the WiFi family */
#include "tasks.h"                     /* data_task_handle — demo tile wake */
#include "power_mgmt.h"                /* app_reboot — single logged restart path */
#include "ota_github.h"                /* firmware version for the More info block */
#include "bsp/esp-bsp.h"               /* bsp_display_brightness_set */
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"             /* xTaskNotifyGive */
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Layout ──────────────────────────────────────────────────────────── */
#define HUB_ROOT_SIZE   (screen_size() - 2 * OUTER_PADDING)  /* 688, same as the tabview root */
#define HUB_HEADER_H     72
#define HUB_BACK_W       96
#define HUB_TILE_W      330   /* 2 x 330 + 20 gap + 2 x 4 pad = 688 */
#define HUB_TILE_H      176
#define HUB_TILE_GAP     20
#define HUB_ROW_H        96   /* More-screen action rows */
#define HUB_CARD_W      214   /* theme cards: 3 x 214 + 2 x 17 gap + 2 x 6 pad = 688 */
#define HUB_CARD_H      196   /* 72 header + 3 x 196 + 2 x 14 row gap = 688 exactly */
#define HUB_BTN_SZ       96   /* brightness +/- buttons */
#define HUB_DANGER_COLOR 0xB02A2A

/* ── Static state ────────────────────────────────────────────────────── */
static lv_obj_t     *s_root    = NULL;  /* hub root container (lives in main_cont) */
static lv_obj_t     *s_screen  = NULL;  /* current screen container inside s_root */
static hub_screen_t  s_current = HUB_SCREEN_HUB;

/* Handles the round fit pass needs. settings_hub_make_header() publishes the
 * header for every screen that has one; a screen builder publishes its grid.
 * settings_hub_goto() clears both before each build, so a screen without such
 * an object leaves them NULL. Declared in settings_hub_internal.h. */
lv_obj_t *hub_header_obj = NULL;
lv_obj_t *hub_grid_obj   = NULL;

/* Brightness screen */
static lv_obj_t *s_bl_slider = NULL;
static lv_obj_t *s_bl_val    = NULL;
static lv_obj_t *s_tb_slider = NULL;
static lv_obj_t *s_tb_val    = NULL;

/* Pages screen */
static lv_obj_t *s_seg_mode    = NULL;
static lv_obj_t *s_home_picker = NULL;  /* full-screen Home Page picker overlay */
static lv_obj_t *s_cycle_val   = NULL;  /* CYCLE on/off row value label */
static int       s_pages_tab   = -1;    /* 0 MANUAL / 1 HOME PAGE / 2 CYCLE; -1 = derive
                                           from config on next build (per hub session) */

/* More screen */
static lv_obj_t   *s_rotate_val   = NULL;
static lv_obj_t   *s_mute_val     = NULL;
static lv_obj_t   *s_reboot_lbl   = NULL;
static bool        s_reboot_armed = false;
static lv_timer_t *s_arm_timer    = NULL;  /* disarms the two-tap reboot confirm */
static lv_obj_t   *s_fr_overlay   = NULL;  /* factory-reset full-screen confirm */
static lv_obj_t   *s_fr_yes       = NULL;
static lv_timer_t *s_fr_timer     = NULL;  /* 3 s enable gate on the destructive button */

/* Pages segmented map (buttonmatrix) */
static const char *s_seg_map[] = {"MANUAL", SCREEN_ROUND ? "HOME" : "HOME PAGE", "CYCLE", ""};

/* ── Forward declarations ────────────────────────────────────────────── */
static void build_hub_screen(lv_obj_t *parent);
static void build_theme_screen(lv_obj_t *parent);
static void build_brightness_screen(lv_obj_t *parent);
static void build_pages_screen(lv_obj_t *parent);
static void build_more_screen(lv_obj_t *parent);
static void hub_back_cb(lv_event_t *e);
static void hub_tile_theme_cb(lv_event_t *e);
static void hub_tile_brightness_cb(lv_event_t *e);
static void hub_tile_wifi_cb(lv_event_t *e);
static void hub_tile_pages_cb(lv_event_t *e);
static void hub_tile_more_cb(lv_event_t *e);
static void hub_tile_demo_cb(lv_event_t *e);
static void theme_card_cb(lv_event_t *e);
static void bl_slider_cb(lv_event_t *e);
static void bl_step_cb(lv_event_t *e);
static void tb_slider_cb(lv_event_t *e);
static void tb_step_cb(lv_event_t *e);
static void pages_mode_cb(lv_event_t *e);
static void pages_home_row_cb(lv_event_t *e);
static void home_pick_row_cb(lv_event_t *e);
static void home_pick_cancel_cb(lv_event_t *e);
static void cycle_toggle_cb(lv_event_t *e);
static void cycle_chip_cb(lv_event_t *e);
static void more_rotate_cb(lv_event_t *e);
static void more_mute_cb(lv_event_t *e);
static void more_reboot_cb(lv_event_t *e);
static void more_factory_cb(lv_event_t *e);
static void fr_cancel_cb(lv_event_t *e);
static void fr_confirm_cb(lv_event_t *e);
static void reboot_disarm_timer_cb(lv_timer_t *t);
static void fr_enable_timer_cb(lv_timer_t *t);
static lv_obj_t *hub_make_more_row(lv_obj_t *parent, const char *title,
                                   lv_obj_t **out_value, lv_event_cb_t cb);

/* ════════════════════════════════════════════════════════════════════════
 *  Persistence helpers — PSRAM snapshot pattern
 * ════════════════════════════════════════════════════════════════════════ */

/* Never mutate app_config_get() field-by-field and never place an
 * app_config_t (9436 bytes) on a task stack: snapshot into PSRAM, poke the
 * field(s) the control owns, then commit. */
static app_config_t *hub_snap_begin(void)
{
    app_config_t *snap = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!snap) {
        return NULL;   /* apply-live still happened; persist lost, tolerable */
    }
    app_config_get_snapshot_into(snap);
    return snap;
}

static void hub_snap_commit(app_config_t *snap)
{
    app_config_save_deferred(snap);   /* commits to RAM now (validate + normalize),
                                         NVS write debounced */
    heap_caps_free(snap);             /* safe: commit copies before return */
}

/* ════════════════════════════════════════════════════════════════════════
 *  Small shared helpers — slideshow list membership, Red Night color map
 * ════════════════════════════════════════════════════════════════════════ */

/* Count the valid stops in an auto_rotate_order2[] list. */
static int hub_order2_count(const uint8_t *order)
{
    int n = 0;
    for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
        if (order[i] != 0xFF && ARP_STOP_IS_VALID(order[i])) {
            n++;
        }
    }
    return n;
}

static bool hub_order2_contains(const uint8_t *order, uint8_t id)
{
    for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
        if (order[i] == id) {
            return true;
        }
    }
    return false;
}

/* Set membership of @p id in the order2 list using the exact encoding the
 * web parse writes (web_handlers_config.c, "auto_rotate_order2"): valid stops
 * packed at the front in play order, every free slot 0xFF. Remove compacts
 * forward and 0xFF-fills the tail; add appends at the first free slot.
 * Invalid stops are dropped in passing (validate_config clamps them to 0xFF
 * anyway) and remove-then-add dedupes. The 18 eligible ids never fill the
 * 24-slot list, so append cannot overflow after compaction. */
static void hub_order2_set(uint8_t *order, uint8_t id, bool want)
{
    int w = 0;
    for (int r = 0; r < ARP_ORDER_CAPACITY; r++) {
        uint8_t v = order[r];
        if (v == id || v == 0xFF || !ARP_STOP_IS_VALID(v)) {
            continue;
        }
        order[w] = v;
        w++;
    }
    if (want && w < ARP_ORDER_CAPACITY) {
        order[w] = id;
        w++;
    }
    while (w < ARP_ORDER_CAPACITY) {
        order[w] = 0xFF;
        w++;
    }
}

/* Map a color to a pure-red shade of equal luminance — the lv_color_t
 * adaptation of image_red_remap_rgb565_force() (same 77/150/29 weights).
 * Colors already pure red (g == b == 0, e.g. the Red Night palette itself)
 * pass through unchanged so the active theme's card previews exactly what
 * is on screen. */
static lv_color_t hub_red_shade(uint32_t hex)
{
    if ((hex & 0x00FFFFu) == 0) {
        return lv_color_hex(hex);
    }
    uint32_t r = (hex >> 16) & 0xFFu;
    uint32_t g = (hex >> 8) & 0xFFu;
    uint32_t b = hex & 0xFFu;
    uint8_t luma = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
    return lv_color_make(luma, 0, 0);
}

/* Theme-card color: the theme's raw palette normally; red-only while the
 * ACTIVE theme is Red Night (star-party white-light discipline — the picker
 * must not flash other themes' full-color palettes). */
static lv_color_t hub_card_color(uint32_t hex, bool red_only)
{
    if (red_only) {
        return hub_red_shade(hex);
    }
    return lv_color_hex(hex);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Screen switching
 * ════════════════════════════════════════════════════════════════════════ */

static bool screen_is_wifi(hub_screen_t s)
{
    return s == HUB_SCREEN_WIFI_HOME || s == HUB_SCREEN_WIFI_SCAN ||
           s == HUB_SCREEN_WIFI_PASSWORD || s == HUB_SCREEN_WIFI_CONNECT;
}

/* Kill screen-scoped timers and NULL every per-screen widget pointer.
 * lv_timers are not parented to objects: deleting the screen without deleting
 * its timers would leave them firing into freed widgets. */
static void hub_reset_screen_state(void)
{
    if (s_arm_timer) {
        lv_timer_delete(s_arm_timer);
        s_arm_timer = NULL;
    }
    if (s_fr_timer) {
        lv_timer_delete(s_fr_timer);
        s_fr_timer = NULL;
    }
    s_reboot_armed = false;
    s_reboot_lbl   = NULL;
    s_fr_overlay   = NULL;
    s_fr_yes       = NULL;
    s_rotate_val   = NULL;
    s_mute_val     = NULL;
    s_bl_slider    = NULL;
    s_bl_val       = NULL;
    s_tb_slider    = NULL;
    s_tb_val       = NULL;
    s_seg_mode     = NULL;
    s_home_picker  = NULL;
    s_cycle_val    = NULL;
}

void settings_hub_goto(hub_screen_t screen)
{
    if (!s_root) {
        return;
    }

    /* Leaving the WiFi family: stop join notifications from reaching a screen
     * that no longer exists. settings_wifi.c re-registers on entry. */
    if (screen_is_wifi(s_current) && !screen_is_wifi(screen)) {
        wifi_join_set_notify_cb(NULL);
    }

    hub_reset_screen_state();
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_current = screen;

    s_screen = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    hub_header_obj = NULL;
    hub_grid_obj   = NULL;

    switch (screen) {
    case HUB_SCREEN_THEME:
        build_theme_screen(s_screen);
        break;
    case HUB_SCREEN_BRIGHTNESS:
        build_brightness_screen(s_screen);
        break;
    case HUB_SCREEN_PAGES:
        build_pages_screen(s_screen);
        break;
    case HUB_SCREEN_MORE:
        build_more_screen(s_screen);
        break;
    case HUB_SCREEN_WIFI_HOME:
    case HUB_SCREEN_WIFI_SCAN:
    case HUB_SCREEN_WIFI_PASSWORD:
    case HUB_SCREEN_WIFI_CONNECT:
        settings_wifi_build(s_screen, screen);
        break;
    case HUB_SCREEN_HUB:
    default:
        build_hub_screen(s_screen);
        break;
    }

    /* One dispatch point for every settings screen, hub and WiFi alike: the
     * round pass re-places what the builder above produced. */
#if CONFIG_NINA_FAMILY_ROUND
    settings_hub_round_fit(s_screen, screen);
#endif
}

hub_screen_t settings_hub_current(void)
{
    return s_current;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Shared header (72 px, BACK 96x72)
 * ════════════════════════════════════════════════════════════════════════ */

static void hub_back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    switch (s_current) {
    case HUB_SCREEN_HUB:
        /* Same exit as the old tabview back button: navigate to Sysinfo,
         * which routes through hide_page_at -> destroy -> modal close. */
        nina_dashboard_show_page(SYSINFO_PAGE_IDX(page_count), total_page_count);
        break;
    case HUB_SCREEN_WIFI_SCAN:
        settings_hub_goto(HUB_SCREEN_WIFI_HOME);
        break;
    case HUB_SCREEN_WIFI_PASSWORD:
        settings_hub_goto(HUB_SCREEN_WIFI_SCAN);
        break;
    default:
        settings_hub_goto(HUB_SCREEN_HUB);
        break;
    }
}

lv_obj_t *settings_hub_make_header(lv_obj_t *parent, const char *title)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), HUB_HEADER_H);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_button_create(header);
    lv_obj_set_size(btn, HUB_BACK_W, HUB_HEADER_H);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_t *lbl_back = ui_label(btn, LV_SYMBOL_LEFT " BACK", &lv_font_montserrat_24,
                                  UI_THEME_COLOR(text_color));
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn, hub_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_title = ui_label(header, title, &lv_font_montserrat_36,
                                   UI_THEME_COLOR(header_text_color));
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    /* Publish for the round fit pass: this factory is the single place every
     * settings and WiFi screen gets its header from, so one write here covers
     * all of them. No effect on square beyond the pointer itself. */
    hub_header_obj = header;

    return header;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Settings Hub screen — six tiles
 * ════════════════════════════════════════════════════════════════════════ */

/* One tile: big name label (child 0) + status line (child 1), whole tile is the
 * target. Square passes HUB_TILE_W / HUB_TILE_H; the round fit pass resizes. */
lv_obj_t *settings_hub_make_tile(lv_obj_t *parent, const char *name,
                                 const char *status, lv_event_cb_t cb, int w, int h)
{
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(tile, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(current_theme->bento_border), LV_STATE_PRESSED);
    }
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, 10, 0);

    lv_obj_t *lbl = ui_label(tile, name, &lv_font_montserrat_36, UI_THEME_COLOR(text_color));
    LV_UNUSED(lbl);

    if (status) {
        lv_obj_t *st = ui_label(tile, status, &lv_font_montserrat_24, UI_THEME_COLOR(label_color));
        lv_obj_set_width(st, w - 24);
        lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
    }

    if (cb) {
        lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);
    }
    return tile;
}

static void hub_tile_theme_cb(lv_event_t *e)      { LV_UNUSED(e); settings_hub_goto(HUB_SCREEN_THEME); }
static void hub_tile_brightness_cb(lv_event_t *e) { LV_UNUSED(e); settings_hub_goto(HUB_SCREEN_BRIGHTNESS); }
static void hub_tile_wifi_cb(lv_event_t *e)       { LV_UNUSED(e); settings_hub_goto(HUB_SCREEN_WIFI_HOME); }
static void hub_tile_pages_cb(lv_event_t *e)      { LV_UNUSED(e); settings_hub_goto(HUB_SCREEN_PAGES); }
static void hub_tile_more_cb(lv_event_t *e)       { LV_UNUSED(e); settings_hub_goto(HUB_SCREEN_MORE); }

/* DEMO tile IS the control: no sub-screen, no confirm. Save + wake the data
 * task (its main loop reconciles demo_mode vs demo_data_is_running within a
 * cycle), then rebuild the hub so the tile and header ring restyle now. */
static void hub_tile_demo_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bool want = !app_config_get()->demo_mode;

    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->demo_mode = want;
        hub_snap_commit(snap);
    }
    if (data_task_handle) {
        xTaskNotifyGive(data_task_handle);
    }

    /* Unmistakable state feedback — users asked whether a reboot is needed
     * (it is not). Toast bars float above the hub in z-order. */
    nina_toast_show(TOAST_INFO, want
        ? "Demo mode on - simulated data in a few seconds. No reboot needed."
        : "Demo mode off - live data resumes.");

    /* LAST statement: the rebuild deletes this tile — no widget access after. */
    settings_hub_goto(HUB_SCREEN_HUB);
}

static void build_hub_screen(lv_obj_t *parent)
{
    const app_config_t *cfg = app_config_get();
    char buf[64];

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = settings_hub_make_header(parent, "SETTINGS");
    if (cfg->demo_mode && current_theme) {
        /* Accent ring while demo mode is active — impossible to forget. */
        lv_obj_set_style_border_width(header, 8, 0);
        lv_obj_set_style_border_color(header, lv_color_hex(current_theme->progress_color), 0);
        lv_obj_set_style_radius(header, 12, 0);
    }

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    hub_grid_obj = grid;
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, HUB_TILE_GAP, 0);
    lv_obj_set_style_pad_row(grid, HUB_TILE_GAP, 0);
    lv_obj_set_style_pad_left(grid, 4, 0);
    lv_obj_set_style_pad_right(grid, 4, 0);
    lv_obj_set_style_pad_top(grid, 16, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* THEME — current theme name + 4-swatch strip */
    const theme_t *th = themes_get(cfg->theme_index);
    lv_obj_t *tile_theme = settings_hub_make_tile(grid, "THEME", th ? th->name : "--",
                                                  hub_tile_theme_cb, HUB_TILE_W, HUB_TILE_H);
    if (th) {
        lv_obj_t *strip = lv_obj_create(tile_theme);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, LV_SIZE_CONTENT, 12);
        lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(strip, 4, 0);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
        const uint32_t swatches[4] = { th->bg_main, th->bento_bg,
                                       th->progress_color, th->text_color };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *sw = lv_obj_create(strip);
            lv_obj_remove_style_all(sw);
            lv_obj_set_size(sw, 24, 12);
            lv_obj_set_style_radius(sw, 3, 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(sw, lv_color_hex(swatches[i]), 0);
        }
    }

    /* BRIGHTNESS. Round board 8's shortened status ("60% / 100%" style): the
     * two-value line at 28 px only fits the narrower round A-row box next to
     * the "%" digits, not next to "SCREEN"/"TEXT". Square is the shipped
     * literal, unchanged. */
    snprintf(buf, sizeof(buf), SCREEN_ROUND ? "%d%% / %d%%" : "SCREEN %d%%  TEXT %d%%",
             cfg->brightness, cfg->color_brightness);
    settings_hub_make_tile(grid, "BRIGHTNESS", buf, hub_tile_brightness_cb, HUB_TILE_W, HUB_TILE_H);

    /* WIFI: live SSID + RSSI when the station link is up. Round board 8
     * drops the RSSI (the SSID alone already reaches the tile's dotted-name
     * bound); the two formats take a different argument count, so this is an
     * if, not a format-string ternary. Square path is the shipped call,
     * unchanged. */
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (SCREEN_ROUND) {
            snprintf(buf, sizeof(buf), "%.32s", (const char *)ap.ssid);
        } else {
            snprintf(buf, sizeof(buf), "%.32s  %d dBm", (const char *)ap.ssid, (int)ap.rssi);
        }
    } else {
        snprintf(buf, sizeof(buf), "Not connected");
    }
    settings_hub_make_tile(grid, "WIFI", buf, hub_tile_wifi_cb, HUB_TILE_W, HUB_TILE_H);

    /* PAGES: nav mode summary. Round board 8 drops "pages" off CYCLE and
     * shortens the HOME PAGE label preview; square strings unchanged. */
    if (cfg->auto_rotate_enabled) {
        snprintf(buf, sizeof(buf), SCREEN_ROUND ? "Cycle: %d" : "Cycle: %d pages",
                 hub_order2_count(cfg->auto_rotate_order2));
    } else if (s_pages_tab == 0) {
        /* MANUAL was chosen this hub session (both non-cycle tabs persist as
         * auto_rotate off; the tab choice itself is session state). */
        snprintf(buf, sizeof(buf), "Manual");
    } else {
        const page_ref_entry_t *pe = page_ref_by_id((page_ref_t)cfg->active_page_override);
        snprintf(buf, sizeof(buf), SCREEN_ROUND ? "Home: %.10s" : "Home: %.24s",
                 pe ? pe->label : "Summary");
    }
    settings_hub_make_tile(grid, "PAGES", buf, hub_tile_pages_cb, HUB_TILE_W, HUB_TILE_H);

    /* DEMO MODE: the tile is the toggle; the ON state must be unmistakable.
     * Round board 8 names the tile "DEMO" in either state (the status line
     * below already carries "OFF" / "Tap to exit", and the active header
     * ring is the unmistakable cue); square keeps its two shipped literals. */
    lv_obj_t *tile_demo = settings_hub_make_tile(grid,
                                                 SCREEN_ROUND ? "DEMO"
                                                     : (cfg->demo_mode ? "DEMO MODE ON" : "DEMO MODE"),
                                                 cfg->demo_mode ? "Tap to exit" : "OFF",
                                                 hub_tile_demo_cb, HUB_TILE_W, HUB_TILE_H);
    if (cfg->demo_mode && current_theme) {
        lv_obj_set_style_bg_color(tile_demo, lv_color_hex(current_theme->progress_color), 0);
        /* Dark text on the accent fill so the state reads at a glance. */
        uint32_t n = lv_obj_get_child_count(tile_demo);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_set_style_text_color(lv_obj_get_child(tile_demo, i),
                                        lv_color_hex(current_theme->bg_main), 0);
        }
    }

    /* MORE. Round drops "/ info" (board 8); square keeps the shipped literal. */
    settings_hub_make_tile(grid, "MORE",
                           SCREEN_ROUND ? "rotate / reboot" : "rotate / reboot / info",
                           hub_tile_more_cb, HUB_TILE_W, HUB_TILE_H);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Theme picker — 3x3 grid of miniature theme cards
 * ════════════════════════════════════════════════════════════════════════ */

static void theme_card_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    /* Persist first (commit updates the live config the re-theme reads). */
    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->theme_index = idx;
        hub_snap_commit(snap);
    }

    /* LAST statement: the apply re-enters settings_hub_apply_theme, which
     * rebuilds this screen — touching any widget after this is use-after-free.
     * Called bare: this callback already runs on the LVGL task with the
     * display lock held. */
    nina_dashboard_apply_theme(idx);
}

/* One theme preview card. The two fake bento rects and the name line scale
 * with the card height so a shorter card still holds all four rows: at
 * h == HUB_CARD_H these resolve to the shipped 40, 26 and montserrat_24, and
 * at the round picker's 140 they give 28, 18 and a 28 px name, which also
 * clears the 27 px round text floor. */
lv_obj_t *settings_hub_make_theme_card(lv_obj_t *parent, int idx, int active,
                                       bool red_only, int w, int h)
{
    int r1_h = h * 40 / HUB_CARD_H;
    int r2_h = h * 26 / HUB_CARD_H;
    const lv_font_t *name_font = (h < HUB_CARD_H) ? &lv_font_montserrat_28
                                                  : &lv_font_montserrat_24;

    const theme_t *t = themes_get(idx);

    /* Card painted in that theme's OWN palette (raw colors, not
     * brightness-adjusted): the card is a preview, not UI chrome. */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, hub_card_color(t->bg_main, red_only), 0);
    if (idx == active) {
        lv_obj_set_style_border_width(card, 4, 0);
        lv_obj_set_style_border_color(card, hub_card_color(t->progress_color, red_only), 0);
    } else {
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, hub_card_color(t->bento_border, red_only), 0);
    }
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 10, 0);

    /* Accent bar */
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 6);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, hub_card_color(t->progress_color, red_only), 0);

    /* Two fake bento rects */
    lv_obj_t *r1 = lv_obj_create(card);
    lv_obj_remove_style_all(r1);
    lv_obj_set_size(r1, LV_PCT(100), r1_h);
    lv_obj_set_style_radius(r1, 6, 0);
    lv_obj_set_style_bg_opa(r1, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r1, hub_card_color(t->bento_bg, red_only), 0);
    lv_obj_set_style_border_width(r1, 1, 0);
    lv_obj_set_style_border_color(r1, hub_card_color(t->bento_border, red_only), 0);

    lv_obj_t *r2 = lv_obj_create(card);
    lv_obj_remove_style_all(r2);
    lv_obj_set_size(r2, LV_PCT(70), r2_h);
    lv_obj_set_style_radius(r2, 6, 0);
    lv_obj_set_style_bg_opa(r2, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r2, hub_card_color(t->bento_border, red_only), 0);

    /* Name label in that theme's text color; check glyph on the active card */
    lv_obj_t *name = lv_label_create(card);
    if (idx == active) {
        lv_label_set_text_fmt(name, "%s %s", t->name, LV_SYMBOL_OK);
    } else {
        lv_label_set_text(name, t->name);
    }
    lv_obj_set_style_text_font(name, name_font, 0);
    lv_obj_set_style_text_color(name, hub_card_color(t->text_color, red_only), 0);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_add_event_cb(card, theme_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    return card;
}

static void build_theme_screen(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    settings_hub_make_header(parent, "THEME");

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    hub_grid_obj = grid;
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 17, 0);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_left(grid, 6, 0);
    lv_obj_set_style_pad_right(grid, 6, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int count = themes_get_count();
    if (count > 9) {
        count = 9;   /* grid is sized for the frozen 9 themes */
    }
    int active = app_config_get()->theme_index;
    /* Red Night active: EVERY card renders red-only, including other themes'
     * palettes — full color returns when a non-red-night theme is applied
     * (the tap rebuilds this screen). */
    bool red_only = theme_is_red_night(themes_get(active));

    for (int i = 0; i < count; i++) {
        settings_hub_make_theme_card(grid, i, active, red_only, HUB_CARD_W, HUB_CARD_H);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Brightness — two giant slider rows
 * ════════════════════════════════════════════════════════════════════════ */

/* One brightness row: name + big value label above [-][slider][+]. Sliders
 * range 5..100 (floor clamp so you cannot black yourself out). Step buttons
 * carry (+/-5) in user_data. */
static lv_obj_t *hub_make_bright_row(lv_obj_t *parent, const char *name, int value,
                                     lv_obj_t **out_slider, lv_obj_t **out_val,
                                     lv_event_cb_t slider_cb, lv_event_cb_t step_cb)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Label row: name left, value right */
    lv_obj_t *top = lv_obj_create(cont);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(top, name, &lv_font_montserrat_36, UI_THEME_COLOR(text_color));
    lv_obj_t *val = lv_label_create(top);
    lv_label_set_text_fmt(val, "%d%%", value);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_48, 0);
    ui_set_theme_text_color(val, UI_THEME_COLOR(header_text_color));

    /* Control row: [-] [slider] [+] */
    lv_obj_t *ctl = lv_obj_create(cont);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_width(ctl, LV_PCT(100));
    lv_obj_set_height(ctl, 120);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctl, 20, 0);
    lv_obj_clear_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_minus = lv_button_create(ctl);
    lv_obj_set_size(btn_minus, HUB_BTN_SZ, HUB_BTN_SZ);
    lv_obj_set_style_radius(btn_minus, 14, 0);
    lv_obj_set_style_bg_opa(btn_minus, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_minus, 0, 0);
    lv_obj_set_style_shadow_width(btn_minus, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn_minus, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn_minus, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_t *lm = ui_label(btn_minus, LV_SYMBOL_MINUS, &lv_font_montserrat_36, UI_THEME_COLOR(text_color));
    lv_obj_center(lm);
    lv_obj_add_event_cb(btn_minus, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-5);

    lv_obj_t *slider = lv_slider_create(ctl);
    lv_obj_set_height(slider, 24);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    /* 72 px knob via padding (24 track + 2 x 24 pad) — cold-finger target */
    lv_obj_set_style_pad_all(slider, 24, LV_PART_KNOB);
    if (current_theme) {
        lv_obj_set_style_bg_color(slider, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(slider, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(current_theme->progress_color), LV_PART_KNOB);
    }
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *btn_plus = lv_button_create(ctl);
    lv_obj_set_size(btn_plus, HUB_BTN_SZ, HUB_BTN_SZ);
    lv_obj_set_style_radius(btn_plus, 14, 0);
    lv_obj_set_style_bg_opa(btn_plus, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_plus, 0, 0);
    lv_obj_set_style_shadow_width(btn_plus, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn_plus, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn_plus, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_t *lp = ui_label(btn_plus, LV_SYMBOL_PLUS, &lv_font_montserrat_36, UI_THEME_COLOR(text_color));
    lv_obj_center(lp);
    lv_obj_add_event_cb(btn_plus, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)5);

    if (out_slider) {
        *out_slider = slider;
    }
    if (out_val) {
        *out_val = val;
    }
    return cont;
}

static int hub_clamp_pct(int v)
{
    if (v < 5) {
        v = 5;
    }
    if (v > 100) {
        v = 100;
    }
    return v;
}

/* SCREEN backlight: hardware call is cheap — apply per drag tick, persist on
 * release only (NVS blob save is ~350 ms; the deferred path debounces). */
static void bl_slider_cb(lv_event_t *e)
{
    if (!s_bl_slider) {
        return;
    }
    int v = lv_slider_get_value(s_bl_slider);
    bsp_display_brightness_set(v);
    if (s_bl_val) {
        lv_label_set_text_fmt(s_bl_val, "%d%%", v);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_config_t *snap = hub_snap_begin();
        if (snap) {
            snap->brightness = v;
            hub_snap_commit(snap);
        }
    }
}

static void bl_step_cb(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int v = hub_clamp_pct(app_config_get()->brightness + step);
    if (s_bl_slider) {
        lv_slider_set_value(s_bl_slider, v, LV_ANIM_OFF);
    }
    bsp_display_brightness_set(v);
    if (s_bl_val) {
        lv_label_set_text_fmt(s_bl_val, "%d%%", v);
    }
    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->brightness = v;
        hub_snap_commit(snap);
    }
}

/* TEXT brightness: label only during drag (a full re-theme per tick
 * stutters); on release persist FIRST, then apply LAST — the re-theme
 * re-enters settings_hub_apply_theme and rebuilds this screen. */
static void tb_slider_cb(lv_event_t *e)
{
    if (!s_tb_slider) {
        return;
    }
    int v = lv_slider_get_value(s_tb_slider);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        if (s_tb_val) {
            lv_label_set_text_fmt(s_tb_val, "%d%%", v);
        }
        return;
    }
    /* RELEASED */
    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->color_brightness = v;
        hub_snap_commit(snap);
    }
    int theme_idx = app_config_get()->theme_index;
    /* LAST statement — rebuilds this screen; no widget access after. */
    nina_dashboard_apply_theme(theme_idx);
}

static void tb_step_cb(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int v = hub_clamp_pct(app_config_get()->color_brightness + step);
    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->color_brightness = v;
        hub_snap_commit(snap);
    }
    int theme_idx = app_config_get()->theme_index;
    /* LAST statement — rebuilds this screen; no widget access after. */
    nina_dashboard_apply_theme(theme_idx);
}

static void build_brightness_screen(lv_obj_t *parent)
{
    const app_config_t *cfg = app_config_get();

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 32, 0);

    settings_hub_make_header(parent, "BRIGHTNESS");

    int bl = hub_clamp_pct(cfg->brightness);
    int tb = hub_clamp_pct(cfg->color_brightness);
    hub_make_bright_row(parent, "SCREEN", bl, &s_bl_slider, &s_bl_val, bl_slider_cb, bl_step_cb);
    hub_make_bright_row(parent, "TEXT", tb, &s_tb_slider, &s_tb_val, tb_slider_cb, tb_step_cb);

    lv_obj_t *hint = ui_label(parent, "Live preview: the panel shows the change as you drag",
                              &lv_font_montserrat_24, UI_THEME_COLOR(label_color));
    lv_obj_set_style_pad_left(hint, 4, 0);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Pages — MANUAL / HOME PAGE / CYCLE tabs under one segmented control
 * ════════════════════════════════════════════════════════════════════════ */

/* True if a registry entry belongs in the Home Page picker and the Cycle
 * chip grid (same filter as the web UI: targetable pages and image sources,
 * no overlays/settings). */
static bool home_entry_is_listed(const page_ref_entry_t *e)
{
    if (e == NULL || !e->targetable) {
        return false;
    }
    return (e->kind == PAGE_REF_KIND_PAGE ||
            e->kind == PAGE_REF_KIND_IMAGE_SOURCE);
}

static void pages_mode_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!s_seg_mode) {
        return;
    }
    uint32_t sel = lv_buttonmatrix_get_selected_button(s_seg_mode);
    if (sel > 2) {
        return;
    }

    app_config_t *snap = hub_snap_begin();
    if (snap) {
        if (sel == 2) {
            /* Cycle */
            snap->auto_rotate_enabled = true;
            snap->idle_page_override_enabled = false;
        } else {
            /* Manual / Home Page */
            snap->auto_rotate_enabled = false;
        }
        /* Commit runs app_config_normalize_nav_exclusivity — no manual call. */
        hub_snap_commit(snap);
    }

    if (sel == 1) {
        /* Takes effect when Settings closes (modal holds the page until
         * then). Safe from this event callback — page_ref_navigate does not
         * take the LVGL lock. */
        page_ref_navigate((page_ref_t)app_config_get()->active_page_override);
    }

    s_pages_tab = (int)sel;
    /* LAST statement: the rebuild deletes the buttonmatrix this event came
     * from — no widget access after. Rebuild also re-checks the selected
     * segment, so no manual one-checked enforcement is needed. */
    settings_hub_goto(HUB_SCREEN_PAGES);
}

/* ── HOME PAGE tab: tappable current-value row + full-screen picker ── */

static void home_pick_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* LAST statement — the rebuild deletes the picker overlay. */
    settings_hub_goto(HUB_SCREEN_PAGES);
}

static void home_pick_row_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);

    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->active_page_override = (int8_t)id;
        hub_snap_commit(snap);
    }
    /* Takes effect when Settings closes (modal holds the page until then);
     * existing Home Page semantics (USER claim). */
    page_ref_navigate((page_ref_t)id);
    /* LAST statement — rebuild returns to the Pages screen showing the new
     * value (and deletes the picker overlay this row lives in). */
    settings_hub_goto(HUB_SCREEN_PAGES);
}

static void pages_home_row_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_home_picker || !s_screen) {
        return;
    }
    int current = app_config_get()->active_page_override;

    /* Full-screen picker over the Pages screen (same floating-overlay
     * pattern as the factory-reset confirm). */
    s_home_picker = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_home_picker);
    lv_obj_set_size(s_home_picker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_home_picker, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_home_picker,
        lv_color_hex(current_theme ? current_theme->bg_main : 0x000000), 0);
    lv_obj_add_flag(s_home_picker, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */
    lv_obj_add_flag(s_home_picker, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_home_picker, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_home_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_home_picker, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_home_picker, 12, 0);

    /* Header — BACK returns to the Pages screen, not the hub. */
    lv_obj_t *head = lv_obj_create(s_home_picker);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), HUB_HEADER_H);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_button_create(head);
    lv_obj_set_size(btn, SCREEN_ROUND ? HUB_BACK_W_ROUND : HUB_BACK_W, HUB_HEADER_H);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    /* Built after settings_hub_round_fit() has already run for this screen
     * (the picker is a tap-triggered overlay, not part of the Pages screen's
     * own tree), so the round sweep can never reach this label: raise it here
     * with the one ternary the addendum's rule 3 allows. Square is the
     * shipped montserrat_24, unchanged. */
    lv_obj_t *lbl_back = ui_label(btn, LV_SYMBOL_LEFT " BACK",
                                  SCREEN_ROUND ? &lv_font_montserrat_28 : &lv_font_montserrat_24,
                                  UI_THEME_COLOR(text_color));
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn, home_pick_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = ui_label(head, "HOME PAGE", &lv_font_montserrat_36,
                               UI_THEME_COLOR(header_text_color));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Scrollable 88 px rows — every eligible page, the current one checked. */
    lv_obj_t *list = lv_obj_create(s_home_picker);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int count = page_ref_count();
    for (int i = 0; i < count; i++) {
        const page_ref_entry_t *pe = page_ref_get(i);
        if (!home_entry_is_listed(pe)) {
            continue;
        }
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, LV_PCT(100), 88);
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
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(row, 24, 0);
        lv_obj_set_style_pad_right(row, 24, 0);

        ui_label(row, pe->label, &lv_font_montserrat_28, UI_THEME_COLOR(text_color));
        if ((int)pe->id == current) {
            ui_label(row, LV_SYMBOL_OK, &lv_font_montserrat_28, UI_THEME_COLOR(header_text_color));
        }
        lv_obj_add_event_cb(row, home_pick_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)pe->id);
    }
}

/* ── CYCLE tab: on/off row + membership chip grid ── */

static void cycle_toggle_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bool want = !app_config_get()->auto_rotate_enabled;

    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->auto_rotate_enabled = want;
        if (want) {
            snap->idle_page_override_enabled = false;
        }
        /* Commit runs app_config_normalize_nav_exclusivity — no manual call. */
        hub_snap_commit(snap);
    }
    if (s_cycle_val) {
        lv_label_set_text(s_cycle_val, want ? "ON" : "OFF");
    }
}

static void cycle_chip_cb(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_target(e);
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    /* CHECKABLE already flipped the widget state — mirror it into config. */
    bool want = lv_obj_has_state(chip, LV_STATE_CHECKED);

    app_config_t *snap = hub_snap_begin();
    if (snap) {
        hub_order2_set(snap->auto_rotate_order2, (uint8_t)id, want);
        hub_snap_commit(snap);
    }
    /* The arbiter re-reads auto_rotate_order2 every resolve tick — applies
     * live with no notify call. */
}

static void build_pages_screen(lv_obj_t *parent)
{
    const app_config_t *cfg = app_config_get();
    int gb = cfg->color_brightness;

    if (s_pages_tab < 0 || s_pages_tab > 2) {
        /* First visit this hub session: cycling or pinned Home Page
         * (MANUAL is never the derived initial tab). */
        s_pages_tab = cfg->auto_rotate_enabled ? 2 : 1;
    }

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 24, 0);

    settings_hub_make_header(parent, "PAGES");

    /* ── Segmented tab selector (buttonmatrix, one row, 96 px) ── */
    s_seg_mode = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(s_seg_mode, s_seg_map);
    lv_obj_set_width(s_seg_mode, LV_PCT(100));
    lv_obj_set_height(s_seg_mode, HUB_ROW_H);
    /* Never let the matrix take FOCUSED: LVGL style lookup prefers the
     * numerically higher state, so the ITEMS|FOCUSED suppression styles
     * below would outrank ITEMS|CHECKED and blank the selected pill the
     * moment the user taps. */
    lv_obj_clear_flag(s_seg_mode, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    if (current_theme) {
        /* Main container (the background strip) */
        lv_obj_set_style_bg_color(s_seg_mode, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_seg_mode, 10, 0);
        lv_obj_set_style_border_width(s_seg_mode, 0, 0);
        lv_obj_set_style_outline_width(s_seg_mode, 0, 0);
        lv_obj_set_style_pad_all(s_seg_mode, 4, 0);
        lv_obj_set_style_pad_gap(s_seg_mode, 4, 0);
        lv_obj_set_style_text_font(s_seg_mode, &lv_font_montserrat_28, 0);

        /* Unchecked items — transparent bg, theme text color */
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS);
        lv_obj_set_style_border_width(s_seg_mode, 0, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(s_seg_mode, 0, LV_PART_ITEMS);
        lv_obj_set_style_outline_width(s_seg_mode, 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(s_seg_mode, 8, LV_PART_ITEMS);
        lv_obj_set_style_text_color(s_seg_mode,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), LV_PART_ITEMS);
        lv_obj_set_style_text_font(s_seg_mode, &lv_font_montserrat_28, LV_PART_ITEMS);

        /* Checked item — progress_color bg */
        lv_obj_set_style_bg_color(s_seg_mode, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(s_seg_mode,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
            LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(s_seg_mode, 0, LV_PART_ITEMS | LV_STATE_CHECKED);

        /* Checked must stay visible through interaction: LVGL picks the
         * style whose state has the numerically HIGHER value, so any of
         * PRESSED/FOCUSED/FOCUS_KEY ORed onto CHECKED outranks the plain
         * CHECKED style above. Restate the accent cover for each combo. */
        lv_obj_set_style_bg_color(s_seg_mode, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s_seg_mode, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(s_seg_mode, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);

        /* Suppress all other visual states that could show a false highlight */
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(s_seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(s_seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(s_seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(s_seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_opa(s_seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(s_seg_mode, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
    }
    lv_obj_add_event_cb(s_seg_mode, pages_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_buttonmatrix_set_button_ctrl(s_seg_mode, (uint32_t)s_pages_tab,
                                    LV_BUTTONMATRIX_CTRL_CHECKED);

    /* ── Tab content ── */
    if (s_pages_tab == 0) {
        /* MANUAL */
        lv_obj_t *info = ui_label(parent,
            "Pages change only when you swipe or use the button.",
            &lv_font_montserrat_28, UI_THEME_COLOR(text_color));
        lv_obj_set_width(info, LV_PCT(100));
        lv_obj_set_style_pad_left(info, 4, 0);
    } else if (s_pages_tab == 1) {
        /* HOME PAGE — tappable current value opens the full-screen picker */
        const page_ref_entry_t *pe = page_ref_by_id((page_ref_t)cfg->active_page_override);
        lv_obj_t *val = NULL;
        hub_make_more_row(parent, "HOME PAGE", &val, pages_home_row_cb);
        if (val) {
            char vbuf[48];
            snprintf(vbuf, sizeof(vbuf), "%.24s  " LV_SYMBOL_RIGHT,
                     pe ? pe->label : "Summary");
            lv_label_set_text(val, vbuf);
        }
        lv_obj_t *hint = ui_label(parent, "Tap to choose the page shown by default.",
                                  &lv_font_montserrat_24, UI_THEME_COLOR(label_color));
        lv_obj_set_style_pad_left(hint, 4, 0);
    } else {
        /* CYCLE — on/off row + membership chips */
        hub_make_more_row(parent, "CYCLE PAGES", &s_cycle_val, cycle_toggle_cb);
        lv_label_set_text(s_cycle_val, cfg->auto_rotate_enabled ? "ON" : "OFF");

        lv_obj_t *chips = lv_obj_create(parent);
        lv_obj_remove_style_all(chips);
        lv_obj_set_width(chips, LV_PCT(100));
        lv_obj_set_flex_grow(chips, 1);
        lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(chips, 12, 0);
        lv_obj_set_style_pad_row(chips, 12, 0);
        lv_obj_set_scroll_dir(chips, LV_DIR_VER);

        int count = page_ref_count();
        for (int i = 0; i < count; i++) {
            const page_ref_entry_t *pe = page_ref_get(i);
            if (!home_entry_is_listed(pe)) {
                continue;
            }
            lv_obj_t *chip = lv_button_create(chips);
            lv_obj_set_size(chip, LV_SIZE_CONTENT, 56);
            lv_obj_set_style_pad_left(chip, 20, 0);
            lv_obj_set_style_pad_right(chip, 20, 0);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(chip, 1, 0);
            lv_obj_set_style_shadow_width(chip, 0, 0);
            if (current_theme) {
                lv_obj_set_style_bg_color(chip, lv_color_hex(current_theme->bento_bg), 0);
                lv_obj_set_style_border_color(chip, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_text_color(chip,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
                lv_obj_set_style_bg_color(chip, lv_color_hex(current_theme->progress_color),
                                          LV_STATE_CHECKED);
                lv_obj_set_style_text_color(chip, lv_color_hex(current_theme->bg_main),
                                            LV_STATE_CHECKED);
                lv_obj_set_style_border_width(chip, 0, LV_STATE_CHECKED);
            }
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CHECKABLE);
            if (hub_order2_contains(cfg->auto_rotate_order2, pe->id)) {
                lv_obj_add_state(chip, LV_STATE_CHECKED);
            }
            /* No local text color on the label: it inherits the chip's
             * per-state text_color (default vs CHECKED). */
            lv_obj_t *cl = lv_label_create(chip);
            lv_label_set_text(cl, pe->label);
            lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
            lv_obj_center(cl);
            lv_obj_add_event_cb(chip, cycle_chip_cb, LV_EVENT_VALUE_CHANGED,
                                (void *)(intptr_t)pe->id);
        }

        lv_obj_t *hint = ui_label(parent, "Order and speed: web UI",
                                  &lv_font_montserrat_24, UI_THEME_COLOR(label_color));
        lv_obj_set_style_pad_left(hint, 4, 0);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  More — rotate, reboot, factory reset, info, web handoff
 * ════════════════════════════════════════════════════════════════════════ */

/* One 96 px action row: title label left, optional value label right.
 * Returns the row button; *out_value receives the right-hand label. */
static lv_obj_t *hub_make_more_row(lv_obj_t *parent, const char *title,
                                   lv_obj_t **out_value, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), HUB_ROW_H);
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
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 24, 0);
    lv_obj_set_style_pad_right(row, 24, 0);

    ui_label(row, title, &lv_font_montserrat_28, UI_THEME_COLOR(text_color));

    lv_obj_t *val = NULL;
    if (out_value) {
        val = ui_label(row, "", &lv_font_montserrat_28, UI_THEME_COLOR(label_color));
        *out_value = val;
    }
    if (cb) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    }
    return row;
}

static void more_rotate_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int v = (app_config_get()->screen_rotation + 1) % 4;

    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->screen_rotation = (uint8_t)v;
        hub_snap_commit(snap);
    }
    /* Already on the LVGL port task with the display lock held — do not re-take. */
    display_rotation_apply(v);
    if (s_rotate_val) {
        lv_label_set_text_fmt(s_rotate_val, "%d\xc2\xb0", v * 90);
    }
}

/* MUTE ALL SOUNDS — this row only owns the audio_muted config field; the
 * structural playback gate lives in audio_alert's enqueue(). */
static void more_mute_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bool want = !app_config_get()->audio_muted;

    app_config_t *snap = hub_snap_begin();
    if (snap) {
        snap->audio_muted = want;
        hub_snap_commit(snap);
    }
    if (s_mute_val) {
        lv_label_set_text(s_mute_val, want ? "ON" : "OFF");
    }
}

static void reboot_disarm_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_arm_timer = NULL;
    s_reboot_armed = false;
    if (s_reboot_lbl) {
        lv_label_set_text(s_reboot_lbl, "");
    }
}

static void more_reboot_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!s_reboot_armed) {
        /* First tap arms a 5 s confirm window on the row itself. */
        s_reboot_armed = true;
        if (s_reboot_lbl) {
            lv_label_set_text(s_reboot_lbl, "Tap again to reboot");
        }
        if (s_arm_timer) {
            lv_timer_delete(s_arm_timer);
        }
        s_arm_timer = lv_timer_create(reboot_disarm_timer_cb, 5000, NULL);
        lv_timer_set_repeat_count(s_arm_timer, 1);
        return;
    }
    /* An in-flight deferred save is lost across reboot — flush first. */
    app_config_flush_deferred();
    app_reboot("panel reboot");
}

static void fr_enable_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_fr_timer = NULL;
    if (s_fr_yes) {
        lv_obj_remove_state(s_fr_yes, LV_STATE_DISABLED);
    }
}

static void fr_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_fr_timer) {
        lv_timer_delete(s_fr_timer);
        s_fr_timer = NULL;
    }
    s_fr_yes = NULL;
    if (s_fr_overlay) {
        lv_obj_delete(s_fr_overlay);
        s_fr_overlay = NULL;
    }
}

static void fr_confirm_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_config_factory_reset();
    app_reboot("panel factory reset");
}

static void more_factory_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_fr_overlay || !s_screen) {
        return;
    }

    /* Full-screen confirm sub-state over the More screen. */
    s_fr_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_fr_overlay);
    lv_obj_set_size(s_fr_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_fr_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_fr_overlay,
        lv_color_hex(current_theme ? current_theme->bg_main : 0x000000), 0);
    lv_obj_add_flag(s_fr_overlay, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */
    /* The More screen is a flex column — float the overlay out of the flow. */
    lv_obj_add_flag(s_fr_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_fr_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_fr_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_fr_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_fr_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_fr_overlay, 28, 0);
    lv_obj_set_style_pad_all(s_fr_overlay, 24, 0);

    lv_obj_t *title = lv_label_create(s_fr_overlay);
    lv_label_set_text(title, "FACTORY RESET");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(HUB_DANGER_COLOR), 0);

    lv_obj_t *warn = ui_label(s_fr_overlay,
                              "This erases ALL settings\nand reboots the device.",
                              &lv_font_montserrat_28, UI_THEME_COLOR(text_color));
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);

    s_fr_yes = lv_button_create(s_fr_overlay);
    lv_obj_set_size(s_fr_yes, LV_PCT(90), HUB_ROW_H);
    lv_obj_set_style_radius(s_fr_yes, 14, 0);
    lv_obj_set_style_bg_opa(s_fr_yes, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_fr_yes, lv_color_hex(HUB_DANGER_COLOR), 0);
    lv_obj_set_style_bg_opa(s_fr_yes, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(s_fr_yes, 0, 0);
    lv_obj_set_style_shadow_width(s_fr_yes, 0, 0);
    lv_obj_t *yes_lbl = lv_label_create(s_fr_yes);
    lv_label_set_text(yes_lbl, "YES, ERASE EVERYTHING");
    lv_obj_set_style_text_font(yes_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(yes_lbl, lv_color_white(), 0);
    lv_obj_center(yes_lbl);
    lv_obj_add_state(s_fr_yes, LV_STATE_DISABLED);   /* 3 s gate below enables it */
    lv_obj_add_event_cb(s_fr_yes, fr_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = lv_button_create(s_fr_overlay);
    lv_obj_set_size(cancel, LV_PCT(90), HUB_ROW_H);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(cancel, lv_color_hex(current_theme->bento_border), 0);
    }
    lv_obj_t *cancel_lbl = ui_label(cancel, "CANCEL", &lv_font_montserrat_28,
                                    UI_THEME_COLOR(text_color));
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel, fr_cancel_cb, LV_EVENT_CLICKED, NULL);

    if (s_fr_timer) {
        lv_timer_delete(s_fr_timer);
    }
    s_fr_timer = lv_timer_create(fr_enable_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_fr_timer, 1);
}

/* Compact read-only "KEY   value" info row for the More screen. */
static void hub_make_info_row(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    /* 40 px for a one-line value; a value that wraps (a dev build's version
     * tag on the 564 px round chord) grows the row instead of being cut. */
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 40, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 24, 0);
    lv_obj_set_style_pad_right(row, 24, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(row, key, &lv_font_montserrat_24, UI_THEME_COLOR(label_color));
    /* The value takes whatever the key leaves and wraps inside it: a dev
     * build's "snd-alpha-76-ge896e32-dirty" at 32 px ran back over the
     * VERSION key on the 564 px round chord (bench B12 on the 3.4C). */
    lv_obj_t *val = ui_label(row, value, &lv_font_montserrat_32, UI_THEME_COLOR(text_color));
    lv_obj_set_flex_grow(val, 1);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_more_screen(lv_obj_t *parent)
{
    const app_config_t *cfg = app_config_get();
    char buf[64];

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 12, 0);
    /* Four action rows + the info block exceed 688 px — this screen scrolls. */
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    settings_hub_make_header(parent, "MORE");

    /* ROTATE SCREEN — tap cycles 0/90/180/270, applies live */
    hub_make_more_row(parent, "ROTATE SCREEN", &s_rotate_val, more_rotate_cb);
    lv_label_set_text_fmt(s_rotate_val, "%d\xc2\xb0", (int)cfg->screen_rotation * 90);

    /* REBOOT — two-tap inline confirm */
    hub_make_more_row(parent, "REBOOT", &s_reboot_lbl, more_reboot_cb);

    /* MUTE ALL SOUNDS — toggle, reflects cfg->audio_muted */
    hub_make_more_row(parent, "MUTE ALL SOUNDS", &s_mute_val, more_mute_cb);
    lv_label_set_text(s_mute_val, cfg->audio_muted ? "ON" : "OFF");

    /* FACTORY RESET — danger color, full-screen confirm with 3 s gate */
    lv_obj_t *fr_row = hub_make_more_row(parent, "FACTORY RESET", NULL, more_factory_cb);
    lv_obj_t *fr_lbl = lv_obj_get_child(fr_row, 0);
    if (fr_lbl) {
        lv_obj_set_style_text_color(fr_lbl, lv_color_hex(0xE05555), 0);
    }

    /* ── Read-only info block ── */
    const char *host = cfg->hostname[0] ? cfg->hostname : "NINA-DISPLAY";
    hub_make_info_row(parent, "HOSTNAME", host);

    {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
        } else {
            snprintf(buf, sizeof(buf), "Not connected");
        }
        hub_make_info_row(parent, "IP", buf);
    }

    hub_make_info_row(parent, "VERSION", ota_github_get_current_version());

    {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%d dBm", (int)ap.rssi);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        hub_make_info_row(parent, "WIFI RSSI", buf);
    }

    /* ── Web UI handoff — everything not on the panel lives there ── */
    lv_obj_t *hand = ui_label(parent, "Everything else:", &lv_font_montserrat_24,
                              UI_THEME_COLOR(label_color));
    lv_obj_set_style_pad_left(hand, 24, 0);
    snprintf(buf, sizeof(buf), "http://%.32s.lan/config", host);
    lv_obj_t *url = ui_label(parent, buf, &lv_font_montserrat_32, UI_THEME_COLOR(header_text_color));
    lv_obj_set_style_pad_left(url, 24, 0);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Dashboard contract — create / destroy / refresh / apply theme
 * ════════════════════════════════════════════════════════════════════════ */

lv_obj_t *settings_hub_create(lv_obj_t *parent)
{
    s_current   = HUB_SCREEN_HUB;
    s_screen    = NULL;
    s_pages_tab = -1;   /* re-derive the Pages tab from config each session */

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, HUB_ROOT_SIZE, HUB_ROOT_SIZE);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    settings_hub_goto(HUB_SCREEN_HUB);
    return s_root;
}

void settings_hub_destroy(void)
{
    hub_reset_screen_state();
    if (screen_is_wifi(s_current)) {
        wifi_join_set_notify_cb(NULL);
    }
    if (s_root) {
        lv_obj_delete(s_root);
    }
    s_root    = NULL;
    s_screen  = NULL;
    s_current = HUB_SCREEN_HUB;
}

void settings_hub_refresh(void)
{
    if (!s_root) {
        return;   /* hub not open — safe no-op (called from main.c GOT_IP) */
    }
    /* A GOT_IP mid-join must not clobber password typing or the result
     * screen; the join state machine owns those transitions. */
    if (s_current == HUB_SCREEN_WIFI_PASSWORD || s_current == HUB_SCREEN_WIFI_CONNECT) {
        return;
    }
    settings_hub_goto(s_current);   /* rebuild = refresh every status line */
}

void settings_hub_apply_theme(void)
{
    if (!s_root) {
        return;
    }
    /* Match the refresh guard: rebuilding the password screen would wipe the
     * user's typed input (theme/brightness can change mid-typing via web or
     * MQTT). A stale theme on that one transient screen is the lesser evil. */
    if (s_current == HUB_SCREEN_WIFI_PASSWORD) {
        return;
    }
    /* All colors are read from current_theme at build time — rebuild. */
    settings_hub_goto(s_current);
}
