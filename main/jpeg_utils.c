/**
 * @file jpeg_utils.c
 * @brief Software JPEG decode fallback and PPA hardware scaling helpers.
 */

#include "jpeg_utils.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "stb_image.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "jpeg_utils";

/* One PPA SRM client shared by every scaler here. max_pending_trans_num = 4:
 * four tasks on two cores can be inside a blocking scaler at once (thumbnail
 * zoom on the LVGL task, Spotify art, the image-page pollers, the moon drag
 * loop) and ppa_do_scale_rotate_mirror() returns ESP_FAIL to any caller that
 * cannot take a free transaction element (ppa_srm.c:307-310). */
/* Largest SRM output block per axis the engine accepts without wedging (13-bit
 * output field); ppa_core.c waits portMAX_DELAY and never sees the error. */
#define PPA_SRM_OUT_MAX_PX 8191u

static ppa_client_handle_t s_ppa_srm_client = NULL;
static portMUX_TYPE s_ppa_srm_mux = portMUX_INITIALIZER_UNLOCKED;

/* The single registration site. Registration happens OUTSIDE the spinlock (it
 * allocates and creates a queue); the loser of a first-call race unregisters
 * its own handle instead of leaking it. */
static ppa_client_handle_t ppa_srm_client_ensure(void)
{
    portENTER_CRITICAL(&s_ppa_srm_mux);
    ppa_client_handle_t h = s_ppa_srm_client;
    portEXIT_CRITICAL(&s_ppa_srm_mux);
    if (h) return h;

    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 4,
    };
    ppa_client_handle_t mine = NULL;
    if (ppa_register_client(&cfg, &mine) != ESP_OK || !mine) {
        ESP_LOGE(TAG, "PPA SRM client registration failed");
        return NULL;
    }

    portENTER_CRITICAL(&s_ppa_srm_mux);
    if (!s_ppa_srm_client) {
        s_ppa_srm_client = mine;
        mine = NULL;
    }
    h = s_ppa_srm_client;
    portEXIT_CRITICAL(&s_ppa_srm_mux);

    if (mine) ppa_unregister_client(mine);   /* lost the race */
    return h;
}

/* One PPA Blend client, registered the same lazy way as the SRM one above.
 * max_pending_trans_num = 2: the image pages' playback dissolve runs on the
 * LVGL task and is the only caller today; the spare slot keeps a second caller
 * from being refused outright (ppa_blend.c returns ESP_FAIL when the client has
 * no free transaction element). */
static ppa_client_handle_t s_ppa_blend_client = NULL;
static portMUX_TYPE s_ppa_blend_mux = portMUX_INITIALIZER_UNLOCKED;

static ppa_client_handle_t ppa_blend_client_ensure(void)
{
    portENTER_CRITICAL(&s_ppa_blend_mux);
    ppa_client_handle_t h = s_ppa_blend_client;
    portEXIT_CRITICAL(&s_ppa_blend_mux);
    if (h) return h;

    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 2,
    };
    ppa_client_handle_t mine = NULL;
    if (ppa_register_client(&cfg, &mine) != ESP_OK || !mine) {
        ESP_LOGE(TAG, "PPA blend client registration failed");
        return NULL;
    }

    portENTER_CRITICAL(&s_ppa_blend_mux);
    if (!s_ppa_blend_client) {
        s_ppa_blend_client = mine;
        mine = NULL;
    }
    h = s_ppa_blend_client;
    portEXIT_CRITICAL(&s_ppa_blend_mux);

    if (mine) ppa_unregister_client(mine);   /* lost the race */
    return h;
}

void jpeg_utils_ppa_init(void)
{
    if (ppa_srm_client_ensure()) {
        ESP_LOGI(TAG, "PPA SRM client registered (4 pending transactions)");
    }
    if (ppa_blend_client_ensure()) {
        ESP_LOGI(TAG, "PPA blend client registered (2 pending transactions)");
    }
}

