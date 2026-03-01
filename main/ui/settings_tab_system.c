/**
 * @file settings_tab_system.c
 * @brief System tab for the settings tabview — Network, MQTT, Firmware, and Device cards.
 *
 * Provides configuration for WiFi/NTP/timezone, MQTT Home Assistant integration,
 * read-only firmware info with update controls, and device management (debug,
 * reboot, factory reset).
 */

#include "settings_tab_system.h"
#include "nina_settings_tabview.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include "ui_styles.h"
#include "lvgl.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"

#include <stdio.h>
#include <string.h>

/* ── OTA trigger from tasks.c ────────────────────────────────────────── */
extern volatile bool ota_check_requested;

/* ── Timezone Data ───────────────────────────────────────────────────── */
static const char *tz_names =
    "UTC\nGMT\nUK (GMT/BST)\nCentral Europe\nEastern Europe\n"
    "US Eastern\nUS Central\nUS Mountain\nUS Arizona\nUS Pacific\n"
    "US Alaska\nUS Hawaii\nAustralia Eastern\nAustralia Central\n"
    "Australia Western\nNew Zealand\nJapan\nChina\nIndia\nArgentina\n"
    "Brazil\nSouth Africa";

static const char *tz_values[] = {
    "", "GMT0", "GMT0BST,M3.5.0/1,M10.5.0",
    "CET-1CEST,M3.5.0,M10.5.0/3", "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0",
    "MST7MDT,M3.2.0,M11.1.0", "MST7", "PST8PDT,M3.2.0,M11.1.0",
    "AKST9AKDT,M3.2.0,M11.1.0", "HST10",
    "AEST-10AEDT,M10.1.0,M4.1.0/3", "ACST-9:30ACDT,M10.1.0,M4.1.0/3",
    "AWST-8", "NZST-12NZDT,M9.5.0,M4.1.0/3",
    "JST-9", "CST-8", "IST-5:30", "ART3", "BRT3BRST,M10.3.0/0,M2.3.0/0",
    "SAST-2"
};
#define TZ_COUNT 22

/* ── Static Widget References ────────────────────────────────────────── */
static lv_obj_t *tab_root = NULL;

/* Network card */
static lv_obj_t *ta_ssid       = NULL;
static lv_obj_t *ta_password   = NULL;
static lv_obj_t *ta_ntp        = NULL;
static lv_obj_t *dd_timezone   = NULL;

/* MQTT card */
static lv_obj_t *sw_mqtt_enabled = NULL;
static lv_obj_t *cont_mqtt_fields = NULL;
static lv_obj_t *ta_mqtt_broker  = NULL;
static lv_obj_t *lbl_mqtt_port   = NULL;
static lv_obj_t *ta_mqtt_prefix  = NULL;
static lv_obj_t *ta_mqtt_user    = NULL;
static lv_obj_t *ta_mqtt_pass    = NULL;

/* Firmware card */
static lv_obj_t *lbl_fw_version    = NULL;
static lv_obj_t *lbl_fw_built      = NULL;
static lv_obj_t *lbl_fw_idf        = NULL;
static lv_obj_t *lbl_fw_partition  = NULL;
static lv_obj_t *sw_auto_update    = NULL;
static lv_obj_t *dd_update_channel = NULL;
static lv_obj_t *btn_check_update  = NULL;
static lv_obj_t *lbl_check_update  = NULL;

/* Device card */
static lv_obj_t *sw_debug_mode = NULL;

/* ── Forward Declarations ────────────────────────────────────────────── */
static void ssid_defocus_cb(lv_event_t *e);
static void password_defocus_cb(lv_event_t *e);
static void ntp_defocus_cb(lv_event_t *e);
static void timezone_change_cb(lv_event_t *e);
static void mqtt_enabled_cb(lv_event_t *e);
static void mqtt_broker_defocus_cb(lv_event_t *e);
static void mqtt_port_minus_cb(lv_event_t *e);
static void mqtt_port_plus_cb(lv_event_t *e);
static void mqtt_prefix_defocus_cb(lv_event_t *e);
static void mqtt_user_defocus_cb(lv_event_t *e);
static void mqtt_pass_defocus_cb(lv_event_t *e);
static void auto_update_cb(lv_event_t *e);
static void update_channel_cb(lv_event_t *e);
static void check_update_btn_cb(lv_event_t *e);
static void debug_mode_cb(lv_event_t *e);
static void reboot_btn_cb(lv_event_t *e);
static void confirm_reboot_cb(lv_event_t *e);
static void factory_reset_btn_cb(lv_event_t *e);
static void confirm_factory_reset_cb(lv_event_t *e);

