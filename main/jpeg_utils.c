/**
 * @file jpeg_utils.c
 * @brief Software JPEG decode fallback and PPA hardware scaling helpers.
 */

#include "jpeg_utils.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "stb_image.h"
#include "esp_cache.h"
#include <string.h>

static const char *TAG = "jpeg_utils";

/* PPA SRM client for hardware image scaling (lazy-initialized) */
static ppa_client_handle_t s_ppa_srm_client = NULL;

// =============================================================================
// Software JPEG Decode Fallback (stb_image)
// =============================================================================

bool jpeg_probe_dimensions(const uint8_t *jpg_data, size_t jpg_size,
                           uint32_t *out_w, uint32_t *out_h)
{
    if (!jpg_data || !out_w || !out_h || jpg_size == 0) return false;

    int w = 0, h = 0, comp = 0;
    /* Header-only probe: reads dimensions without decoding pixel data. */
    if (!stbi_info_from_memory(jpg_data, (int)jpg_size, &w, &h, &comp) ||
        w <= 0 || h <= 0) {
        return false;
    }
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    return true;
}

img_fmt_t image_probe_format_dims(const uint8_t *data, size_t size,
                                  uint32_t *out_w, uint32_t *out_h)
{
    /* Sniff + PNG/GIF dimensions are pure byte reads, kept in image_probe.h so
     * they can be host-tested (this file cannot: PPA/cache/stb_image). */
    img_fmt_t fmt = image_probe_sniff(data, size, out_w, out_h);

    /* JPEG is the one format image_probe_sniff() leaves at 0x0: its dimensions
     * need the SOF marker walk, which stb_image does for us. A failed probe
     * still yields IMG_FMT_JPEG with 0x0, same as any short header. */
    if (fmt == IMG_FMT_JPEG) {
        uint32_t w = 0, h = 0;
        if (jpeg_probe_dimensions(data, size, &w, &h)) {
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
        }
    }
    return fmt;
}

