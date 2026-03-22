/**
 * @file jpeg_utils.c
 * @brief JPEG fetch, hardware decode, and grayscale conversion for thumbnail display.
 */

#include "jpeg_utils.h"
#include "nina_client.h"
#include "ui/nina_dashboard.h"
#include "bsp/esp-bsp.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "perf_monitor.h"
#include "stb_image.h"
#include "esp_cache.h"

static const char *TAG = "jpeg_utils";

/* PPA SRM client for hardware image scaling (lazy-initialized) */
static ppa_client_handle_t s_ppa_srm_client = NULL;

/* Minimum free internal DMA heap required before attempting hardware JPEG decode.
 * The JPEG decoder engine allocates DMA tx/rx link descriptors from internal memory.
 * If we let it try when memory is tight, the ESP-IDF cleanup path crashes on NULL
 * dereference (IDF bug in jpeg_release_codec_handle).  Keep enough headroom for the
 * SDIO WiFi driver which also needs internal DMA buffers. */
#define JPEG_DMA_MIN_FREE_BYTES  (20 * 1024)

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
        // Output dimensions rounded up to 16 — HW decoder always uses 16px MCU alignment
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
            memset(decode_buf, 0, allocated_size); /* Zero buffer so PPA edge interpolation reads black, not heap garbage */
            /* Guard: only create the HW decoder when internal DMA heap has enough
             * headroom.  This avoids both the ESP-IDF crash (buggy cleanup on alloc
             * failure) and starving the SDIO WiFi driver of DMA buffers. */
            size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (free_dma < JPEG_DMA_MIN_FREE_BYTES) {
                ESP_LOGW(TAG, "Skipping HW decode: low DMA heap (%d bytes)", (int)free_dma);
                free(decode_buf);
                free(jpeg_buf);
                return false;
            }

            jpeg_decoder_handle_t decoder = NULL;
            jpeg_decode_engine_cfg_t engine_cfg = {
                .intr_priority = 0,
                .timeout_ms = 5000,
            };
            err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
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

// =============================================================================
// Software JPEG Decode Fallback (stb_image)
// =============================================================================

