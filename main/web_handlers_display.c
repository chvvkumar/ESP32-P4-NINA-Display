#include "web_server_internal.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui/nina_dashboard.h"
#include "ui/themes.h"
#include "lvgl.h"
#include "src/others/snapshot/lv_snapshot.h"
#include "driver/jpeg_encode.h"
#include "mqtt_ha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// Handler for live brightness adjustment (no reboot needed)
esp_err_t brightness_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(val)) {
        int brightness = val->valueint;
        if (brightness < 0) brightness = 0;
        if (brightness > 100) brightness = 100;
        bsp_display_brightness_set(brightness);
        ESP_LOGI(TAG, "Brightness set to %d%%", brightness);
        mqtt_ha_publish_state();
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live color brightness adjustment (no reboot needed)
esp_err_t color_brightness_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "color_brightness");
    if (cJSON_IsNumber(val)) {
        int cb = val->valueint;
        if (cb < 0) cb = 0;
        if (cb > 100) cb = 100;
        app_config_get()->color_brightness = cb;

        // Re-apply theme to update static text brightness
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_apply_theme(app_config_get()->theme_index);
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "Display lock timeout (color brightness)");
        }

        ESP_LOGI(TAG, "Color brightness set to %d%%", cb);
        mqtt_ha_publish_state();
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live theme switching (no reboot needed)
esp_err_t theme_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "theme_index");
    if (cJSON_IsNumber(val)) {
        int idx = val->valueint;
        if (idx < 0) idx = 0;
        if (idx >= themes_get_count()) idx = themes_get_count() - 1;
        app_config_get()->theme_index = idx;
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_apply_theme(idx);
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "Display lock timeout (theme switch)");
        }
        ESP_LOGI(TAG, "Theme set to %d", idx);
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live page switching (saves override and switches immediately)
esp_err_t page_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "page");
    if (cJSON_IsNumber(val)) {
        int page = val->valueint;
        int cnt = app_config_get_instance_count();
        int total = cnt + 3;  /* summary + NINA pages + settings + sysinfo */
        app_config_t *cfg = app_config_get();

        if (page >= 0 && page < total) {
            cfg->active_page_override = (int8_t)page;
            app_config_save(cfg);
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_show_page(page, cnt);
                bsp_display_unlock();
            } else {
                ESP_LOGW(TAG, "Display lock timeout (page switch)");
            }
            ESP_LOGI(TAG, "Page switched to %d via web, override saved", page);
        } else if (page == -1) {
            cfg->active_page_override = -1;
            app_config_save(cfg);
            ESP_LOGI(TAG, "Page override cleared via web");
        }
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for screenshot capture - serves a JPEG image via hardware encoder
esp_err_t screenshot_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Screenshot requested");

    // Take LVGL snapshot while holding the display lock
    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "Failed to acquire display lock for screenshot");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "No active screen");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    lv_draw_buf_t *snapshot = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();

    if (!snapshot || !snapshot->data) {
        ESP_LOGE(TAG, "Snapshot capture failed");
        if (snapshot) lv_draw_buf_destroy(snapshot);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint32_t width = snapshot->header.w;
    uint32_t height = snapshot->header.h;
    uint32_t stride = snapshot->header.stride;
    uint32_t row_size = width * 2;  // RGB565: 2 bytes per pixel
    uint32_t raw_size = row_size * height;

    ESP_LOGI(TAG, "Snapshot captured: %lux%lu, stride=%lu", width, height, stride);

    // Create hardware JPEG encoder FIRST — it needs internal DMA memory for
    // link descriptors which must be allocated before the large data buffers.
    jpeg_encoder_handle_t encoder = NULL;
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    esp_err_t err = jpeg_new_encoder_engine(&engine_cfg, &encoder);
    if (err != ESP_OK || !encoder) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
        lv_draw_buf_destroy(snapshot);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Allocate DMA-aligned input buffer for the JPEG encoder
    jpeg_encode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    size_t in_alloc_size = 0;
    uint8_t *enc_in = (uint8_t *)jpeg_alloc_encoder_mem(raw_size, &in_mem_cfg, &in_alloc_size);
    if (!enc_in) {
        ESP_LOGE(TAG, "Failed to alloc JPEG encoder input buffer (%lu bytes)", raw_size);
        jpeg_del_encoder_engine(encoder);
        lv_draw_buf_destroy(snapshot);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Copy snapshot pixels into the DMA-aligned buffer (handle stride != row_size)
    for (uint32_t y = 0; y < height; y++) {
        memcpy(enc_in + y * row_size, snapshot->data + y * stride, row_size);
    }
    lv_draw_buf_destroy(snapshot);

    // Allocate DMA-aligned output buffer (raw_size is generous upper bound)
    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t out_alloc_size = 0;
    uint8_t *enc_out = (uint8_t *)jpeg_alloc_encoder_mem(raw_size, &out_mem_cfg, &out_alloc_size);
    if (!enc_out) {
        ESP_LOGE(TAG, "Failed to alloc JPEG encoder output buffer");
        jpeg_del_encoder_engine(encoder);
        free(enc_in);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Encode RGB565 -> JPEG
    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = 90,
        .width = width,
        .height = height,
    };
    uint32_t jpg_size = 0;
    err = jpeg_encoder_process(encoder, &enc_cfg,
        enc_in, raw_size, enc_out, out_alloc_size, &jpg_size);
    jpeg_del_encoder_engine(encoder);
    free(enc_in);

    if (err != ESP_OK || jpg_size == 0) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
        free(enc_out);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Screenshot encoded: %lu bytes JPEG (%.1f:1 ratio)",
        jpg_size, (float)raw_size / jpg_size);

    // Send as JPEG image
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.jpg\"");
    httpd_resp_send(req, (const char *)enc_out, jpg_size);

    free(enc_out);
    ESP_LOGI(TAG, "Screenshot sent successfully");
    return ESP_OK;
}
