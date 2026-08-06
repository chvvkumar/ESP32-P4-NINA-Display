/**
 * @file nina_setup_hint.c
 * @brief First-boot setup hint overlay (see nina_setup_hint.h).
 *
 * Layout is a single opaque 720x720 container, with absolutely aligned
 * children, created as the last child of the active screen, so it draws above
 * the dashboard and its overlays.
 * Palette and spacing follow nina_setup_screen.c, including the Red Night
 * substitution so the wizard-style screens never leak bright colors.
 */

#include "nina_setup_hint.h"
#include "nina_dashboard.h"      /* nina_dashboard_get_theme() */
#include "nina_nav_arbiter.h"
#include "app_config.h"
#include "themes.h"
#include "display_defs.h"
#include "bsp/esp-bsp.h"         /* bsp_display_lock / bsp_display_unlock */
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>            /* strcasecmp */

static const char *TAG = "setup_hint";

/** "http://" + "255.255.255.255" + NUL = 23 bytes; rounded up for headroom. */
#define HINT_URL_BUF_LEN 32

/** Factory hostname shown when the user has not chosen one. */
#define HINT_FACTORY_HOSTNAME "NINA-DISPLAY"

/* ── Widget state ────────────────────────────────────────────────────── */
static lv_obj_t *s_cont    = NULL;   /* fullscreen opaque container        */
static lv_obj_t *s_lbl_url = NULL;   /* URL label, refreshed on new IP     */
static bool      s_visible = false;  /* pairs modal open/close exactly once */
static bool      s_dismissed_boot = false; /* user dismissed; suppress until reboot */

/* ── Forward declarations ────────────────────────────────────────────── */
static bool hostname_is_factory(const char *hostname);
static bool build_sta_url(char *buf, size_t buf_len);
static void teardown_common(void);
static void cont_delete_cb(lv_event_t *e);
static void dismiss_btn_cb(lv_event_t *e);
static void never_again_btn_cb(lv_event_t *e);
static void build_overlay(const char *url, const char *hostname);

/* ── Helpers ─────────────────────────────────────────────────────────── */

/** True while the hostname is unset or still the factory default. */
static bool hostname_is_factory(const char *hostname)
{
    if (hostname == NULL || hostname[0] == '\0') {
        return true;
    }
    return strcasecmp(hostname, HINT_FACTORY_HOSTNAME) == 0;
}

/** Format the STA address as a browser URL. False when there is no STA IP. */
static bool build_sta_url(char *buf, size_t buf_len)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }
    snprintf(buf, buf_len, "http://" IPSTR, IP2STR(&ip_info.ip));
    return true;
}

/**
 * Drop the widget pointers and release the arbiter freeze exactly once.
 * Does NOT delete the LVGL object; each caller picks sync or async delete.
 */
static void teardown_common(void)
{
    s_cont    = NULL;
    s_lbl_url = NULL;
    if (s_visible) {
        s_visible = false;
        nav_arbiter_notify_modal_close(esp_timer_get_time() / 1000);
    }
}

/**
 * Safety net for any deletion of the container that did not go through one of
 * the teardown paths below (e.g. the screen being cleared). Without it the
 * arbiter would stay frozen and s_cont would dangle.
 *
 * The identity check makes the callback inert for a container that has already
 * been retired: every in-tree path calls teardown_common() (which NULLs s_cont)
 * before requesting the delete, so when the DELETE event finally arrives the
 * target no longer matches and nothing is double-counted.
 */
static void cont_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_cont) {
        return;   /* already torn down by the path that requested the delete */
    }
    teardown_common();
}

/* ── Button callbacks (run inside the LVGL task; lock already held) ───── */

/**
 * Dismiss for this boot only. The overlay is deleted asynchronously because
 * the click event originates inside the subtree being removed.
 */
static void dismiss_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_dismissed_boot = true;
    lv_obj_t *cont = s_cont;
    teardown_common();
    if (cont) {
        lv_obj_delete_async(cont);
    }
    ESP_LOGI(TAG, "Setup hint dismissed for this boot");
}

/**
 * Persist the dismissal. Uses the config snapshot pattern: a PSRAM copy is
 * taken, modified, and handed to app_config_save(); app_config_t is ~7.6 KB
 * and must never land on a task stack.
 */
