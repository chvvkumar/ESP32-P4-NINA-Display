#include "freertos/FreeRTOS.h" // NINA Display
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "ui/nina_dashboard.h"
#include "ui/nina_toast.h"
#include "ui/nina_event_log.h"
#include "ui/nina_alerts.h"
#include "ui/nina_safety.h"
#include "ui/nina_session_stats.h"
#include "app_config.h"
#include "web_server.h"
#include "mqtt_ha.h"
#include "tasks.h"
#include "esp_ota_ops.h"
#include "driver/jpeg_decode.h"
#include "display/lv_display_private.h"
#include "draw/sw/lv_draw_sw_utils.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "perf_monitor.h"
#include "nina_connection.h"
#include "power_mgmt.h"
#include "spotify_auth.h"
#include "spotify_client.h"
#include "weather_client.h"
#include "ui/nina_spotify.h"
#include "wifi_manager.h"
#include "ui/nina_settings_tabview.h"
#include "ui/nina_thumbnail.h"

/* Embedded splash logo (JPEG, hardware-decoded at boot) */
extern const uint8_t logo_jpg_start[] asm("_binary_logo_jpg_start");
extern const uint8_t logo_jpg_end[]   asm("_binary_logo_jpg_end");

/*
 * ── SW rotation for DPI avoid-tearing mode ─────────────────────────
 *
 * The BSP disables sw_rotate when avoid_tearing is enabled because its
 * flush callback only has a single rotation buffer — the DPI controller
 * reads it while the next frame is being written, causing tearing.
 *
 * Fix: intercept the flush callback, rotate the full frame in-place
 * (DPI buf → temp PSRAM buf → back to DPI buf), then let the BSP
 * flush handle the normal DPI buffer swap.  The DPI controller only
 * ever sees its own registered framebuffers — no flicker.
 */
static lv_color_t *rot_buf;              /* temp rotation buffer (PSRAM)    */
static lv_display_flush_cb_t orig_flush; /* saved BSP flush callback        */

/* Mirror of the BSP's private lvgl_port_display_ctx_t — only the fields we
 * need for rotation.  Must match esp_lvgl_port v2.7.2 layout.
 * NOTE: BSP uses CONFIG_LVGL_PORT_ENABLE_PPA (not CONFIG_LV_USE_PPA) to
 * conditionally include ppa_handle. Using the wrong guard shifts the flags
 * field by 4 bytes, causing silent memory corruption. */
typedef struct {
    int   disp_type;
    void *io_handle, *panel_handle, *control_handle;
    struct { bool swap_xy, mirror_x, mirror_y; } rotation;
    lv_color_t *draw_buffs[3];
    uint8_t    *oled_buffer;
    void       *disp_drv;
    int         current_rotation;
    void       *trans_sem, *rounder_cb;
#if CONFIG_LVGL_PORT_ENABLE_PPA
    void       *ppa_handle;
#endif
    struct {
        unsigned int monochrome:  1;
        unsigned int swap_bytes:  1;
        unsigned int full_refresh:1;
        unsigned int direct_mode: 1;
        unsigned int sw_rotate:   1;
    } flags;
} disp_ctx_compat_t;

static void rotated_flush_cb(lv_display_t *drv, const lv_area_t *area,
                              uint8_t *color_map)
{
    int rot = lv_display_get_rotation(drv);
    if (rot == LV_DISPLAY_ROTATION_0 || !rot_buf) {
        orig_flush(drv, area, color_map);
        return;
    }

    /* Rotate entire frame: DPI buffer → temp → back to DPI buffer.
     * The DPI controller is reading the OTHER framebuffer right now,
     * so overwriting this one is safe (no tearing). */
    lv_color_format_t cf = lv_display_get_color_format(drv);
    uint32_t stride = lv_draw_buf_width_to_stride(BSP_LCD_H_RES, cf);

    lv_draw_sw_rotate(color_map, (uint8_t *)rot_buf,
                      BSP_LCD_H_RES, BSP_LCD_V_RES,
                      stride, stride, rot, cf);
    memcpy(color_map, rot_buf, stride * BSP_LCD_V_RES);

    /* BSP flush handles draw_bitmap + semaphore.  It checks
     * sw_rotate && current_rotation > 0, but draw_buffs[2] == NULL
     * so BSP skips its own rotation → straight to DPI buffer swap. */
    orig_flush(drv, area, color_map);
}