/* ── Helpers ─────────────────────────────────────────────────────────── */

/** Find the timezone index matching the current TZ string in config. */
static int find_tz_index(const char *tz_str) {
    for (int i = 0; i < TZ_COUNT; i++) {
        if (strcmp(tz_values[i], tz_str) == 0) return i;
    }
    return 0; /* Default to UTC */
}

/** Extract the host portion from an "mqtt://host" URL. */
static void extract_mqtt_host(const char *url, char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';

    const char *start = url;
    if (strncmp(start, "mqtt://", 7) == 0) start += 7;
    else if (strncmp(start, "mqtts://", 8) == 0) start += 8;

    /* Copy up to end or first '/' or ':' */
    size_t i = 0;
    while (start[i] && start[i] != '/' && start[i] != ':' && i < out_size - 1) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
}

/** Create a read-only info row: left label + right value label. */
static lv_obj_t *make_info_row(lv_obj_t *parent, const char *title,
                                const char *value, lv_obj_t **out_val_lbl)
{
    int gb = app_config_get()->color_brightness;

    lv_obj_t *row = settings_make_row(parent);

    lv_obj_t *lbl_title = lv_label_create(row);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_label_set_text(lbl_val, value ? value : "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_val,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    if (out_val_lbl) *out_val_lbl = lbl_val;
    return row;
}

/** Create an outlined button (transparent bg, border). */
static lv_obj_t *make_outlined_button(lv_obj_t *parent, const char *text,
                                       lv_obj_t **out_lbl)
{
    int gb = app_config_get()->color_brightness;

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 48);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    if (current_theme) {
        lv_obj_set_style_border_color(btn, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->bento_border), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_center(lbl);

    if (out_lbl) *out_lbl = lbl;
    return btn;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Network Card — WiFi SSID/Password, NTP, Timezone
 * ════════════════════════════════════════════════════════════════════════ */

static void create_network_card(lv_obj_t *parent) {
    int gb = app_config_get()->color_brightness;
    lv_obj_t *card = settings_make_card(parent, "NETWORK");

    /* WiFi SSID */
    settings_make_textarea_row(card, "WiFi SSID", "Network name", false, &ta_ssid);
    {
        wifi_config_t wifi_cfg;
        memset(&wifi_cfg, 0, sizeof(wifi_cfg));
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
            lv_textarea_set_text(ta_ssid, (const char *)wifi_cfg.sta.ssid);
        }
    }
    lv_obj_add_event_cb(ta_ssid, ssid_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta_ssid, ssid_defocus_cb, LV_EVENT_READY, NULL);

    settings_make_divider(card);

    /* WiFi Password */
    settings_make_textarea_row(card, "WiFi Password", "Enter new password", true, &ta_password);
    /* Never pre-populate password for security */
    lv_obj_add_event_cb(ta_password, password_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta_password, password_defocus_cb, LV_EVENT_READY, NULL);

    settings_make_divider(card);

    /* NTP Server */
    settings_make_textarea_row(card, "NTP Server", "pool.ntp.org", false, &ta_ntp);
    lv_textarea_set_text(ta_ntp, app_config_get()->ntp_server);
    lv_obj_add_event_cb(ta_ntp, ntp_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta_ntp, ntp_defocus_cb, LV_EVENT_READY, NULL);

    settings_make_divider(card);

    /* Timezone Dropdown */
    {
        lv_obj_t *row = settings_make_row(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Timezone");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_timezone = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_timezone, tz_names);
        lv_obj_set_width(dd_timezone, 220);
        lv_obj_set_style_text_font(dd_timezone, &lv_font_montserrat_16, 0);
        if (current_theme) {
            lv_obj_set_style_bg_color(dd_timezone, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_timezone, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(dd_timezone,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_border_color(dd_timezone, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_timezone, 1, 0);
        }

        int tz_idx = find_tz_index(app_config_get()->tz_string);
        lv_dropdown_set_selected(dd_timezone, tz_idx);
        lv_obj_add_event_cb(dd_timezone, timezone_change_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    settings_make_divider(card);

    /* Hint label */
    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Reboot required after WiFi/NTP changes");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(hint,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  MQTT Card — Enable toggle, broker, port, prefix, credentials
 * ════════════════════════════════════════════════════════════════════════ */

static void create_mqtt_card(lv_obj_t *parent) {
    int gb = app_config_get()->color_brightness;
    app_config_t *cfg = app_config_get();
    lv_obj_t *card = settings_make_card(parent, "MQTT");

    /* Enable MQTT toggle */
    settings_make_toggle_row(card, "Enable MQTT", &sw_mqtt_enabled);
    if (cfg->mqtt_enabled) {
        lv_obj_add_state(sw_mqtt_enabled, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw_mqtt_enabled, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw_mqtt_enabled, mqtt_enabled_cb, LV_EVENT_VALUE_CHANGED, NULL);

    settings_make_divider(card);

    /* MQTT fields container (shown/hidden by toggle) */
    cont_mqtt_fields = lv_obj_create(card);
    lv_obj_remove_style_all(cont_mqtt_fields);
    lv_obj_set_width(cont_mqtt_fields, LV_PCT(100));
    lv_obj_set_height(cont_mqtt_fields, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont_mqtt_fields, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont_mqtt_fields, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont_mqtt_fields, 8, 0);

    /* Broker Host */
    settings_make_textarea_row(cont_mqtt_fields, "Broker Host", "192.168.1.100", false, &ta_mqtt_broker);
    {
        char host[128] = {0};
        extract_mqtt_host(cfg->mqtt_broker_url, host, sizeof(host));
        lv_textarea_set_text(ta_mqtt_broker, host);
    }
    lv_obj_add_event_cb(ta_mqtt_broker, mqtt_broker_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta_mqtt_broker, mqtt_broker_defocus_cb, LV_EVENT_READY, NULL);

    settings_make_divider(cont_mqtt_fields);

    /* Port Stepper */
    {
        lv_obj_t *row = settings_make_row(cont_mqtt_fields);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Port");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        lv_obj_t *btn_minus, *btn_plus;
        settings_make_stepper(row, &btn_minus, &lbl_mqtt_port, &btn_plus);

        char port_buf[8];
        snprintf(port_buf, sizeof(port_buf), "%u", cfg->mqtt_port);
        lv_label_set_text(lbl_mqtt_port, port_buf);

        lv_obj_add_event_cb(btn_minus, mqtt_port_minus_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn_plus, mqtt_port_plus_cb, LV_EVENT_CLICKED, NULL);
    }

    settings_make_divider(cont_mqtt_fields);

    /* Topic Prefix */
    settings_make_textarea_row(cont_mqtt_fields, "Topic Prefix", "ninadisplay", false, &ta_mqtt_prefix);
    lv_textarea_set_text(ta_mqtt_prefix, cfg->mqtt_topic_prefix);
    lv_obj_add_event_cb(ta_mqtt_prefix, mqtt_prefix_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta_mqtt_prefix, mqtt_prefix_defocus_cb, LV_EVENT_READY, NULL);

    settings_make_divider(cont_mqtt_fields);

    /* Username */
    settings_make_textarea_row(cont_mqtt_fields, "Username", "Username", false, &ta_mqtt_user);
    lv_textarea_set_text(ta_mqtt_user, cfg->mqtt_username);
    lv_obj_add_event_cb(ta_mqtt_user, mqtt_user_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta_mqtt_user, mqtt_user_defocus_cb, LV_EVENT_READY, NULL);

    settings_make_divider(cont_mqtt_fields);

    /* Password */
    settings_make_textarea_row(cont_mqtt_fields, "Password", "Password", true, &ta_mqtt_pass);
    /* Don't pre-populate password for security — only store if non-empty */
    lv_obj_add_event_cb(ta_mqtt_pass, mqtt_pass_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta_mqtt_pass, mqtt_pass_defocus_cb, LV_EVENT_READY, NULL);

    /* Show/hide fields based on initial state */
    if (!cfg->mqtt_enabled) {
        lv_obj_add_flag(cont_mqtt_fields, LV_OBJ_FLAG_HIDDEN);
    }

    settings_make_divider(card);

    /* Hint */
    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Reboot required after MQTT changes");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(hint,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Firmware Card — Version info, auto-update, check for updates
 * ════════════════════════════════════════════════════════════════════════ */

static void create_firmware_card(lv_obj_t *parent) {
    int gb = app_config_get()->color_brightness;
    app_config_t *cfg = app_config_get();
    lv_obj_t *card = settings_make_card(parent, "FIRMWARE");

    /* Version */
    make_info_row(card, "Version", BUILD_GIT_TAG, &lbl_fw_version);

    settings_make_divider(card);

    /* Built */
    {
        char built_str[64];
        /* Show first 7 chars of SHA + branch */
        snprintf(built_str, sizeof(built_str), "%.7s / %s", BUILD_GIT_SHA, BUILD_GIT_BRANCH);
        make_info_row(card, "Built", built_str, &lbl_fw_built);
    }

    settings_make_divider(card);

    /* IDF Version */
    make_info_row(card, "IDF", esp_get_idf_version(), &lbl_fw_idf);

    settings_make_divider(card);

    /* Running Partition */
    {
        const esp_partition_t *part = esp_ota_get_running_partition();
        make_info_row(card, "Partition", part ? part->label : "unknown", &lbl_fw_partition);
    }

    settings_make_divider(card);

    /* Auto Update Check toggle */
    settings_make_toggle_row(card, "Auto Update Check", &sw_auto_update);
    if (cfg->auto_update_check) {
        lv_obj_add_state(sw_auto_update, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw_auto_update, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw_auto_update, auto_update_cb, LV_EVENT_VALUE_CHANGED, NULL);

    settings_make_divider(card);

    /* Update Channel dropdown */
    {
        lv_obj_t *row = settings_make_row(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Channel");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_update_channel = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_update_channel, "Stable\nPre-releases");
        lv_obj_set_width(dd_update_channel, 180);
        lv_obj_set_style_text_font(dd_update_channel, &lv_font_montserrat_16, 0);
        if (current_theme) {
            lv_obj_set_style_bg_color(dd_update_channel, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_update_channel, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(dd_update_channel,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_border_color(dd_update_channel, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_update_channel, 1, 0);
        }

        lv_dropdown_set_selected(dd_update_channel, cfg->update_channel);
        lv_obj_add_event_cb(dd_update_channel, update_channel_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    settings_make_divider(card);

    /* Check for Updates button */
    btn_check_update = lv_button_create(card);
    lv_obj_set_width(btn_check_update, LV_PCT(100));
    lv_obj_set_height(btn_check_update, 48);
    lv_obj_set_style_bg_opa(btn_check_update, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_check_update, 2, 0);
    lv_obj_set_style_shadow_width(btn_check_update, 0, 0);
    lv_obj_set_style_radius(btn_check_update, 10, 0);
    if (current_theme) {
        lv_obj_set_style_border_color(btn_check_update, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn_check_update, lv_color_hex(current_theme->bento_border), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn_check_update, LV_OPA_40, LV_STATE_PRESSED);
    }

    lbl_check_update = lv_label_create(btn_check_update);
    lv_label_set_text(lbl_check_update, LV_SYMBOL_REFRESH "  Check for Updates");
    lv_obj_set_style_text_font(lbl_check_update, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_check_update,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_center(lbl_check_update);

    lv_obj_add_event_cb(btn_check_update, check_update_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Device Card — Debug mode, Reboot, Factory Reset
 * ════════════════════════════════════════════════════════════════════════ */

static void create_device_card(lv_obj_t *parent) {
    app_config_t *cfg = app_config_get();
    lv_obj_t *card = settings_make_card(parent, "DEVICE");

    /* Debug Mode toggle */
    settings_make_toggle_row(card, "Debug Mode", &sw_debug_mode);
    if (cfg->debug_mode) {
        lv_obj_add_state(sw_debug_mode, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw_debug_mode, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw_debug_mode, debug_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    settings_make_divider(card);

    /* Reboot button */
    {
        lv_obj_t *btn_lbl;
        lv_obj_t *btn = make_outlined_button(card, LV_SYMBOL_REFRESH "  Reboot Device", &btn_lbl);
        lv_obj_add_event_cb(btn, reboot_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    settings_make_divider(card);

    /* Factory Reset button (red/danger styling) */
    {
        lv_obj_t *btn = lv_button_create(card);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 48);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xDC2626), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xDC2626), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, LV_SYMBOL_TRASH "  Factory Reset");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xDC2626), 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, factory_reset_btn_cb, LV_EVENT_CLICKED, NULL);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Event Callbacks — Network
 * ════════════════════════════════════════════════════════════════════════ */

static void ssid_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!ta_ssid) return;

    const char *text = lv_textarea_get_text(ta_ssid);
    if (!text || text[0] == '\0') return;

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

    /* Only update if changed */
    if (strcmp((const char *)wifi_cfg.sta.ssid, text) != 0) {
        strncpy((char *)wifi_cfg.sta.ssid, text, sizeof(wifi_cfg.sta.ssid) - 1);
        wifi_cfg.sta.ssid[sizeof(wifi_cfg.sta.ssid) - 1] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        settings_mark_dirty(true);
    }
}

static void password_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!ta_password) return;

    const char *text = lv_textarea_get_text(ta_password);
    if (!text || text[0] == '\0') return;  /* Don't store empty password */

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

    strncpy((char *)wifi_cfg.sta.password, text, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.password[sizeof(wifi_cfg.sta.password) - 1] = '\0';
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    /* Clear the password field after storing */
    lv_textarea_set_text(ta_password, "");
    settings_mark_dirty(true);
}

static void ntp_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!ta_ntp) return;

    const char *text = lv_textarea_get_text(ta_ntp);
    if (!text) return;

    app_config_t *cfg = app_config_get();
    if (strcmp(cfg->ntp_server, text) != 0) {
        strncpy(cfg->ntp_server, text, sizeof(cfg->ntp_server) - 1);
        cfg->ntp_server[sizeof(cfg->ntp_server) - 1] = '\0';
        settings_mark_dirty(true);
    }
}

static void timezone_change_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!dd_timezone) return;

    uint32_t sel = lv_dropdown_get_selected(dd_timezone);
    if (sel >= TZ_COUNT) return;

    app_config_t *cfg = app_config_get();
    if (strcmp(cfg->tz_string, tz_values[sel]) != 0) {
        strncpy(cfg->tz_string, tz_values[sel], sizeof(cfg->tz_string) - 1);
        cfg->tz_string[sizeof(cfg->tz_string) - 1] = '\0';
        settings_mark_dirty(true);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Event Callbacks — MQTT
 * ════════════════════════════════════════════════════════════════════════ */

static void mqtt_enabled_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!sw_mqtt_enabled || !cont_mqtt_fields) return;

    app_config_t *cfg = app_config_get();
    bool enabled = lv_obj_has_state(sw_mqtt_enabled, LV_STATE_CHECKED);
    cfg->mqtt_enabled = enabled;

    if (enabled) {
        lv_obj_clear_flag(cont_mqtt_fields, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(cont_mqtt_fields, LV_OBJ_FLAG_HIDDEN);
    }

    settings_mark_dirty(true);
}

static void mqtt_broker_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!ta_mqtt_broker) return;

    const char *text = lv_textarea_get_text(ta_mqtt_broker);
    if (!text) return;

    app_config_t *cfg = app_config_get();
    char new_url[128];
    snprintf(new_url, sizeof(new_url), "mqtt://%s", text);

    if (strcmp(cfg->mqtt_broker_url, new_url) != 0) {
        strncpy(cfg->mqtt_broker_url, new_url, sizeof(cfg->mqtt_broker_url) - 1);
        cfg->mqtt_broker_url[sizeof(cfg->mqtt_broker_url) - 1] = '\0';
        settings_mark_dirty(true);
    }
}

static void mqtt_port_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->mqtt_port > 1) {
        cfg->mqtt_port--;
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", cfg->mqtt_port);
        if (lbl_mqtt_port) lv_label_set_text(lbl_mqtt_port, buf);
        settings_mark_dirty(true);
    }
}

static void mqtt_port_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->mqtt_port < 65535) {
        cfg->mqtt_port++;
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", cfg->mqtt_port);
        if (lbl_mqtt_port) lv_label_set_text(lbl_mqtt_port, buf);
        settings_mark_dirty(true);
    }
}

static void mqtt_prefix_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!ta_mqtt_prefix) return;

    const char *text = lv_textarea_get_text(ta_mqtt_prefix);
    if (!text) return;

    app_config_t *cfg = app_config_get();
    if (strcmp(cfg->mqtt_topic_prefix, text) != 0) {
        strncpy(cfg->mqtt_topic_prefix, text, sizeof(cfg->mqtt_topic_prefix) - 1);
        cfg->mqtt_topic_prefix[sizeof(cfg->mqtt_topic_prefix) - 1] = '\0';
        settings_mark_dirty(true);
    }
}

static void mqtt_user_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!ta_mqtt_user) return;

    const char *text = lv_textarea_get_text(ta_mqtt_user);
    if (!text) return;

    app_config_t *cfg = app_config_get();
    if (strcmp(cfg->mqtt_username, text) != 0) {
        strncpy(cfg->mqtt_username, text, sizeof(cfg->mqtt_username) - 1);
        cfg->mqtt_username[sizeof(cfg->mqtt_username) - 1] = '\0';
        settings_mark_dirty(true);
    }
}

static void mqtt_pass_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!ta_mqtt_pass) return;

    const char *text = lv_textarea_get_text(ta_mqtt_pass);
    if (!text || text[0] == '\0') return;  /* Don't store empty password */

    app_config_t *cfg = app_config_get();
    strncpy(cfg->mqtt_password, text, sizeof(cfg->mqtt_password) - 1);
    cfg->mqtt_password[sizeof(cfg->mqtt_password) - 1] = '\0';
    /* Clear after storing */
    lv_textarea_set_text(ta_mqtt_pass, "");
    settings_mark_dirty(true);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Event Callbacks — Firmware
 * ════════════════════════════════════════════════════════════════════════ */

static void auto_update_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!sw_auto_update) return;

    app_config_t *cfg = app_config_get();
    cfg->auto_update_check = lv_obj_has_state(sw_auto_update, LV_STATE_CHECKED) ? 1 : 0;
    settings_mark_dirty(false);
}

