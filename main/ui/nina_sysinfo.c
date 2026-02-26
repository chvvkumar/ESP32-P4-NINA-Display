/**
 * @file nina_sysinfo.c
 * @brief System information page — IP, hostname, WiFi quality, CPU, memory, PSRAM stats.
 */

#include "nina_sysinfo.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include "perf_monitor.h"

/* ── Layout ──────────────────────────────────────────────────────────── */
#define SI_PAD       16
#define SI_GAP       12
#define SI_RADIUS    24
#define SI_ICON_SIZE 48

/* ── Widgets ─────────────────────────────────────────────────────────── */
static lv_obj_t *si_page        = NULL;

/* Header */
static lv_obj_t *lbl_title      = NULL;

/* Network card */
static lv_obj_t *lbl_hostname_val = NULL;
static lv_obj_t *lbl_sta_ip_val   = NULL;
static lv_obj_t *lbl_ap_ip_val    = NULL;
static lv_obj_t *lbl_mac_val      = NULL;

/* WiFi card */
static lv_obj_t *lbl_ssid_val    = NULL;
static lv_obj_t *lbl_rssi_val    = NULL;
static lv_obj_t *lbl_channel_val = NULL;
static lv_obj_t *bar_wifi        = NULL;

/* Memory card */
static lv_obj_t *lbl_heap_val    = NULL;
static lv_obj_t *bar_heap        = NULL;
static lv_obj_t *lbl_psram_val   = NULL;
static lv_obj_t *bar_psram       = NULL;

/* System card */
static lv_obj_t *lbl_chip_val    = NULL;
static lv_obj_t *lbl_idf_val     = NULL;
static lv_obj_t *lbl_uptime_val  = NULL;
static lv_obj_t *lbl_tasks_val   = NULL;

/* Performance card (debug mode) */
static lv_obj_t *perf_card = NULL;
static lv_obj_t *lbl_poll_cycle_val = NULL;
static lv_obj_t *lbl_http_reqs_val  = NULL;
static lv_obj_t *lbl_ws_events_val  = NULL;
static lv_obj_t *lbl_json_parses_val = NULL;
static lv_obj_t *lbl_lock_wait_val  = NULL;
static lv_obj_t *lbl_stack_hwm_val  = NULL;

/* CPU card (debug mode) */
static lv_obj_t *cpu_card = NULL;
static lv_obj_t *lbl_core0_val   = NULL;
static lv_obj_t *bar_core0       = NULL;
static lv_obj_t *lbl_core1_val   = NULL;
static lv_obj_t *bar_core1       = NULL;
static lv_obj_t *lbl_cputotal_val = NULL;
static lv_obj_t *bar_cputotal    = NULL;
static lv_obj_t *lbl_headroom_val = NULL;
static lv_obj_t *lbl_lvgl_fps_val = NULL;
static lv_obj_t *lbl_top_task_val = NULL;

/* Red Night theme forces all colors to the red palette */
static bool theme_forces_colors(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

/* Bar color helper: returns dim/medium/bright red when Red Night,
 * otherwise returns standard green/yellow/red based on percentage. */
static uint32_t bar_level_color(int pct, int low_thresh, int high_thresh) {
    if (theme_forces_colors()) {
        /* Dim, medium, bright red levels */
        if (pct < low_thresh)  return 0x7f1d1d;  /* dim red */
        if (pct < high_thresh) return 0xcc0000;   /* medium red */
        return 0xff0000;                           /* bright red */
    }
    if (pct < low_thresh)  return 0x22c55e;  /* green */
    if (pct < high_thresh) return 0xeab308;  /* yellow */
    return 0xef4444;                           /* red */
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static lv_obj_t *make_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_bento_box, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 6, 0);
    return card;
}

static lv_obj_t *make_section_title(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
    return lbl;
}

/* Row: "LABEL  value" in a single line */
static lv_obj_t *make_kv_row(lv_obj_t *parent, const char *key, lv_obj_t **out_val) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_key = lv_label_create(row);
    lv_label_set_text(lbl_key, key);
    lv_obj_set_style_text_font(lbl_key, &lv_font_montserrat_16, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_key, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_18, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_val, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    *out_val = lbl_val;
    return row;
}