esp_err_t ppa_blend_rgb565(const uint8_t *bg, const uint8_t *fg, uint8_t *out,
                           uint32_t w, uint32_t h, uint8_t fg_alpha)
{
    if (!bg || !fg || !out) return ESP_ERR_INVALID_ARG;
    /* A block of 0 or over 8191 px per axis raises an engine error the driver
     * never services (no error IRQ; ppa_core.c waits portMAX_DELAY), so a
     * blocking caller would hang forever. Refuse instead. */
    if (w == 0 || h == 0 || w > PPA_SRM_OUT_MAX_PX || h > PPA_SRM_OUT_MAX_PX) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* The driver checks the output ADDRESS and the declared buffer size against
     * the cache line (128 B on this build); it cannot check the allocation, so
     * the contract in the header says the caller owns that. */
    if (((uintptr_t)out & 127u) != 0) return ESP_ERR_INVALID_ARG;
    size_t need = (((size_t)w * h * 2) + 127) & ~(size_t)127;

    ppa_client_handle_t client = ppa_blend_client_ensure();
    if (!client) return ESP_ERR_INVALID_STATE;

    /* RGB565 inputs have no alpha channel, so the hardware fills A = 255 for
     * both. Leaving the FG at that would make the blend a plain copy of the FG
     * (ppa.rst); PPA_ALPHA_FIX_VALUE is what turns it into
     * out = bg*(1 - a) + fg*a with a = fg_alpha/255. */
    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = bg,
            .pic_w = w, .pic_h = h,
            .block_w = w, .block_h = h,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = fg,
            .pic_w = w, .pic_h = h,
            .block_w = w, .block_h = h,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = out,
            .buffer_size = need,
            .pic_w = w, .pic_h = h,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .fg_alpha_fix_val = fg_alpha,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_blend(client, &cfg);
}

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

    /* Convert RGB888 to standard RGB565 (R high, B low) -- same in-memory layout
     * the HW decoder produces, which the panel/LVGL pipeline renders correctly.
     * The PPA does the whole picture in one DMA pass; the loop below is the
     * fallback for when the hardware refuses it. */
    if (!ppa_rgb888_to_rgb565(rgb, ow, oh, rgb565, buf_sz)) {
        uint16_t *dst = (uint16_t *)rgb565;

        for (int y = 0; y < h; y++) {
            const uint8_t *src_row = rgb + y * w * 3;
            uint16_t *dst_row = dst + y * ow;
            for (int x = 0; x < w; x++) {
                uint8_t r = src_row[x * 3 + 0];
                uint8_t g = src_row[x * 3 + 1];
                uint8_t b = src_row[x * 3 + 2];
                /* Standard RGB565 (R high, B low). This matches the in-memory
                 * layout the panel/LVGL pipeline expects -- the same result the
                 * HW decoder produces with JPEG_DEC_RGB_ELEMENT_ORDER_BGR.
                 * Packing B into the high bits here swaps red and blue (sodium
                 * lights render blue). */
                dst_row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }

    stbi_image_free(rgb);

    *out_buf = rgb565;
    *out_w = ow;
    *out_h = oh;
    *out_size = buf_sz;
    return true;
}

// =============================================================================
// Hardware JPEG Decode (esp_driver_jpeg) with software fallback
// =============================================================================

/* One HW decode attempt. Returns ESP_OK (caller owns *out_buf) or the esp_err
 * that says why HW cannot do this picture, so the caller falls back to stb.
 *
 * Peak PSRAM is the whole point: the HW path needs only the padded RGB565
 * output (~1.04 MB at 720x720), where stb needs an RGB888 intermediate
 * (~1.55 MB) live at the same time as that RGB565 buffer.
 *
 * Driver contract (esp-idf 5.5.2 components/esp_driver_jpeg/jpeg_decode.c):
 *  - input buffer: no alignment requirement (comment at jpeg_decode.c:326-328),
 *    a plain PSRAM heap buffer is fine;
 *  - output buffer: address AND size must be cache-aligned, checked at
 *    jpeg_decode.c:211 -- hence jpeg_alloc_decoder_mem(), which aligns to the
 *    L2 line (128 B here) and reports the rounded-up size;
 *  - jpeg_decoder_process() serializes on a shared codec mutex
 *    (jpeg_decode.c:219, portMAX_DELAY), so a concurrent decode elsewhere
 *    (the thumbnail fetch worker) waits rather than failing;
 *  - progressive/CMYK/odd sampling are refused by the header parse or by
 *    jpeg_decoder_process() with ESP_ERR_NOT_SUPPORTED. */
/* Minimal baseline-JPEG header scan: walks the marker chain up to SOF0 and
 * reads the frame size, component count and the Y sampling factors. Every read
 * is bounds-checked. Returns false for anything that is not a plain SOF0
 * baseline picture (progressive SOF2, arithmetic, 12-bit, truncated header).
 *
 * WHY not jpeg_decoder_get_info(): IDF 5.5.2's copy leaks its ~3.3 KB
 * header_info allocation (internal RAM) on the "sampling factor cannot be
 * recognized" path, and it hardcodes nothing about the MCU size we need for
 * the repack below. This scan gives us both without the driver allocating. */
static bool jpeg_scan_sof0(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h,
                           uint8_t *nf, uint8_t *hi0, uint8_t *vi0)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) return false;
        uint8_t m = d[i + 1];
        if (m == 0xFF) { i++; continue; }              /* fill byte */
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        size_t seg = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (seg < 2 || i + 2 + seg > n) return false;
        if (m == 0xC0) {                                /* SOF0: baseline */
            const uint8_t *f = d + i + 4;               /* after length */
            if (seg < 8 || f[0] != 8) return false;     /* 8-bit precision only */
            *h  = ((uint32_t)f[1] << 8) | f[2];
            *w  = ((uint32_t)f[3] << 8) | f[4];
            *nf = f[5];
            if (*nf < 1 || *nf > 3 || seg < 8u + 3u * *nf) return false;
            *hi0 = f[7] >> 4;
            *vi0 = f[7] & 0x0F;
            return *w > 0 && *h > 0;
        }
        if (m == 0xDA || (m >= 0xC1 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)) {
            return false;                               /* SOS before SOF0, or a non-baseline SOFn */
        }
        i += 2 + seg;
    }
    return false;
}