static void update_channel_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!dd_update_channel) return;

    app_config_t *cfg = app_config_get();
    cfg->update_channel = (uint8_t)lv_dropdown_get_selected(dd_update_channel);
    settings_mark_dirty(false);
}

static void check_update_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    ota_check_requested = true;
    if (lbl_check_update) {
        lv_label_set_text(lbl_check_update, LV_SYMBOL_REFRESH "  Checking...");
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Event Callbacks — Device
 * ════════════════════════════════════════════════════════════════════════ */

static void debug_mode_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!sw_debug_mode) return;

    app_config_t *cfg = app_config_get();
    cfg->debug_mode = lv_obj_has_state(sw_debug_mode, LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

static void confirm_reboot_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_user_data(e);
    lv_msgbox_close(mbox);
    esp_restart();
}

static void reboot_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(mbox, "Reboot Device");
    lv_msgbox_add_text(mbox, "Are you sure you want to reboot?");
    lv_msgbox_add_close_button(mbox);
    lv_obj_t *btn_yes = lv_msgbox_add_footer_button(mbox, "Reboot");
    lv_obj_add_event_cb(btn_yes, confirm_reboot_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_center(mbox);
}

static void confirm_factory_reset_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_user_data(e);
    lv_msgbox_close(mbox);
    app_config_factory_reset();
    esp_restart();
}