static lv_obj_t *make_bar(lv_obj_t *parent) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    if (current_theme) {
        lv_obj_set_style_bg_color(bar, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }
    return bar;
}

/* ── Page Creation ───────────────────────────────────────────────────── */

lv_obj_t *sysinfo_page_create(lv_obj_t *parent) {
    si_page = lv_obj_create(parent);
    lv_obj_remove_style_all(si_page);
    lv_obj_set_size(si_page, SCREEN_SIZE - 2 * OUTER_PADDING, SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_flex_flow(si_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(si_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(si_page, SI_GAP, 0);
    lv_obj_set_scrollbar_mode(si_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(si_page, LV_DIR_VER);

    /* ── Title ── */
    lv_obj_t *hdr = lv_obj_create(si_page);
    lv_obj_remove_style_all(hdr);
    lv_obj_add_style(hdr, &style_header_gradient, 0);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(hdr, 14, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 12, 0);

    /* Gear icon — uses default font which includes LV_SYMBOL glyphs */
    lv_obj_t *icon = lv_label_create(hdr);
    lv_label_set_text(icon, LV_SYMBOL_SETTINGS);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(icon, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    }

    lbl_title = lv_label_create(hdr);
    lv_label_set_text(lbl_title, "System Info");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_26, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_title, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    }

    /* ── Two-column layout for cards ── */
    lv_obj_t *cols = lv_obj_create(si_page);
    lv_obj_remove_style_all(cols);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_flex_grow(cols, 1);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(cols, SI_GAP, 0);

    /* Left column */
    lv_obj_t *col_left = lv_obj_create(cols);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_flex_grow(col_left, 1);
    lv_obj_set_height(col_left, LV_PCT(100));
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_left, SI_GAP, 0);

    /* Right column */
    lv_obj_t *col_right = lv_obj_create(cols);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_flex_grow(col_right, 1);
    lv_obj_set_height(col_right, LV_PCT(100));
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_right, SI_GAP, 0);

    /* ── Network Card (left) ── */
    {
        lv_obj_t *card = make_card(col_left);
        make_section_title(card, "NETWORK");
        make_kv_row(card, "Hostname", &lbl_hostname_val);
        make_kv_row(card, "STA IP", &lbl_sta_ip_val);
        make_kv_row(card, "AP IP", &lbl_ap_ip_val);
        make_kv_row(card, "MAC", &lbl_mac_val);
    }

    /* ── WiFi Card (left) ── */
    {
        lv_obj_t *card = make_card(col_left);
        make_section_title(card, "WIFI");
        make_kv_row(card, "SSID", &lbl_ssid_val);
        make_kv_row(card, "RSSI", &lbl_rssi_val);
        make_kv_row(card, "Channel", &lbl_channel_val);
        bar_wifi = make_bar(card);
    }

    /* ── Memory Card (right) ── */
    {
        lv_obj_t *card = make_card(col_right);
        make_section_title(card, "MEMORY");
        make_kv_row(card, "Heap", &lbl_heap_val);
        bar_heap = make_bar(card);
        make_kv_row(card, "PSRAM", &lbl_psram_val);
        bar_psram = make_bar(card);
    }

    /* ── CPU Card (right, visible when debug mode enabled) ── */
    {
        cpu_card = make_card(col_right);
        make_section_title(cpu_card, "CPU");
        make_kv_row(cpu_card, "Core 0", &lbl_core0_val);
        bar_core0 = make_bar(cpu_card);
        make_kv_row(cpu_card, "Core 1", &lbl_core1_val);
        bar_core1 = make_bar(cpu_card);
        make_kv_row(cpu_card, "Total", &lbl_cputotal_val);
        bar_cputotal = make_bar(cpu_card);
        make_kv_row(cpu_card, "Headroom", &lbl_headroom_val);
        make_kv_row(cpu_card, "Render", &lbl_lvgl_fps_val);
        make_kv_row(cpu_card, "Top Task", &lbl_top_task_val);
        if (!g_perf.enabled) lv_obj_add_flag(cpu_card, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── System Card (right) ── */
    {
        lv_obj_t *card = make_card(col_right);
        make_section_title(card, "SYSTEM");
        make_kv_row(card, "Chip", &lbl_chip_val);
        make_kv_row(card, "IDF", &lbl_idf_val);
        make_kv_row(card, "Uptime", &lbl_uptime_val);
        make_kv_row(card, "Tasks", &lbl_tasks_val);
    }

    /* ── Performance Card (left, visible when debug mode enabled) ── */
    {
        perf_card = make_card(col_left);
        make_section_title(perf_card, "PERFORMANCE");
        make_kv_row(perf_card, "Poll Cycle", &lbl_poll_cycle_val);
        make_kv_row(perf_card, "HTTP Reqs", &lbl_http_reqs_val);
        make_kv_row(perf_card, "WS Events", &lbl_ws_events_val);
        make_kv_row(perf_card, "JSON Parses", &lbl_json_parses_val);
        make_kv_row(perf_card, "Lock Wait", &lbl_lock_wait_val);
        make_kv_row(perf_card, "Stack HWM", &lbl_stack_hwm_val);
        if (!g_perf.enabled) lv_obj_add_flag(perf_card, LV_OBJ_FLAG_HIDDEN);
    }

    return si_page;
}

/* ── Data Refresh ────────────────────────────────────────────────────── */

void sysinfo_page_refresh(void) {
    if (!si_page) return;

    char buf[128];

    /* ── Network ── */
    {
        /* Hostname from config (AP SSID serves as device name) */
        lv_label_set_text(lbl_hostname_val, "NINA-DISPLAY");

        /* STA IP */
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(lbl_sta_ip_val, buf);
            } else {
                lv_label_set_text(lbl_sta_ip_val, "Not connected");
            }
        } else {
            lv_label_set_text(lbl_sta_ip_val, "N/A");
        }

        /* AP IP */
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
                snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(lbl_ap_ip_val, buf);
            } else {
                lv_label_set_text(lbl_ap_ip_val, "N/A");
            }
        }

        /* MAC address */
        uint8_t mac[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            lv_label_set_text(lbl_mac_val, buf);
        }
    }

    /* ── WiFi ── */
    {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            lv_label_set_text(lbl_ssid_val, (const char *)ap_info.ssid);

            snprintf(buf, sizeof(buf), "%d dBm", ap_info.rssi);
            lv_label_set_text(lbl_rssi_val, buf);

            snprintf(buf, sizeof(buf), "%d", ap_info.primary);
            lv_label_set_text(lbl_channel_val, buf);

            /* RSSI to percentage: -100 dBm = 0%, -30 dBm = 100% */
            int pct = (ap_info.rssi + 100) * 100 / 70;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            lv_bar_set_value(bar_wifi, pct, LV_ANIM_ON);

            /* Color the bar based on signal quality (inverted: high pct = good) */
            uint32_t bar_color = bar_level_color(100 - pct, 30, 60);
            lv_obj_set_style_bg_color(bar_wifi, lv_color_hex(bar_color), LV_PART_INDICATOR);
        } else {
            lv_label_set_text(lbl_ssid_val, "N/A");
            lv_label_set_text(lbl_rssi_val, "N/A");
            lv_label_set_text(lbl_channel_val, "N/A");
            lv_bar_set_value(bar_wifi, 0, LV_ANIM_OFF);
        }
    }

    /* ── Memory ── */
    {
        size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t heap_used = heap_total - heap_free;
        int heap_pct = heap_total > 0 ? (int)((heap_used * 100) / heap_total) : 0;
        snprintf(buf, sizeof(buf), "%u / %u KB", (unsigned)(heap_used / 1024), (unsigned)(heap_total / 1024));
        lv_label_set_text(lbl_heap_val, buf);
        lv_bar_set_value(bar_heap, heap_pct, LV_ANIM_ON);

        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t psram_used = psram_total - psram_free;
        int psram_pct = psram_total > 0 ? (int)((psram_used * 100) / psram_total) : 0;
        if (psram_total > 0) {
            snprintf(buf, sizeof(buf), "%u / %u KB", (unsigned)(psram_used / 1024), (unsigned)(psram_total / 1024));
        } else {
            snprintf(buf, sizeof(buf), "Not available");
        }
        lv_label_set_text(lbl_psram_val, buf);
        lv_bar_set_value(bar_psram, psram_pct, LV_ANIM_ON);

        /* Color bars based on usage level */
        lv_obj_set_style_bg_color(bar_heap, lv_color_hex(bar_level_color(heap_pct, 60, 85)), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar_psram, lv_color_hex(bar_level_color(psram_pct, 60, 85)), LV_PART_INDICATOR);
    }

    /* ── System ── */
    {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        const char *model_name;
        switch (chip.model) {
            case CHIP_ESP32:   model_name = "ESP32";   break;
            case CHIP_ESP32S2: model_name = "ESP32-S2"; break;
            case CHIP_ESP32S3: model_name = "ESP32-S3"; break;
            case CHIP_ESP32C3: model_name = "ESP32-C3"; break;
            case CHIP_ESP32H2: model_name = "ESP32-H2"; break;
            default:           model_name = "ESP32-P4"; break;
        }
        snprintf(buf, sizeof(buf), "%s r%d.%d (%d core%s)",
                 model_name, chip.revision / 100, chip.revision % 100,
                 chip.cores, chip.cores > 1 ? "s" : "");
        lv_label_set_text(lbl_chip_val, buf);

        lv_label_set_text(lbl_idf_val, esp_get_idf_version());

        /* Uptime */
        int64_t us = esp_timer_get_time();
        int secs = (int)(us / 1000000);
        int days = secs / 86400; secs %= 86400;
        int hrs  = secs / 3600;  secs %= 3600;
        int mins = secs / 60;    secs %= 60;
        if (days > 0)
            snprintf(buf, sizeof(buf), "%dd %02d:%02d:%02d", days, hrs, mins, secs);
        else
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hrs, mins, secs);
        lv_label_set_text(lbl_uptime_val, buf);

        snprintf(buf, sizeof(buf), "%u", (unsigned)uxTaskGetNumberOfTasks());
        lv_label_set_text(lbl_tasks_val, buf);
    }

    /* ── Show/hide debug cards based on runtime toggle ── */
    if (cpu_card) {
        if (g_perf.enabled) lv_obj_clear_flag(cpu_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(cpu_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (perf_card) {
        if (g_perf.enabled) lv_obj_clear_flag(perf_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(perf_card, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── CPU ── */
    if (g_perf.enabled && cpu_card) {
        if (g_perf.cpu.valid) {
            int core0_pct = (int)(g_perf.cpu.core_load[0] + 0.5f);
            int core1_pct = (int)(g_perf.cpu.core_load[1] + 0.5f);
            int total_pct = (int)(g_perf.cpu.total_load + 0.5f);
            int headroom_pct = 100 - total_pct;

            snprintf(buf, sizeof(buf), "%d%%", core0_pct);
            lv_label_set_text(lbl_core0_val, buf);
            lv_bar_set_value(bar_core0, core0_pct, LV_ANIM_ON);

            snprintf(buf, sizeof(buf), "%d%%", core1_pct);
            lv_label_set_text(lbl_core1_val, buf);
            lv_bar_set_value(bar_core1, core1_pct, LV_ANIM_ON);

            snprintf(buf, sizeof(buf), "%d%%", total_pct);
            lv_label_set_text(lbl_cputotal_val, buf);
            lv_bar_set_value(bar_cputotal, total_pct, LV_ANIM_ON);

            snprintf(buf, sizeof(buf), "%d%%", headroom_pct);
            lv_label_set_text(lbl_headroom_val, buf);

            /* LVGL render timing */
            if (g_perf.ui_update_total.count > 0) {
                double avg_ms = (double)g_perf.ui_update_total.total_us
                                / (double)g_perf.ui_update_total.count / 1000.0;
                snprintf(buf, sizeof(buf), "%.1f ms avg", avg_ms);
            } else {
                snprintf(buf, sizeof(buf), "--");
            }
            lv_label_set_text(lbl_lvgl_fps_val, buf);

            /* Top task by CPU */
            if (g_perf.cpu.task_info_count > 0) {
                snprintf(buf, sizeof(buf), "%s %.1f%%",
                         g_perf.cpu.tasks[0].name, g_perf.cpu.tasks[0].cpu_percent);
            } else {
                snprintf(buf, sizeof(buf), "--");
            }
            lv_label_set_text(lbl_top_task_val, buf);

            /* Color bars based on CPU load level */
            lv_obj_set_style_bg_color(bar_core0, lv_color_hex(bar_level_color(core0_pct, 60, 85)), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar_core1, lv_color_hex(bar_level_color(core1_pct, 60, 85)), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar_cputotal, lv_color_hex(bar_level_color(total_pct, 60, 85)), LV_PART_INDICATOR);
        } else {
            lv_label_set_text(lbl_core0_val, "...");
            lv_label_set_text(lbl_core1_val, "...");
            lv_label_set_text(lbl_cputotal_val, "...");
            lv_label_set_text(lbl_headroom_val, "...");
            lv_label_set_text(lbl_lvgl_fps_val, "...");
            lv_label_set_text(lbl_top_task_val, "Waiting...");
            lv_bar_set_value(bar_core0, 0, LV_ANIM_OFF);
            lv_bar_set_value(bar_core1, 0, LV_ANIM_OFF);
            lv_bar_set_value(bar_cputotal, 0, LV_ANIM_OFF);
        }
    }

    /* ── Performance ── */
    if (g_perf.enabled && perf_card) {
        if (g_perf.poll_cycle_total.count > 0) {
            int64_t avg = g_perf.poll_cycle_total.total_us / g_perf.poll_cycle_total.count;
            snprintf(buf, sizeof(buf), "%lld ms (avg)", (long long)(avg / 1000));
            lv_label_set_text(lbl_poll_cycle_val, buf);
        }

        snprintf(buf, sizeof(buf), "%lu / %lus",
                 (unsigned long)g_perf.http_request_count.per_interval,
                 (unsigned long)g_perf.report_interval_s);
        lv_label_set_text(lbl_http_reqs_val, buf);

        snprintf(buf, sizeof(buf), "%lu / %lus",
                 (unsigned long)g_perf.ws_event_count.per_interval,
                 (unsigned long)g_perf.report_interval_s);
        lv_label_set_text(lbl_ws_events_val, buf);

        snprintf(buf, sizeof(buf), "%lu / %lus",
                 (unsigned long)g_perf.json_parse_count.per_interval,
                 (unsigned long)g_perf.report_interval_s);
        lv_label_set_text(lbl_json_parses_val, buf);

        if (g_perf.ui_lock_wait.count > 0) {
            int64_t avg = g_perf.ui_lock_wait.total_us / g_perf.ui_lock_wait.count;
            snprintf(buf, sizeof(buf), "%.1f ms (avg)", (float)avg / 1000.0f);
            lv_label_set_text(lbl_lock_wait_val, buf);
        }

        snprintf(buf, sizeof(buf), "%lu B",
                 (unsigned long)g_perf.data_task_stack_hwm);
        lv_label_set_text(lbl_stack_hwm_val, buf);
    }
}

/* ── Theme ───────────────────────────────────────────────────────────── */

static void apply_theme_recursive(lv_obj_t *obj);

static void apply_theme_to_label(lv_obj_t *obj) {
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;

    /* Section titles use label_color, value labels use text_color */
    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    if (font == &lv_font_montserrat_14) {
        lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    } else if (font == &lv_font_montserrat_26 || font == &lv_font_montserrat_28) {
        lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    } else if (font == &lv_font_montserrat_16) {
        lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    } else {
        lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
}

static void apply_theme_recursive(lv_obj_t *obj) {
    uint32_t child_cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            apply_theme_to_label(child);
        } else if (lv_obj_check_type(child, &lv_bar_class)) {
            if (current_theme) {
                lv_obj_set_style_bg_color(child, lv_color_hex(current_theme->bento_border), 0);
            }
        }
        apply_theme_recursive(child);
    }
}

void sysinfo_page_apply_theme(void) {
    if (!si_page || !current_theme) return;
    apply_theme_recursive(si_page);
    lv_obj_invalidate(si_page);
}