/* 4x4 Bayer thresholds for the GRAY8 -> RGB565 expansion below. A NINA
 * prepared thumbnail is a grey JPEG whose stretched sky spans about 15 grey
 * levels; plain truncation collapses that to 2 red/blue steps and 4 green
 * steps and the disc shows tinted concentric bands (seen on the 4C, hidden
 * on a star-noisy frame). The offsets run 0..7 (5-bit channels) and 0..3
 * (6-bit), so the rounded-down result averages to the source value instead
 * of sitting half a quantum dark. */
static const uint8_t s_bayer4[4][4] = {
    { 0,  8,  2, 10 },
    { 12, 4, 14,  6 },
    { 3, 11,  1,  9 },
    { 15, 7, 13,  5 },
};

static esp_err_t jpeg_hw_decode_rgb565(const uint8_t *jpg, size_t len,
                                       uint8_t **out_buf, uint32_t *out_w,
                                       uint32_t *out_h, size_t *out_size)
{
    uint32_t w = 0, h = 0;
    uint8_t nf = 0, hi0 = 0, vi0 = 0;
    if (!jpeg_scan_sof0(jpg, len, &w, &h, &nf, &hi0, &vi0)) return ESP_ERR_NOT_SUPPORTED;
    /* Single-component JPEG decodes to GRAY8 and is expanded to RGB565 below --
     * far cheaper than handing a 3-6 MP mono camera frame to stb. For 3
     * components the HW accepts exactly 4:4:4 (1x1), 4:2:2 (2x1) and 4:2:0
     * (2x2) (jpeg_decode.c get_info switch); anything else goes to stb. */
    const bool is_gray = (nf == 1);
    if (nf == 2) return ESP_ERR_NOT_SUPPORTED;
    if (is_gray) {
        /* Any sane sampling factor: the pad below only needs hi0/vi0 non-zero. */
        if (hi0 < 1 || hi0 > 4 || vi0 < 1 || vi0 > 4) return ESP_ERR_NOT_SUPPORTED;
    } else if (!((hi0 == 1 && vi0 == 1) || (hi0 == 2 && vi0 == 1) ||
                 (hi0 == 2 && vi0 == 2))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((w * h) % 8 != 0) return ESP_ERR_NOT_SUPPORTED;   /* driver's own SOF check */

    /* The decode engine's DMA descriptors come from the internal DMA heap. */
    if (heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) < 20 * 1024) {
        return ESP_ERR_NO_MEM;
    }

    /* HW writes whole MCUs: the row stride is the width padded to the Y MCU
     * width (hi0*8: 8 for 4:4:4, 16 for 4:2:2/4:2:0) and the height to the Y
     * MCU height (vi0*8) -- jpeg_parse_marker.c jpeg_parse_sof_marker(). A
     * fixed 16 would shear every 4:4:4 picture whose width is 8 mod 16. */
    uint32_t mcu_w = (uint32_t)hi0 * 8, mcu_h = (uint32_t)vi0 * 8;
    uint32_t pad_w = ((w + mcu_w - 1) / mcu_w) * mcu_w;
    uint32_t pad_h = ((h + mcu_h - 1) / mcu_h) * mcu_h;

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_size = 0;
    uint8_t *buf = jpeg_alloc_decoder_mem((size_t)pad_w * pad_h * (is_gray ? 1 : 2),
                                          &mem_cfg, &alloc_size);
    if (!buf) return ESP_ERR_NO_MEM;

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = { .intr_priority = 0, .timeout_ms = 5000 };
    esp_err_t err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (err != ESP_OK || !decoder) {
        heap_caps_free(buf);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    /* DMA discipline: jpeg_alloc_decoder_mem() callocs the buffer through the
     * CPU, which leaves up to L1+L2 (~320 KB) of DIRTY zero lines behind. The
     * driver only invalidates (M2C) before and after the transfer; a dirty zero
     * line that survives and is evicted after the DMA overwrites decoded pixels
     * in PSRAM with black. Seen live on dash4: every HW-decoded Cloud Cover
     * frame read back as >3% pure black and was rejected. Write the zeros back
     * NOW so no dirty line outlives the transfer. */
    esp_cache_msync(buf, alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* BGR element order gives the same in-memory RGB565 layout the SW path
     * produces (R high, B low) -- see the packing comment above. */
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = is_gray ? JPEG_DECODE_OUT_FORMAT_GRAY
                                 : JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t decoded_size = 0;
    err = jpeg_decoder_process(decoder, &decode_cfg, jpg, (uint32_t)len,
                               buf, (uint32_t)alloc_size, &decoded_size);
    jpeg_del_decoder_engine(decoder);
    if (err != ESP_OK || decoded_size == 0) {
        heap_caps_free(buf);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    /* Diagnostic: fraction of sampled pixels exactly 0x0000 in the raw DMA
     * output. Confirmed 2026-08-24 on dash4: the HW colour conversion crushes
     * dark values to 0 (7-8 % of a night GeoColor frame vs <1 % from stb),
     * which is why clouds_frame_incomplete() tests NEAR black. Debug only. */
    if (!is_gray) {
        const uint16_t *px = (const uint16_t *)buf;
        uint32_t n = 0, z = 0;
        for (uint32_t y = 0; y < h; y += 8) {
            for (uint32_t x = 0; x < w; x += 8) {
                n++;
                if (px[(size_t)y * pad_w + x] == 0) z++;
            }
        }
        ESP_LOGD(TAG, "HW JPEG raw: %lu/%lu samples zero, px[0]=%04x mid=%04x",
                 (unsigned long)z, (unsigned long)n, px[0],
                 px[(size_t)(h / 2) * pad_w + w / 2]);
    }

    /* GRAY8 -> RGB565 into a tight w*h*2 buffer (the padded GRAY8 one is freed).
     * The expansion is ordered-dithered with s_bayer4 above, not truncated. */
    if (is_gray) {
        size_t rgb_sz = (((size_t)w * h * 2) + 127) & ~(size_t)127;
        uint8_t *rgb = heap_caps_aligned_calloc(128, 1, rgb_sz, MALLOC_CAP_SPIRAM);
        if (!rgb) {
            heap_caps_free(buf);
            return ESP_ERR_NO_MEM;
        }
        uint16_t *d = (uint16_t *)rgb;
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *srow = buf + (size_t)y * pad_w;
            const uint8_t *th   = s_bayer4[y & 3];
            uint16_t *drow = d + (size_t)y * w;
            for (uint32_t x = 0; x < w; x++) {
                int g  = srow[x];
                int t  = th[x & 3];
                int r5 = g + (t >> 1);          /* +0..7 before the >> 3 */
                int g6 = g + (t >> 2);          /* +0..3 before the >> 2 */
                if (r5 > 255) r5 = 255;
                if (g6 > 255) g6 = 255;
                drow[x] = (uint16_t)(((r5 >> 3) << 11) | ((g6 >> 2) << 5) | (r5 >> 3));
            }
        }
        heap_caps_free(buf);
        *out_buf = rgb;
        *out_w = w;
        *out_h = h;
        *out_size = rgb_sz;
        return ESP_OK;
    }

    /* Callers expect tightly packed w*h*2 rows, so drop the MCU pad in place.
     * Destination row y starts at or before source row y, and we walk top-down,
     * so no row is overwritten before it is copied. */
    if (pad_w != w) {
        for (uint32_t y = 1; y < h; y++) {
            memmove(buf + (size_t)y * w * 2,
                    buf + (size_t)y * pad_w * 2,
                    (size_t)w * 2);
        }
    }

    /* No write-back here: the CPU readers (blank-frame check, LVGL's software
     * renderer) go through the cache, and the PPA scalers' driver C2Ms its own
     * input window before every transfer (ppa_srm.c:250). */

    *out_buf = buf;
    *out_w = w;
    *out_h = h;
    *out_size = alloc_size;
    return ESP_OK;
}

bool jpeg_decode_rgb565(const uint8_t *jpg, size_t len, uint8_t **out_buf,
                        uint32_t *out_w, uint32_t *out_h, size_t *out_size)
{
    if (!jpg || !out_buf || !out_w || !out_h || !out_size || len == 0) return false;

    /* Only JPEG goes near the HW engine: PNG/GIF payloads (radar tiles) are
     * stb's job and must not log a HW failure every frame. */
    if (len >= 3 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[2] == 0xFF) {
        esp_err_t err = jpeg_hw_decode_rgb565(jpg, len, out_buf, out_w, out_h, out_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HW JPEG decoded %lux%lu",
                     (unsigned long)*out_w, (unsigned long)*out_h);
            return true;
        }
        ESP_LOGI(TAG, "HW JPEG decode skipped (%s), using software decode",
                 esp_err_to_name(err));
    }
    return jpeg_sw_decode_rgb565(jpg, len, out_buf, out_w, out_h, out_size);
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

    ppa_client_handle_t client = ppa_srm_client_ensure();
    if (!client) return NULL;

    /* Output buffer: 128-byte aligned address and size (L2 cache line requirement) */
    size_t buf_size = dst_w * dst_h * 2;  /* RGB565 = 2 bytes/pixel */
    buf_size = (buf_size + 127) & ~(size_t)127;

    uint8_t *dst = heap_caps_aligned_calloc(128, 1, buf_size, MALLOC_CAP_SPIRAM);
    if (!dst) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for PPA output", buf_size);
        return NULL;
    }
    /* Write the CPU's zero lines back before the driver's M2C invalidate of the
     * output window (ppa_srm.c:256) discards them and they later evict onto the
     * DMA'd pixels -- the hazard the HW JPEG path proved live on dash4. */
    esp_cache_msync(dst, buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

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

    {
        /* Same engine-hang guard as ppa_srm_rgb565(): the driver truncates the
         * scale to 1/16 steps, so predict the written block the way it does. */
        float sx16 = (float)((uint32_t)(scale_x * 16.0f)) / 16.0f;
        float sy16 = (float)((uint32_t)(scale_y * 16.0f)) / 16.0f;
        uint32_t ow = (uint32_t)(sx16 * (float)src_w), oh = (uint32_t)(sy16 * (float)src_h);
        if (ow == 0 || oh == 0 || ow > PPA_SRM_OUT_MAX_PX || oh > PPA_SRM_OUT_MAX_PX) {
            ESP_LOGE(TAG, "PPA scale %lux%lu -> %lux%lu refused: output block %lux%lu",
                     (unsigned long)src_w, (unsigned long)src_h,
                     (unsigned long)dst_w, (unsigned long)dst_h,
                     (unsigned long)ow, (unsigned long)oh);
            free(dst);
            return NULL;
        }
    }
    esp_err_t err = ppa_do_scale_rotate_mirror(client, &srm);
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

bool ppa_rgb888_to_rgb565(const uint8_t *rgb888, uint32_t w, uint32_t h,
                          uint8_t *dst565, size_t dst_size)
{
    if (!rgb888 || !dst565 || w == 0 || h == 0) return false;
    /* 2D-DMA descriptor fields are 14-bit; a wider picture would silently
     * truncate, so hand those to the software loop instead. */
    if (w > PPA_SRM_OUT_MAX_PX || h > PPA_SRM_OUT_MAX_PX) return false;  /* see ppa_srm_rgb565 */
    /* The driver rejects an unaligned output address or size outright
     * (ppa_srm.c:178-181); check here so the fallback runs without an ERROR. */
    if (((uintptr_t)dst565 & 127u) != 0 || (dst_size & 127u) != 0) return false;
    if ((size_t)w * h * 2 > dst_size) return false;

    ppa_client_handle_t client = ppa_srm_client_ensure();
    if (!client) return false;

    /* Same dirty-zero-line write-back as the scalers above: the caller's buffer
     * came from a calloc and the driver only invalidates the output window. */
    esp_cache_msync(dst565, dst_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = rgb888,
            .pic_w = w,
            .pic_h = h,
            .block_w = w,
            .block_h = h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = dst565,
            .buffer_size = dst_size,
            .pic_w = w,
            .pic_h = h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        /* Byte order. hal/color_types.h:187-194 lays out the PPA's RGB888 pixel
         * as b,g,r -- byte 0 is BLUE -- while stb writes r,g,b, so the input
         * triplet needs the swap (ppa.h:174 "RGB becomes BGR"). The RGB565
         * output word is (r<<11)|(g<<5)|b (color_types.h:199-206), which is
         * exactly what the software pack and the HW JPEG BGR path produce, so
         * no byte_swap and no change for LVGL. */
        .rgb_swap = true,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_scale_rotate_mirror(client, &srm) == ESP_OK;
}

esp_err_t ppa_srm_rgb565(ppa_srm_job_t *job)
{
    if (!job || !job->src || !job->dst) return ESP_ERR_INVALID_ARG;
    if (job->scale_n16 == 0 || job->src_stride_px == 0 || job->src_h == 0 ||
        job->block_w == 0 || job->block_h == 0 ||
        job->dst_w == 0 || job->dst_h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->src_stride_px > 16383u || job->src_h > 16383u ||
        job->dst_w > 16383u || job->dst_h > 16383u) {
        return ESP_ERR_INVALID_ARG;   /* 2D-DMA descriptor fields are 14-bit */
    }
    /* Source block inside the source picture. */
    if (job->block_x + job->block_w > job->src_stride_px ||
        job->block_y + job->block_h > job->src_h) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t need = (((size_t)job->dst_w * job->dst_h * 2) + 127) & ~(size_t)127;
    if (job->dst_buf_size < need) return ESP_ERR_INVALID_SIZE;

    /* The driver enum counts COUNTER-clockwise (hal/ppa_types.h:29-32), the page
     * convention is clockwise: 90 CW == 270 CCW, 270 CW == 90 CCW. */
    static const ppa_srm_rotation_angle_t k_cw_to_ccw[4] = {
        PPA_SRM_ROTATION_ANGLE_0,
        PPA_SRM_ROTATION_ANGLE_270,
        PPA_SRM_ROTATION_ANGLE_180,
        PPA_SRM_ROTATION_ANGLE_90,
    };
    ppa_srm_rotation_angle_t angle = k_cw_to_ccw[job->rotate_cw & 3];

    /* Exact 1/16 step, so scale_x_frag reproduces scale_n16 & 15 verbatim
     * (ppa_srm.c:276-279) and out_w/out_h below are what the hardware writes. */
    float scale = (float)job->scale_n16 / 16.0f;

    uint32_t ow, oh;
    if (angle == PPA_SRM_ROTATION_ANGLE_0 || angle == PPA_SRM_ROTATION_ANGLE_180) {
        ow = (uint32_t)(scale * (float)job->block_w);
        oh = (uint32_t)(scale * (float)job->block_h);
    } else {
        /* 90/270 swap the block axes (ppa_srm.c:215-222). */
        ow = (uint32_t)(scale * (float)job->block_h);
        oh = (uint32_t)(scale * (float)job->block_w);
    }
    if (job->dst_x + ow > job->dst_w || job->dst_y + oh > job->dst_h) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* An output block of 0 or above 8191 px per axis makes the SRM engine raise
     * an error the IDF driver never services (no error IRQ, portMAX_DELAY in
     * ppa_core.c), so a blocking caller would hang forever. Refuse it here. */
    if (ow == 0 || oh == 0 || ow > PPA_SRM_OUT_MAX_PX || oh > PPA_SRM_OUT_MAX_PX) {
        return ESP_ERR_INVALID_SIZE;
    }

    ppa_client_handle_t client = ppa_srm_client_ensure();
    if (!client) return ESP_ERR_INVALID_STATE;

    if (job->clear_dst) {
        memset(job->dst, 0, need);
        /* Write those zero lines back before the driver's M2C invalidate of the
         * output window discards them (see ppa_scale_rgb565). */
        esp_cache_msync(job->dst, need, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    /* mirror_x/mirror_y are the hardware's own bits (ppa_ll.h:176-190); the
     * pages flip the upright image and THEN rotate. If the engine turns out to
     * mirror AFTER rotating, swap hflip/vflip for rotate_cw 1 and 3 here -- a
     * mirror before a 90-degree rotation is the opposite-axis mirror after it. */
    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = job->src,
            .pic_w = job->src_stride_px,
            .pic_h = job->src_h,
            .block_w = job->block_w,
            .block_h = job->block_h,
            .block_offset_x = job->block_x,
            .block_offset_y = job->block_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = job->dst,
            .buffer_size = job->dst_buf_size,
            .pic_w = job->dst_w,
            .pic_h = job->dst_h,
            .block_offset_x = job->dst_x,
            .block_offset_y = job->dst_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle,
        .scale_x = scale,
        .scale_y = scale,
        .mirror_x = job->hflip,
        .mirror_y = job->vflip,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t err = ppa_do_scale_rotate_mirror(client, &srm);
    if (err == ESP_OK) {
        job->out_w = ow;
        job->out_h = oh;
    }
    return err;
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

    size_t needed = ((size_t)dst_w * dst_h * 2 + 127) & ~(size_t)127;
    if (needed > dst_buf_size) {
        ESP_LOGE(TAG, "Pre-allocated buffer too small: need %zu, have %zu", needed, dst_buf_size);
        return NULL;
    }

    /* One 1/16-step factor drives both axes. Every caller here scales
     * isotropically (thumbnail zoom 2.0x = 32, moon drag 3.0x = 48); an
     * anisotropic request is refused rather than silently squashed on one axis,
     * and the callers already treat NULL as "no hardware scale". */
    uint32_t n16_x = (uint32_t)(((float)dst_w / (float)src_w) * 16.0f);
    uint32_t n16_y = (uint32_t)(((float)dst_h / (float)src_h) * 16.0f);
    if (n16_x != n16_y || n16_x == 0 || n16_x > 255) {
        ESP_LOGE(TAG, "PPA scale %lux%lu -> %lux%lu not a single n/16 factor (x=%lu y=%lu)",
                 (unsigned long)src_w, (unsigned long)src_h,
                 (unsigned long)dst_w, (unsigned long)dst_h,
                 (unsigned long)n16_x, (unsigned long)n16_y);
        return NULL;
    }

    ppa_srm_job_t job = {
        .src = src,
        .src_stride_px = src_stride,
        .src_h = src_h,
        .block_w = src_w,
        .block_h = src_h,
        .dst = dst_buf,
        .dst_buf_size = dst_buf_size,
        .dst_w = dst_w,
        .dst_h = dst_h,
        .scale_n16 = (uint8_t)n16_x,
        .clear_dst = clear_dst,
    };

    esp_err_t err = ppa_srm_rgb565(&job);
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
