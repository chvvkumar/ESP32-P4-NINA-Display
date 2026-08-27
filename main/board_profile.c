#include "board_profile.h"
#include "board_detect.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "board";

#define BOARD_NVS_NAMESPACE  "board"
#define BOARD_NVS_PANEL_KEY  "panel"
#define BOARD_PANEL_3_4      1
#define BOARD_PANEL_4C       2

static const board_profile_t s_profiles[] = {
    { .id = "4b",   .ha_model = "ESP32-P4-WIFI6-Touch-LCD-4B",
      .panel_type = BSP_PANEL_SQUARE_4B, .width = 720,
      .is_round = false, .safe_inset = 0,   .safe_radius = 0   },
    { .id = "3.4c", .ha_model = "ESP32-P4-WIFI6-Touch-LCD-3.4C",
      .panel_type = BSP_PANEL_ROUND_3_4,  .width = 800,
      .is_round = true,  .safe_inset = 118, .safe_radius = 400 },
    { .id = "4c",   .ha_model = "ESP32-P4-WIFI6-Touch-LCD-4C",
      .panel_type = BSP_PANEL_ROUND_4C,   .width = 720,
      .is_round = true,  .safe_inset = 105, .safe_radius = 360 },
};

static const board_profile_t *s_profile = &s_profiles[0];
static bool s_present = false;
static bool s_done    = false;

int board_panel_nvs_get(void)
{
    nvs_handle_t h;
    if (nvs_open(BOARD_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return BOARD_PANEL_3_4;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, BOARD_NVS_PANEL_KEY, &v);
    nvs_close(h);
    if (err != ESP_OK || (v != BOARD_PANEL_3_4 && v != BOARD_PANEL_4C)) {
        return BOARD_PANEL_3_4;
    }
    return (int)v;
}

esp_err_t board_panel_nvs_set(int value)
{
    if (value != BOARD_PANEL_3_4 && value != BOARD_PANEL_4C) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOARD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, BOARD_NVS_PANEL_KEY, (uint8_t)value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Pending images that cannot drive the panel they found do not wait for a
 * health check that will never fail: nothing marks the slot invalid, and the
 * confirm guard's uptime fallback would otherwise validate a blind image five
 * minutes in. Rolling back here brings the previous family's image straight
 * back. A factory flash of the wrong family is not pending, so it stays up
 * headless and the user can upload the right image over the web. */
static void rollback_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "wrong-family image is pending verify, rolling back now");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* does not return */
    }
    ESP_LOGE(TAG, "booting headless: web server and console stay up so the "
                  "correct firmware family can be uploaded");
}

void board_profile_init(void)
{
    if (s_done) {
        return;
    }
    s_done = true;

#if CONFIG_NINA_FAMILY_ROUND
    const int panel = board_panel_nvs_get();
    s_profile = (panel == BOARD_PANEL_4C) ? &s_profiles[2] : &s_profiles[1];
    const board_controller_t expect = BOARD_CTRL_JD9365;
#else
    s_profile = &s_profiles[0];
    const board_controller_t expect = BOARD_CTRL_ST7703;
#endif

    /* Width first: screenshot_encoder_init() and every geometry consumer read
     * it before display start runs. */
    bsp_display_set_panel_type(s_profile->panel_type);

    uint8_t id[3] = {0, 0, 0};
    esp_err_t err = bsp_display_probe_rddid(id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RDDID probe failed (%s), display stays off", esp_err_to_name(err));
        s_present = false;
        rollback_if_pending();
        return;
    }
    /* Logged unconditionally and before the match: capturing these three bytes
     * on real hardware is a phase 1 deliverable, not a debug aid. */
    ESP_LOGW(TAG, "RDDID %02X %02X %02X", id[0], id[1], id[2]);

    size_t count = 0;
    const board_rddid_entry_t *table = board_detect_table(&count);
    const board_controller_t got = board_detect_controller(table, count, id);

    if (got != expect) {
        ESP_LOGE(TAG, "panel controller %s does not match this %s-family binary "
                      "(expected %s)",
                 board_detect_controller_name(got), board_profile_shape(),
                 board_detect_controller_name(expect));
        s_present = false;
        rollback_if_pending();
        return;
    }

    s_present = true;
    ESP_LOGI(TAG, "board %s, shape %s, panel %dx%d, controller %s",
             s_profile->id, board_profile_shape(), s_profile->width,
             s_profile->width, board_detect_controller_name(got));
}

const board_profile_t *board_profile(void) { return s_profile; }
bool board_display_present(void)           { return s_present; }

const char *board_profile_id(void)
{
    return s_present ? s_profile->id : "unknown";
}

const char *board_profile_shape(void)
{
#if CONFIG_NINA_FAMILY_ROUND
    return "round";
#else
    return "square";
#endif
}