static void factory_reset_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(mbox, "Factory Reset");
    lv_msgbox_add_text(mbox, "This will erase ALL settings and reboot.\nAre you sure?");
    lv_msgbox_add_close_button(mbox);
    lv_obj_t *btn_yes = lv_msgbox_add_footer_button(mbox, "Reset Everything");
    lv_obj_add_event_cb(btn_yes, confirm_factory_reset_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_center(mbox);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — Create
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_system_create(lv_obj_t *parent) {
    tab_root = parent;

    create_network_card(parent);
    create_mqtt_card(parent);
    create_firmware_card(parent);
    create_device_card(parent);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — Refresh
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_system_refresh(void) {
    app_config_t *cfg = app_config_get();

    /* Network — WiFi SSID */
    if (ta_ssid) {
        wifi_config_t wifi_cfg;
        memset(&wifi_cfg, 0, sizeof(wifi_cfg));
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
            lv_textarea_set_text(ta_ssid, (const char *)wifi_cfg.sta.ssid);
        }
    }

    /* WiFi Password — never pre-populate */
    if (ta_password) {
        lv_textarea_set_text(ta_password, "");
    }

    /* NTP */
    if (ta_ntp) {
        lv_textarea_set_text(ta_ntp, cfg->ntp_server);
    }

    /* Timezone */
    if (dd_timezone) {
        int tz_idx = find_tz_index(cfg->tz_string);
        lv_dropdown_set_selected(dd_timezone, tz_idx);
    }

    /* MQTT */
    if (sw_mqtt_enabled) {
        if (cfg->mqtt_enabled) {
            lv_obj_add_state(sw_mqtt_enabled, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(sw_mqtt_enabled, LV_STATE_CHECKED);
        }
    }
    if (cont_mqtt_fields) {
        if (cfg->mqtt_enabled) {
            lv_obj_clear_flag(cont_mqtt_fields, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(cont_mqtt_fields, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ta_mqtt_broker) {
        char host[128] = {0};
        extract_mqtt_host(cfg->mqtt_broker_url, host, sizeof(host));
        lv_textarea_set_text(ta_mqtt_broker, host);
    }
    if (lbl_mqtt_port) {
        char port_buf[8];
        snprintf(port_buf, sizeof(port_buf), "%u", cfg->mqtt_port);
        lv_label_set_text(lbl_mqtt_port, port_buf);
    }
    if (ta_mqtt_prefix) {
        lv_textarea_set_text(ta_mqtt_prefix, cfg->mqtt_topic_prefix);
    }
    if (ta_mqtt_user) {
        lv_textarea_set_text(ta_mqtt_user, cfg->mqtt_username);
    }
    if (ta_mqtt_pass) {
        lv_textarea_set_text(ta_mqtt_pass, "");
    }

    /* Firmware */
    if (lbl_fw_version) {
        lv_label_set_text(lbl_fw_version, BUILD_GIT_TAG);
    }
    if (lbl_fw_built) {
        char built_str[64];
        snprintf(built_str, sizeof(built_str), "%.7s / %s", BUILD_GIT_SHA, BUILD_GIT_BRANCH);
        lv_label_set_text(lbl_fw_built, built_str);
    }
    if (lbl_fw_idf) {
        lv_label_set_text(lbl_fw_idf, esp_get_idf_version());
    }
    if (lbl_fw_partition) {
        const esp_partition_t *part = esp_ota_get_running_partition();
        lv_label_set_text(lbl_fw_partition, part ? part->label : "unknown");
    }
    if (sw_auto_update) {
        if (cfg->auto_update_check) {
            lv_obj_add_state(sw_auto_update, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(sw_auto_update, LV_STATE_CHECKED);
        }
    }
    if (dd_update_channel) {
        lv_dropdown_set_selected(dd_update_channel, cfg->update_channel);
    }

    /* Reset check-for-updates button label */
    if (lbl_check_update && !ota_check_requested) {
        lv_label_set_text(lbl_check_update, LV_SYMBOL_REFRESH "  Check for Updates");
    }

    /* Device */
    if (sw_debug_mode) {
        if (cfg->debug_mode) {
            lv_obj_add_state(sw_debug_mode, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(sw_debug_mode, LV_STATE_CHECKED);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — Apply Theme
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_system_apply_theme(void) {
    if (tab_root) settings_apply_theme_recursive(tab_root);
}