bool jpeg_sw_decode_rgb565(const uint8_t *jpg_data, size_t jpg_size,
                           uint8_t **out_buf, uint32_t *out_w, uint32_t *out_h,
                           size_t *out_size)
{
    if (!jpg_data || !out_buf || !out_w || !out_h || !out_size) return false;

    int w = 0, h = 0, channels = 0;
    /* Force 3-channel RGB output — stb_image converts CMYK/YCCK internally */
    uint8_t *rgb = stbi_load_from_memory(jpg_data, (int)jpg_size, &w, &h, &channels, 3);
    if (!rgb || w <= 0 || h <= 0) {
        ESP_LOGE(TAG, "stb_image decode failed (channels=%d)", channels);
        if (rgb) stbi_image_free(rgb);
        return false;
    }

    ESP_LOGI(TAG, "SW JPEG decoded: %dx%d (%d ch -> RGB)", w, h, channels);

    /* stb_image outputs exact dimensions — no MCU rounding needed */
    uint32_t ow = (uint32_t)w;
    uint32_t oh = (uint32_t)h;
    size_t buf_sz = (size_t)ow * oh * 2;

    /* 128-byte aligned allocation required for PPA DMA (L2 cache line size) */
    buf_sz = (buf_sz + 127) & ~(size_t)127;
    uint8_t *rgb565 = heap_caps_aligned_calloc(128, 1, buf_sz, MALLOC_CAP_SPIRAM);
    if (!rgb565) {
        ESP_LOGE(TAG, "Failed to alloc %zu bytes for SW decode RGB565", buf_sz);
        stbi_image_free(rgb);
        return false;
    }

    /* Convert RGB888 to RGB565 (BGR byte order to match HW decoder output) */
    uint16_t *dst = (uint16_t *)rgb565;

    for (int y = 0; y < h; y++) {
        const uint8_t *src_row = rgb + y * w * 3;
        uint16_t *dst_row = dst + y * ow;
        for (int x = 0; x < w; x++) {
            uint8_t r = src_row[x * 3 + 0];
            uint8_t g = src_row[x * 3 + 1];
            uint8_t b = src_row[x * 3 + 2];
            /* BGR565 to match JPEG_DEC_RGB_ELEMENT_ORDER_BGR */
            dst_row[x] = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
        }
    }

    stbi_image_free(rgb);

    /* Flush CPU cache to PSRAM so PPA DMA reads correct data */
    esp_cache_msync(rgb565, buf_sz, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    *out_buf = rgb565;
    *out_w = ow;
    *out_h = oh;
    *out_size = buf_sz;
    return true;
}

// =============================================================================
// PPA Hardware Image Scaling
// =============================================================================

uint8_t *ppa_scale_rgb565(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                           uint32_t src_stride,
                           uint32_t dst_w, uint32_t dst_h, size_t *out_size)
{
    if (!src || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return NULL;
    if (src_stride == 0) src_stride = src_w;

    /* Lazy-init PPA SRM client */
    if (!s_ppa_srm_client) {
        ppa_client_config_t cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&cfg, &s_ppa_srm_client) != ESP_OK) {
            ESP_LOGE(TAG, "PPA SRM client registration failed");
            return NULL;
        }
        ESP_LOGI(TAG, "PPA SRM client registered for image scaling");
    }

    /* Output buffer: 128-byte aligned address and size (L2 cache line requirement) */
    size_t buf_size = dst_w * dst_h * 2;  /* RGB565 = 2 bytes/pixel */
    buf_size = (buf_size + 127) & ~(size_t)127;

    uint8_t *dst = heap_caps_aligned_calloc(128, 1, buf_size, MALLOC_CAP_SPIRAM);
    if (!dst) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for PPA output", buf_size);
        return NULL;
    }

    float scale_x = (float)dst_w / (float)src_w;
    float scale_y = (float)dst_h / (float)src_h;

    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = src,
            .pic_w = src_stride,
            .pic_h = src_h,
            .block_w = src_w,
            .block_h = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = buf_size,
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm_client, &srm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA scale %lux%lu -> %lux%lu failed: %s",
                 (unsigned long)src_w, (unsigned long)src_h,
                 (unsigned long)dst_w, (unsigned long)dst_h,
                 esp_err_to_name(err));
        free(dst);
        return NULL;
    }

    if (out_size) *out_size = buf_size;
    if (src_stride != src_w) {
        ESP_LOGI(TAG, "PPA scaled %lux%lu (stride %lu) -> %lux%lu (%.2fx)",
                 (unsigned long)src_w, (unsigned long)src_h, (unsigned long)src_stride,
                 (unsigned long)dst_w, (unsigned long)dst_h, scale_x);
    } else {
        ESP_LOGI(TAG, "PPA scaled %lux%lu -> %lux%lu (%.2fx)",
                 (unsigned long)src_w, (unsigned long)src_h,
                 (unsigned long)dst_w, (unsigned long)dst_h, scale_x);
    }
    return dst;
}

uint8_t *ppa_scale_rgb565_into(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint32_t src_stride,
                                uint32_t dst_w, uint32_t dst_h,
                                uint8_t *dst_buf, size_t dst_buf_size,
                                size_t *out_size)
{
    if (!src || !dst_buf || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return NULL;
    if (src_stride == 0) src_stride = src_w;

    size_t needed = dst_w * dst_h * 2;
    needed = (needed + 127) & ~(size_t)127;
    if (needed > dst_buf_size) {
        ESP_LOGE(TAG, "Pre-allocated buffer too small: need %zu, have %zu", needed, dst_buf_size);
        return NULL;
    }

    /* Lazy-init PPA SRM client */
    if (!s_ppa_srm_client) {
        ppa_client_config_t cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&cfg, &s_ppa_srm_client) != ESP_OK) {
            ESP_LOGE(TAG, "PPA SRM client registration failed");
            return NULL;
        }
        ESP_LOGI(TAG, "PPA SRM client registered for image scaling");
    }

    /* Zero the destination to clear any stale data */
    memset(dst_buf, 0, needed);

    float scale_x = (float)dst_w / (float)src_w;
    float scale_y = (float)dst_h / (float)src_h;

    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = src,
            .pic_w = src_stride,
            .pic_h = src_h,
            .block_w = src_w,
            .block_h = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst_buf,
            .buffer_size = needed,
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm_client, &srm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA scale %lux%lu -> %lux%lu failed: %s",
                 (unsigned long)src_w, (unsigned long)src_h,
                 (unsigned long)dst_w, (unsigned long)dst_h,
                 esp_err_to_name(err));
        return NULL;
    }

    if (out_size) *out_size = needed;
    ESP_LOGI(TAG, "PPA scaled %lux%lu -> %lux%lu into pre-allocated buffer",
             (unsigned long)src_w, (unsigned long)src_h,
             (unsigned long)dst_w, (unsigned long)dst_h);
    return dst_buf;
}