static void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }

/* Splash screen animation helpers */
static void splash_fade_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static lv_image_dsc_t splash_dsc;

static void splash_fade_done(lv_anim_t *a)
{
    lv_obj_delete((lv_obj_t *)a->var);
    /* Free the hardware-decoded RGB565 buffer */
    if (splash_dsc.data) {
        free((void *)splash_dsc.data);
        splash_dsc.data = NULL;
    }
}

static const char *TAG = "main";

/* Shared globals — used by event_handler() and tasks.c */
EventGroupHandle_t s_wifi_event_group;
int instance_count = 1;

/* WiFi multi-network fallback */
static int current_network_index = 0;
static int wifi_attempt_count = 0;
static bool manual_switch_pending = false;
static int pending_switch_index = -1;
static int networks_tried = 0;
static esp_timer_handle_t wifi_reconnect_timer = NULL;
#define WIFI_RETRY_PER_NETWORK  2
#define WIFI_FULL_CYCLE_DELAY_MS 30000

static bool wifi_advance_to_next_network(void)
{
    const app_config_t *cfg = app_config_get();
    for (int i = 1; i <= 3; i++) {
        int idx = (current_network_index + i) % 3;
        if (cfg->wifi_networks[idx].ssid[0] != '\0') {
            current_network_index = idx;
            return true;
        }
    }
    return false;
}

static void wifi_connect_to_slot(int index)
{
    const app_config_t *cfg = app_config_get();
    wifi_config_t sta_cfg = {0};

    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.rssi = -90;
    if (cfg->wifi_networks[index].password[0] != '\0') {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    strlcpy((char *)sta_cfg.sta.ssid, cfg->wifi_networks[index].ssid,
            sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg->wifi_networks[index].password,
            sizeof(sta_cfg.sta.password));

    current_network_index = index;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi: connecting to network %d (%s)", index + 1,
             cfg->wifi_networks[index].ssid);
}

void wifi_switch_to_network(int index)
{
    if (index < 0 || index > 2) return;
    const app_config_t *cfg = app_config_get();
    if (cfg->wifi_networks[index].ssid[0] == '\0') return;

    ESP_LOGI(TAG, "WiFi: manual switch to network %d (%s)", index + 1,
             cfg->wifi_networks[index].ssid);

    manual_switch_pending = true;
    wifi_attempt_count = 0;
    networks_tried = 0;

    pending_switch_index = index;
    esp_timer_stop(wifi_reconnect_timer);
    esp_wifi_disconnect();
    /* Connection happens in disconnect handler when it sees manual_switch_pending */
}

int wifi_get_current_network_index(void)
{
    return current_network_index;
}

