/**
 * @file jpeg_utils.c
 * @brief JPEG fetch, hardware decode, and grayscale conversion for thumbnail display.
 */

#include "jpeg_utils.h"
#include "nina_client.h"
#include "ui/nina_dashboard.h"
#include "bsp/esp-bsp.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "perf_monitor.h"

static const char *TAG = "jpeg_utils";

bool fetch_and_show_thumbnail(const char *base_url) {
    size_t jpeg_size = 0;
    perf_timer_start(&g_perf.jpeg_fetch);
    uint8_t *jpeg_buf = nina_client_fetch_prepared_image(base_url, 720, 720, 70, &jpeg_size);
    perf_timer_stop(&g_perf.jpeg_fetch);
    if (!jpeg_buf || jpeg_size == 0) {
        return false;
    }

    bool success = false;
    jpeg_decode_picture_info_t pic_info = {0};
    esp_err_t err = jpeg_decoder_get_info(jpeg_buf, jpeg_size, &pic_info);
    if (err == ESP_OK && pic_info.width > 0 && pic_info.height > 0) {
        bool is_gray = (pic_info.sample_method == JPEG_DOWN_SAMPLING_GRAY);
        // Output dimensions rounded up to multiples of 16 (JPEG MCU requirement)
        uint32_t out_w = ((pic_info.width + 15) / 16) * 16;
        uint32_t out_h = ((pic_info.height + 15) / 16) * 16;
        // Grayscale: 1 byte/pixel decode buffer; RGB565: 2 bytes/pixel
        uint32_t decode_buf_size = out_w * out_h * (is_gray ? 1 : 2);

        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t allocated_size = 0;
        uint8_t *decode_buf = (uint8_t *)jpeg_alloc_decoder_mem(decode_buf_size, &mem_cfg, &allocated_size);

        if (decode_buf) {
            jpeg_decoder_handle_t decoder = NULL;
            jpeg_decode_engine_cfg_t engine_cfg = {
                .intr_priority = 0,
                .timeout_ms = 5000,
            };
            err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
                ESP_LOGE(TAG, "Free Internal DMA Heap: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            }
            if (err == ESP_OK && decoder) {
                jpeg_decode_cfg_t decode_cfg = {
                    .output_format = is_gray ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565,
                    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                };
                uint32_t out_size = 0;
                perf_timer_start(&g_perf.jpeg_decode);
                err = jpeg_decoder_process(decoder, &decode_cfg,
                    jpeg_buf, jpeg_size,
                    decode_buf, allocated_size, &out_size);
                perf_timer_stop(&g_perf.jpeg_decode);
                jpeg_del_decoder_engine(decoder);

                if (err == ESP_OK && out_size > 0) {
                    uint8_t *rgb_buf = decode_buf;
                    uint32_t rgb_size = out_size;

                    if (is_gray) {
                        // Convert grayscale to RGB565 for LVGL display
                        uint32_t pixel_count = out_w * out_h;
                        rgb_size = pixel_count * 2;
                        rgb_buf = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
                        if (rgb_buf) {
                            uint16_t *dst = (uint16_t *)rgb_buf;
                            for (uint32_t i = 0; i < pixel_count; i++) {
                                uint8_t g = decode_buf[i];
                                // RGB565: 5-bit R, 6-bit G, 5-bit B
                                dst[i] = ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3);
                            }
                            free(decode_buf);
                            decode_buf = NULL;
                        } else {
                            ESP_LOGE(TAG, "Failed to alloc gray->RGB565 buffer");
                            free(decode_buf);
                            decode_buf = NULL;
                            free(jpeg_buf);
                            return false;
                        }
                    }

                    ESP_LOGI(TAG, "JPEG decoded: %lux%lu %s -> %lu bytes",
                        (unsigned long)pic_info.width, (unsigned long)pic_info.height,
                        is_gray ? "gray" : "color", (unsigned long)rgb_size);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_dashboard_set_thumbnail(rgb_buf, out_w, out_h, rgb_size);
                        bsp_display_unlock();
                        rgb_buf = NULL;  // ownership transferred
                    } else {
                        ESP_LOGW(TAG, "Display lock timeout (thumbnail set)");
                        free(rgb_buf);
                        rgb_buf = NULL;
                    }
                    decode_buf = NULL;
                    success = true;
                } else {
                    ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
                }
            }
            if (decode_buf) free(decode_buf);
        }
    }

    free(jpeg_buf);
    return success;
}