static void never_again_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_dismissed_boot = true;

    app_config_t *snap = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (snap != NULL) {
        app_config_get_snapshot_into(snap);
        snap->setup_hint_dismissed = true;
        app_config_save(snap);
        heap_caps_free(snap);
        ESP_LOGI(TAG, "Setup hint disabled permanently");
    } else {
        ESP_LOGE(TAG, "No PSRAM for config snapshot; setup hint not persisted");
    }

    lv_obj_t *cont = s_cont;
    teardown_common();
    if (cont) {
        lv_obj_delete_async(cont);
    }
}

/* ── Overlay construction ────────────────────────────────────────────── */

/**
 * Build the overlay on the active screen. Caller holds the display lock and
 * has already verified that no overlay exists.
 *
 * Absolute layout (720 px tall): title at top (+90), URL centered (-80) with
 * the hostname caption (-15) and value (+25) below it, help line and button
 * row anchored to the bottom (-170 and -60).
 */
static void build_overlay(const char *url, const char *hostname)
{
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        return;
    }

    /* Red Night keeps the panel monochrome red; every other theme uses the
     * same white/gray/blue palette as the WiFi setup wizard. */
    const theme_t *t = nina_dashboard_get_theme();
    bool red = (t && theme_is_red_night(t));

    uint32_t col_title  = red ? t->text_color      : 0xFFFFFF;
    uint32_t col_url    = red ? t->text_color      : 0x3b82f6;
    uint32_t col_host   = red ? t->label_color     : 0xCCCCCC;
    uint32_t col_help   = red ? t->label_color     : 0x999999;
    uint32_t col_border = red ? t->bento_border    : 0x555555;
    uint32_t col_accent = red ? t->progress_color  : 0x3b82f6;

    s_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cont);
    lv_obj_set_size(s_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(s_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_cont, 0, 0);
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_cont);
    lv_obj_add_event_cb(s_cont, cont_delete_cb, LV_EVENT_DELETE, NULL);

    /* Title */
    lv_obj_t *title = lv_label_create(s_cont);
    lv_label_set_text(title, "Ready to set up");
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(col_title), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 90);

    /* URL — dominant element */
    s_lbl_url = lv_label_create(s_cont);
    lv_label_set_text(s_lbl_url, url);
    lv_obj_set_width(s_lbl_url, lv_pct(100));
    lv_obj_set_style_text_font(s_lbl_url, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_url, lv_color_hex(col_url), 0);
    lv_obj_set_style_text_align(s_lbl_url, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_url, LV_ALIGN_CENTER, 0, -80);

    /* Hostname caption */
    lv_obj_t *host_cap = lv_label_create(s_cont);
    lv_label_set_text(host_cap, "Hostname");
    lv_obj_set_width(host_cap, lv_pct(100));
    lv_obj_set_style_text_font(host_cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(host_cap, lv_color_hex(col_help), 0);
    lv_obj_set_style_text_align(host_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(host_cap, LV_ALIGN_CENTER, 0, -15);

    /* Hostname */
    lv_obj_t *host = lv_label_create(s_cont);
    lv_label_set_text(host, hostname);
    lv_obj_set_width(host, lv_pct(100));
    lv_obj_set_style_text_font(host, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(host, lv_color_hex(col_host), 0);
    lv_obj_set_style_text_align(host, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(host, LV_ALIGN_CENTER, 0, 25);

    /* Help line */
    lv_obj_t *help = lv_label_create(s_cont);
    lv_label_set_text(help, "Open the address in a browser on this network to configure the display");
    lv_obj_set_width(help, lv_pct(88));
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(help, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(help, lv_color_hex(col_help), 0);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(help, 6, 0);
    lv_obj_align(help, LV_ALIGN_BOTTOM_MID, 0, -170);

    /* Button row: 240 + 32 gap + 240 = 512 px inside a 720 px content box */
    lv_obj_t *btn_row = lv_obj_create(s_cont);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 72);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 32, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, -60);

    /* Dismiss — ghost outline */
    lv_obj_t *btn_dismiss = lv_button_create(btn_row);
    lv_obj_set_size(btn_dismiss, 240, 72);
    lv_obj_set_style_radius(btn_dismiss, 14, 0);
    lv_obj_set_style_bg_opa(btn_dismiss, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_dismiss, 1, 0);
    lv_obj_set_style_border_color(btn_dismiss, lv_color_hex(col_border), 0);
    lv_obj_set_style_border_opa(btn_dismiss, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn_dismiss, 0, 0);
    lv_obj_set_style_bg_color(btn_dismiss, lv_color_hex(col_border), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_dismiss, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_dismiss, dismiss_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_dismiss = lv_label_create(btn_dismiss);
    lv_label_set_text(lbl_dismiss, "Dismiss");
    lv_obj_set_style_text_font(lbl_dismiss, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_dismiss, lv_color_hex(col_title), 0);
    lv_obj_center(lbl_dismiss);

    /* Don't show again — solid accent */
    lv_obj_t *btn_never = lv_button_create(btn_row);
    lv_obj_set_size(btn_never, 240, 72);
    lv_obj_set_style_radius(btn_never, 14, 0);
    lv_obj_set_style_bg_color(btn_never, lv_color_hex(col_accent), 0);
    lv_obj_set_style_bg_opa(btn_never, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_never, 0, 0);
    lv_obj_set_style_shadow_width(btn_never, 0, 0);
    lv_obj_add_event_cb(btn_never, never_again_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_never = lv_label_create(btn_never);
    lv_label_set_text(lbl_never, "Don't show again");
    lv_obj_set_style_text_font(lbl_never, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_never, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl_never);

    s_visible = true;
    nav_arbiter_notify_modal_open();
    ESP_LOGI(TAG, "Setup hint shown (%s)", url);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void nina_setup_hint_show_if_needed(void)
{
    if (s_dismissed_boot) {
        return;   /* user closed it; stays closed until reboot */
    }

    const app_config_t *cfg = app_config_get();
    if (cfg == NULL || cfg->setup_hint_dismissed) {
        return;
    }
    if (!hostname_is_factory(cfg->hostname)) {
        return;
    }

    char url[HINT_URL_BUF_LEN];
    if (!build_sta_url(url, sizeof(url))) {
        return;   /* no STA address yet */
    }

    /* The got-IP event can land before the display is up (wifi_init runs
     * before bsp_display_start_with_config); taking the lock then would trip
     * the esp_lvgl_port assert. app_main re-invokes this after the dashboard
     * is built, so skipping here loses nothing. */
    if (!lv_is_initialized()) {
        return;
    }

    if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Display lock timeout; setup hint not updated");
        return;
    }

    /* Re-check the dismiss state now that the lock is held: waiting for the
     * lock can take up to LVGL_LOCK_TIMEOUT_MS, long enough for the user to tap
     * Dismiss (or for a web save to retire the hint) in between. Without this
     * the overlay would be rebuilt right after it was torn down. */
    if (s_dismissed_boot) {
        bsp_display_unlock();
        return;
    }
    cfg = app_config_get();
    if (cfg == NULL || cfg->setup_hint_dismissed) {
        bsp_display_unlock();
        return;
    }

    if (s_cont != NULL) {
        /* Already up: refresh the address and keep it above anything that was
         * created after it (dashboard build, toasts, overlays). */
        if (s_lbl_url != NULL) {
            lv_label_set_text(s_lbl_url, url);
        }
        lv_obj_move_foreground(s_cont);
    } else {
        const char *hostname = (cfg->hostname[0] != '\0') ? cfg->hostname
                                                          : HINT_FACTORY_HOSTNAME;
        build_overlay(url, hostname);
    }

    bsp_display_unlock();
}

void nina_setup_hint_destroy(void)
{
    /* Called from the httpd task after a web config save, so the display lock
     * is taken here (same pattern as the theme/brightness handlers). */
    if (!lv_is_initialized()) {
        return;
    }

    s_dismissed_boot = true;   /* suppress a rebuild from a pending got-IP call */

    if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Display lock timeout; setup hint not destroyed");
        return;
    }

    lv_obj_t *cont = s_cont;
    if (cont == NULL && !s_visible) {
        bsp_display_unlock();
        return;   /* nothing showing */
    }

    /* teardown_common() first: it NULLs s_cont, so the LV_EVENT_DELETE callback
     * fired by lv_obj_delete() below sees a non-matching target and stays inert.
     * Exactly one modal_close per show. A synchronous delete is correct here
     * because this runs on the httpd task, not inside an LVGL event. */
    teardown_common();
    if (cont != NULL) {
        lv_obj_delete(cont);
    }
    bsp_display_unlock();
    ESP_LOGI(TAG, "Setup hint destroyed");
}