static void wifi_reconnect_cb(void *arg)
{
    if (manual_switch_pending) {
        return;
    }

    const app_config_t *cfg = app_config_get();

    if (wifi_attempt_count >= WIFI_RETRY_PER_NETWORK) {
        wifi_attempt_count = 0;
        networks_tried++;

        int configured_count = 0;
        for (int i = 0; i < 3; i++) {
            if (cfg->wifi_networks[i].ssid[0] != '\0') configured_count++;
        }

        if (networks_tried >= configured_count) {
            networks_tried = 0;
            current_network_index = 0;
            for (int i = 0; i < 3; i++) {
                if (cfg->wifi_networks[i].ssid[0] != '\0') {
                    current_network_index = i;
                    break;
                }
            }
            ESP_LOGW(TAG, "WiFi: all networks exhausted, retrying in %d s",
                     WIFI_FULL_CYCLE_DELAY_MS / 1000);
            esp_timer_start_once(wifi_reconnect_timer,
                                 (uint64_t)WIFI_FULL_CYCLE_DELAY_MS * 1000ULL);
            return;
        }

        if (!wifi_advance_to_next_network()) {
            networks_tried = 0;
            ESP_LOGW(TAG, "WiFi: no other networks configured, retrying in %d s",
                     WIFI_FULL_CYCLE_DELAY_MS / 1000);
            esp_timer_start_once(wifi_reconnect_timer,
                                 (uint64_t)WIFI_FULL_CYCLE_DELAY_MS * 1000ULL);
            return;
        }
    }

    wifi_attempt_count++;
    wifi_connect_to_slot(current_network_index);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        perf_counter_increment(&g_perf.wifi_disconnect_count);
        esp_wifi_set_mode(WIFI_MODE_APSTA);

        if (manual_switch_pending) {
            manual_switch_pending = false;
            if (pending_switch_index >= 0) {
                wifi_connect_to_slot(pending_switch_index);
                pending_switch_index = -1;
            }
            return;
        }

        esp_timer_stop(wifi_reconnect_timer);
        esp_timer_start_once(wifi_reconnect_timer, 1000000ULL);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_attempt_count = 0;
        networks_tried = 0;

        /* Show green toast with connected network name and refresh settings UI */
        {
            const app_config_t *cfg = app_config_get();
            const char *ssid = cfg->wifi_networks[current_network_index].ssid;
            if (ssid[0] != '\0') {
                nina_toast_show_fmt(TOAST_SUCCESS, "Connected to %s", ssid);
            }
            if (lvgl_port_lock(100)) {
                settings_tabview_refresh();
                lvgl_port_unlock();
            }
        }

        /* STA connected — disable the config AP to keep the air clean */
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_APSTA) {
            ESP_LOGI(TAG, "STA connected, disabling AP");
            esp_wifi_set_mode(WIFI_MODE_STA);
        }

        /* Apply timezone before SNTP so localtime_r works as soon as time is set */
        const char *tz = app_config_get()->tz_string;
        if (tz[0] != '\0') {
            setenv("TZ", tz, 1);
            tzset();
            ESP_LOGI(TAG, "Timezone set to: %s", tz);
        }

        /* Guard against double-init on WiFi reconnect */
        if (!esp_sntp_enabled()) {
            ESP_LOGI(TAG, "Initializing SNTP");
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            const char *ntp = app_config_get()->ntp_server;
            esp_sntp_setservername(0, (ntp[0] != '\0') ? ntp : "pool.ntp.org");
            esp_sntp_init();
        }

        /* Enable WiFi modem sleep if configured — C6 radio sleeps between
         * DTIM beacons while maintaining AP association. Reduces radio power
         * from ~115 mA to ~20-40 mA average with zero reconnection cost. */
        if (app_config_get()->wifi_power_save) {
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            ESP_LOGI(TAG, "WiFi modem sleep enabled");
        } else {
            esp_wifi_set_ps(WIFI_PS_NONE);
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* Set device hostname for DHCP client (used by routers for device identification) */
    const char *hostname = app_config_get()->hostname;
    if (hostname[0] != '\0') {
        esp_netif_set_hostname(sta_netif, hostname);
        ESP_LOGI(TAG, "Hostname set to: %s", hostname);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Create WiFi reconnection timer (multi-network fallback)
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = wifi_reconnect_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &wifi_reconnect_timer));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    /*
     * STA credentials are stored in app_config wifi_networks[] array
     * (up to 3 priority-ordered networks). The first non-empty slot is
     * used at boot; fallback rotates through configured networks.
     */

    wifi_config_t wifi_config_ap = {
        .ap = {
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    /* Use configured hostname as AP SSID so the device is identifiable */
    const char *ap_name = hostname[0] ? hostname : "NINA-DISPLAY";
    size_t ap_len = strlen(ap_name);
    if (ap_len > sizeof(wifi_config_ap.ap.ssid)) ap_len = sizeof(wifi_config_ap.ap.ssid);
    memcpy(wifi_config_ap.ap.ssid, ap_name, ap_len);
    wifi_config_ap.ap.ssid_len = ap_len;

    /* Load STA credentials from app_config wifi_networks[0] (highest priority) */
    {
        const app_config_t *cfg = app_config_get();
        if (cfg->wifi_networks[0].ssid[0] != '\0') {
            wifi_config_t sta_cfg = {0};
            sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
            sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
            sta_cfg.sta.threshold.rssi = -90;
            if (cfg->wifi_networks[0].password[0] != '\0') {
                sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            } else {
                sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            }
            strlcpy((char *)sta_cfg.sta.ssid, cfg->wifi_networks[0].ssid,
                    sizeof(sta_cfg.sta.ssid));
            strlcpy((char *)sta_cfg.sta.password, cfg->wifi_networks[0].password,
                    sizeof(sta_cfg.sta.password));
            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init finished.");
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Increment boot counter in NVS */
    {
        nvs_handle_t nvs;
        if (nvs_open("system", NVS_READWRITE, &nvs) == ESP_OK) {
            uint32_t boot_cnt = 0;
            nvs_get_u32(nvs, "boot_cnt", &boot_cnt);
            boot_cnt++;
            nvs_set_u32(nvs, "boot_cnt", boot_cnt);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)boot_cnt);
        }
    }

    /* Route all cJSON allocations to PSRAM to reduce internal heap pressure */
    cJSON_Hooks psram_hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = free };
    cJSON_InitHooks(&psram_hooks);

    /* Suppress verbose ESP-IDF HTTP/TLS transport errors — connection failures
     * are already reported cleanly by nina_client as "unreachable" messages. */
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);

    app_config_init();

    // Check if we woke from deep sleep
    esp_sleep_wakeup_cause_t wake_cause = power_mgmt_check_wake_cause();

    // Track crash resets (PANIC, WDT) in RTC memory
    power_mgmt_check_crash();

    nina_connection_init();

    perf_monitor_init(30);
    perf_monitor_set_enabled(app_config_get()->debug_mode);

    instance_count = app_config_get_instance_count();
    if (app_config_get()->demo_mode) {
        instance_count = 3;  /* Demo mode always shows all 3 instance profiles */
        ESP_LOGI(TAG, "Demo mode: forcing instance_count = 3");
    }
    ESP_LOGI(TAG, "Configured instances: %d", instance_count);

    wifi_init();

    /* One-time migration: copy WiFi credentials from ESP-IDF NVS to app_config
     * wifi_networks[0] if it's empty (first boot after upgrade to v24). */
    {
        app_config_t *cfg = app_config_get();
        if (cfg->wifi_networks[0].ssid[0] == '\0') {
            wifi_config_t sta_cfg = {0};
            if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK &&
                sta_cfg.sta.ssid[0] != '\0') {
                strlcpy(cfg->wifi_networks[0].ssid,
                        (const char *)sta_cfg.sta.ssid,
                        sizeof(cfg->wifi_networks[0].ssid));
                strlcpy(cfg->wifi_networks[0].password,
                        (const char *)sta_cfg.sta.password,
                        sizeof(cfg->wifi_networks[0].password));
                app_config_save(cfg);
                ESP_LOGI(TAG, "Migrated WiFi credentials to wifi_networks[0]: %s",
                         cfg->wifi_networks[0].ssid);
            }
        }
    }

    // Enable Dynamic Frequency Scaling — CPU scales 360 MHz (active) to 40 MHz (idle)
    power_mgmt_init();

    start_web_server();

    /* Pre-allocate JPEG encoder DMA channel before display init claims DMA resources */
    screenshot_encoder_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    cfg.lvgl_port_cfg.task_stack = 10240; /* Default 7168 is too tight for settings page */
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    bsp_display_brightness_set(app_config_get()->brightness);

    /* ── SW rotation setup ──
     * Allocate one temp PSRAM buffer for in-place rotation.  The flush
     * callback rotates DPI buf → temp → back to DPI buf, then lets the
     * BSP handle the normal DPI buffer swap. */
    {
        lv_display_t *disp = lv_display_get_default();
        if (disp) {
            const size_t fb_size = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(lv_color_t);
            rot_buf = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
            if (!rot_buf) {
                ESP_LOGW(TAG, "No PSRAM for rotation buffer, SW rotation disabled");
            } else {
                /* Enable sw_rotate in BSP context so LVGL handles coordinate
                 * transforms and the BSP rotation-update returns early. */
                disp_ctx_compat_t *ctx =
                    (disp_ctx_compat_t *)lv_display_get_driver_data(disp);
                if (ctx) ctx->flags.sw_rotate = 1;

                orig_flush = disp->flush_cb;
                lv_display_set_flush_cb(disp, rotated_flush_cb);
                ESP_LOGI(TAG, "SW rotation ready (%.0f KB PSRAM)",
                         fb_size / 1024.0);
            }

            /* Apply saved screen rotation */
            uint8_t rot = app_config_get()->screen_rotation;
            if (rot > 0 && rot <= 3) {
                lvgl_port_lock(0);
                lv_display_set_rotation(disp, rot);
                lvgl_port_unlock();
            }
        }
    }

    /* Initialize session stats (PSRAM allocation, no LVGL) */
    nina_session_stats_init();

    /* ── Splash: hardware JPEG decode (no LVGL needed) ── */
    bool splash_ready = false;
    {
        const uint8_t *jpg_data = logo_jpg_start;
        size_t jpg_size = (size_t)(logo_jpg_end - logo_jpg_start);

        jpeg_decode_picture_info_t pic_info = {0};
        esp_err_t err = jpeg_decoder_get_info(jpg_data, jpg_size, &pic_info);

        if (err == ESP_OK && pic_info.width > 0) {
            uint32_t out_w = ((pic_info.width + 15) / 16) * 16;
            uint32_t out_h = ((pic_info.height + 15) / 16) * 16;

            jpeg_decode_memory_alloc_cfg_t mem_cfg = {
                .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
            };
            size_t allocated = 0;
            uint8_t *rgb_buf = (uint8_t *)jpeg_alloc_decoder_mem(out_w * out_h * 2, &mem_cfg, &allocated);

            if (rgb_buf) {
                memset(rgb_buf, 0, allocated); /* Zero buffer so PPA edge interpolation reads black, not heap garbage */
                jpeg_decoder_handle_t decoder = NULL;
                jpeg_decode_engine_cfg_t engine_cfg = { .intr_priority = 0, .timeout_ms = 5000 };
                err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
                if (err == ESP_OK && decoder) {
                    jpeg_decode_cfg_t dec_cfg = {
                        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                    };
                    uint32_t out_size = 0;
                    err = jpeg_decoder_process(decoder, &dec_cfg,
                              jpg_data, jpg_size, rgb_buf, allocated, &out_size);
                    jpeg_del_decoder_engine(decoder);

                    if (err == ESP_OK && out_size > 0) {
                        splash_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
                        splash_dsc.header.w      = (int32_t)out_w;
                        splash_dsc.header.h      = (int32_t)out_h;
                        splash_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
                        splash_dsc.header.stride = out_w * 2;
                        splash_dsc.data          = rgb_buf;
                        splash_dsc.data_size     = out_size;
                        splash_ready = true;
                        ESP_LOGI(TAG, "Splash decoded: %lux%lu", (unsigned long)out_w, (unsigned long)out_h);
                    } else {
                        free(rgb_buf);
                    }
                } else {
                    free(rgb_buf);
                }
            }
        }
    }

    /* Weather client init — creates mutex before dashboard (clock page timer fires immediately) */
    weather_client_init();

    /* ── Build UI: dashboard behind splash, everything starts immediately ── */
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

        create_nina_dashboard(scr, instance_count);

        /* Create splash ON TOP of dashboard — last child draws on top */
        if (splash_ready) {
            lv_obj_t *splash_cont = lv_obj_create(scr);
            lv_obj_remove_style_all(splash_cont);
            lv_obj_set_size(splash_cont, 720, 720);
            lv_obj_set_style_bg_color(splash_cont, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(splash_cont, LV_OPA_COVER, 0);
            lv_obj_center(splash_cont);

            lv_obj_t *img = lv_image_create(splash_cont);
            lv_image_set_src(img, &splash_dsc);
            lv_obj_center(img);

            /* Delayed fade: hold 3 s, then fade out over 500 ms */
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, splash_cont);
            lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
            lv_anim_set_delay(&a, 3000);
            lv_anim_set_duration(&a, 500);
            lv_anim_set_exec_cb(&a, splash_fade_cb);
            lv_anim_set_completed_cb(&a, splash_fade_done);
            lv_anim_start(&a);
        }

        /* Initialize notification overlays (must be after dashboard so they float on top) */
        nina_toast_init(scr);
        nina_event_log_overlay_create(scr);
        nina_alerts_init(scr);
        nina_safety_create(scr);

        {
            /* Apply persisted page override immediately on boot.
             * Override stores absolute page index: 0=allsky, 1=spotify, 2=summary, 3..N+2=NINA, N+3=settings, N+4=sysinfo */
            app_config_t *cfg = app_config_get();
            int total = nina_dashboard_get_total_page_count();
            if (cfg->active_page_override >= 0 && cfg->active_page_override < total) {
                nina_dashboard_show_page(cfg->active_page_override, 0);
            }
        }

        // Restore page from deep sleep if applicable
        if (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
            int saved_page = power_mgmt_get_saved_page();
            ESP_LOGI(TAG, "Restoring page %d from deep sleep", saved_page);
            nina_dashboard_show_page(saved_page, 0);
        }

        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire display lock during init!");
    }

    nina_dashboard_set_page_change_cb(on_page_changed);

    nina_client_init();  // DNS cache mutex — must be called before poll tasks spawn
    nina_client_init_image_buffers();  // Pre-allocate PSRAM image fetch buffer
    nina_thumbnail_init();  // Pre-allocate PSRAM zoom buffer

    /* Spotify init — always called so web handlers (config, login) work even when disabled */
    spotify_auth_init();
    spotify_client_init();

    /* weather_client_init() already called above, before dashboard creation */

    /* Allocate task stacks in PSRAM to save internal heap; TCBs stay internal */
    {
        StackType_t *input_stack = heap_caps_malloc(6144 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *input_tcb  = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (input_stack && input_tcb) {
            xTaskCreateStaticPinnedToCore(input_task, "input_task", 6144, NULL, 5,
                                          input_stack, input_tcb, 0);
        } else {
            ESP_LOGE(TAG, "Failed to alloc input_task stack from PSRAM, falling back");
            if (input_stack) heap_caps_free(input_stack);
            if (input_tcb) heap_caps_free(input_tcb);
            xTaskCreatePinnedToCore(input_task, "input_task", 6144, NULL, 5, NULL, 0);
        }

        StackType_t *data_stack = heap_caps_malloc(12288 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *data_tcb  = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (data_stack && data_tcb) {
            data_task_handle = xTaskCreateStaticPinnedToCore(data_update_task, "data_task", 12288, NULL, 4,
                                                             data_stack, data_tcb, 1);
        } else {
            ESP_LOGE(TAG, "Failed to alloc data_task stack from PSRAM, falling back");
            if (data_stack) heap_caps_free(data_stack);
            if (data_tcb) heap_caps_free(data_tcb);
            xTaskCreatePinnedToCore(data_update_task, "data_task", 12288, NULL, 4, &data_task_handle, 1);
        }
    }

    /* Spotify poll task — only when enabled (Core 0, 10KB PSRAM stack for HTTPS+JSON) */
    if (app_config_get()->spotify_enabled) {
        StackType_t *sp_stack = heap_caps_malloc(10240 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *sp_tcb  = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (sp_stack && sp_tcb) {
            spotify_task_handle = xTaskCreateStaticPinnedToCore(
                spotify_poll_task, "spotify_poll", 10240, NULL, 4,
                sp_stack, sp_tcb, 0);
        } else {
            ESP_LOGE(TAG, "Failed to alloc spotify_poll stack from PSRAM, falling back");
            if (sp_stack) heap_caps_free(sp_stack);
            if (sp_tcb) heap_caps_free(sp_tcb);
            xTaskCreatePinnedToCore(spotify_poll_task, "spotify_poll", 10240, NULL, 4,
                                    &spotify_task_handle, 0);
        }
    }

    /* Weather poll task — always start; task sleeps when no location configured */
    weather_client_start();

    /* Mark this firmware as valid so the bootloader won't roll back.
     * This must come after successful init — if we crash before here,
     * the bootloader will revert to the previous OTA partition. */
    esp_ota_mark_app_valid_cancel_rollback();
}