/* Format-agnostic despite the name: stbi_load_from_memory() decodes any format
 * stb_image is compiled for (JPEG, PNG, GIF). Kept as jpeg_* to avoid churning
 * every call site.
 *
 * STACK BUDGET: callers must have >= ~10 KB of stack headroom below this call.
 * The deepest chain is PNG (zlib inflate ~4.2 KB + huffman build ~2.6 KB + the
 * IHDR/PLTE frame ~1.4 KB, ~8.2 KB total). The GIF and JPEG decoder contexts
 * (~35 KB and ~18 KB) are heap-allocated, not stack frames: JPEG by upstream
 * stb, GIF by a LOCAL PATCH in stb_image.h dated 2026-08-16 (a device panicked
 * with a hardware stack-protection fault when GIF was enabled and stb's 34 KB
 * `stbi__gif g;` automatic hit a 12288-byte poller task stack). If that patch
 * is ever lost to an stb upgrade, every image poller faults on the first GIF. */
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

    /* Convert RGB888 to standard RGB565 (R high, B low) — same in-memory layout
     * the HW decoder produces, which the panel/LVGL pipeline renders correctly. */
    uint16_t *dst = (uint16_t *)rgb565;

    for (int y = 0; y < h; y++) {
        const uint8_t *src_row = rgb + y * w * 3;
        uint16_t *dst_row = dst + y * ow;
        for (int x = 0; x < w; x++) {
            uint8_t r = src_row[x * 3 + 0];
            uint8_t g = src_row[x * 3 + 1];
            uint8_t b = src_row[x * 3 + 2];
            /* Standard RGB565 (R high, B low). This matches the in-memory layout
             * the panel/LVGL pipeline expects — the same result the HW decoder
             * produces with JPEG_DEC_RGB_ELEMENT_ORDER_BGR. Packing B into the
             * high bits here swaps red and blue (sodium lights render blue). */
            dst_row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
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

// =============================================================================
// Software Bilinear Scaling
// =============================================================================

void sw_scale_rgb565_bilinear(const uint16_t *src, int sw, int sh,
                              uint16_t *dst, int dw, int dh)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    /* 16.16 fixed point. Centre-aligned sample positions
     * (src = (d + 0.5) * scale - 0.5) so the output is not shifted by half a
     * source pixel the way edge-aligned mapping is. */
    const uint32_t step_x = ((uint32_t)sw << 16) / (uint32_t)dw;
    const uint32_t step_y = ((uint32_t)sh << 16) / (uint32_t)dh;
    const int32_t  base_x = (int32_t)(step_x >> 1) - 32768;
    int32_t        pos_y  = (int32_t)(step_y >> 1) - 32768;

    for (int y = 0; y < dh; y++, pos_y += (int32_t)step_y) {
        int32_t py = pos_y < 0 ? 0 : pos_y;
        int y0 = py >> 16;
        if (y0 > sh - 1) y0 = sh - 1;
        int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        /* 8-bit blend weights: products stay well inside 32 bits. */
        uint32_t wy = ((uint32_t)py >> 8) & 0xFF;
        uint32_t iy = 256 - wy;

        const uint16_t *row0 = src + (size_t)y0 * (size_t)sw;
        const uint16_t *row1 = src + (size_t)y1 * (size_t)sw;
        uint16_t *out = dst + (size_t)y * (size_t)dw;
        int32_t pos_x = base_x;

        for (int x = 0; x < dw; x++, pos_x += (int32_t)step_x) {
            int32_t px = pos_x < 0 ? 0 : pos_x;
            int x0 = px >> 16;
            if (x0 > sw - 1) x0 = sw - 1;
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            uint32_t wx = ((uint32_t)px >> 8) & 0xFF;
            uint32_t ix = 256 - wx;

            uint32_t p00 = row0[x0], p01 = row0[x1];
            uint32_t p10 = row1[x0], p11 = row1[x1];

            /* Interpolate each RGB565 channel at its own bit depth, then
             * repack — blending the packed words directly would bleed carries
             * from blue into green into red. */
            uint32_t top = ((p00 >> 11) & 0x1F) * ix + ((p01 >> 11) & 0x1F) * wx;
            uint32_t bot = ((p10 >> 11) & 0x1F) * ix + ((p11 >> 11) & 0x1F) * wx;
            uint32_t r   = (top * iy + bot * wy) >> 16;

            top = ((p00 >> 5) & 0x3F) * ix + ((p01 >> 5) & 0x3F) * wx;
            bot = ((p10 >> 5) & 0x3F) * ix + ((p11 >> 5) & 0x3F) * wx;
            uint32_t g = (top * iy + bot * wy) >> 16;

            top = (p00 & 0x1F) * ix + (p01 & 0x1F) * wx;
            bot = (p10 & 0x1F) * ix + (p11 & 0x1F) * wx;
            uint32_t b = (top * iy + bot * wy) >> 16;

            out[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

/* Shared core for the two into-a-buffer scalers. `clear_dst` zeroes the
 * destination before the transfer (needed when the output is not fully covered,
 * e.g. a non-integer ratio leaves a remainder strip); `quiet` suppresses the
 * per-call INFO log (the moon drag loop calls this every frame). */
static uint8_t *ppa_scale_rgb565_into_core(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                           uint32_t src_stride,
                                           uint32_t dst_w, uint32_t dst_h,
                                           uint8_t *dst_buf, size_t dst_buf_size,
                                           size_t *out_size, bool clear_dst, bool quiet)
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

    /* Zero the destination to clear any stale data. Skippable when the caller
     * guarantees the transfer overwrites every output pixel (exact integer
     * ratio), avoiding a ~1MB/frame memset in the moon drag path. */
    if (clear_dst) memset(dst_buf, 0, needed);

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
    if (!quiet) {
        ESP_LOGI(TAG, "PPA scaled %lux%lu -> %lux%lu into pre-allocated buffer",
                 (unsigned long)src_w, (unsigned long)src_h,
                 (unsigned long)dst_w, (unsigned long)dst_h);
    }
    return dst_buf;
}

uint8_t *ppa_scale_rgb565_into(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint32_t src_stride,
                                uint32_t dst_w, uint32_t dst_h,
                                uint8_t *dst_buf, size_t dst_buf_size,
                                size_t *out_size)
{
    /* Clears the destination (existing GOES/thumbnail callers rely on this for
     * non-integer ratios). */
    return ppa_scale_rgb565_into_core(src, src_w, src_h, src_stride,
                                      dst_w, dst_h, dst_buf, dst_buf_size,
                                      out_size, true, false);
}

uint8_t *ppa_scale_rgb565_into_noclear(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                        uint32_t src_stride,
                                        uint32_t dst_w, uint32_t dst_h,
                                        uint8_t *dst_buf, size_t dst_buf_size,
                                        size_t *out_size)
{
    /* No destination memset and no per-call INFO log: for the moon drag loop,
     * which scales at an EXACT integer ratio (240->720 = 3.0x) so every output
     * pixel is written, and calls this every frame. */
    return ppa_scale_rgb565_into_core(src, src_w, src_h, src_stride,
                                      dst_w, dst_h, dst_buf, dst_buf_size,
                                      out_size, false, true);
}
