#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* img_fmt_t + the pure magic/PNG/GIF header logic image_probe_format_dims()
 * delegates to (host-testable; see test/host/test_image_probe.c). */
#include "image_probe.h"

/**
 * @brief Register the shared PPA SRM client used by every scaler in this file.
 * Call once from app_main after the display is up. The scalers register it
 * lazily if this was never called, so it is an optimisation, not a hard
 * prerequisite.
 */
void jpeg_utils_ppa_init(void);

/**
 * @brief Software JPEG decode fallback (stb_image).
 * Handles CMYK, progressive, and other JPEGs that the HW decoder rejects.
 * Output is RGB565 in PSRAM.  Caller takes ownership (free with free()).
 *
 * @param jpg_data  JPEG compressed data
 * @param jpg_size  Size in bytes
 * @param out_buf   Receives allocated RGB565 buffer
 * @param out_w     Receives image width in pixels (exact)
 * @param out_h     Receives image height in pixels (exact)
 * @param out_size  Receives buffer size in bytes
 * @return true on success
 */
bool jpeg_sw_decode_rgb565(const uint8_t *jpg_data, size_t jpg_size,
                           uint8_t **out_buf, uint32_t *out_w, uint32_t *out_h,
                           size_t *out_size);

/**
 * @brief Decode an image to RGB565, ESP32-P4 hardware JPEG engine first.
 *
 * Tries the HW decoder for baseline JPEG and falls back to
 * jpeg_sw_decode_rgb565() for anything it refuses (progressive, CMYK, grayscale,
 * odd sampling, engine busy/out of memory) and for the non-JPEG formats stb
 * handles (PNG, GIF). The HW path decodes RGB888 (~1.55 MB at 720x720) but packs
 * it to RGB565 in place and trims the block, so unlike stb it never holds the
 * two at once, which is what makes the image pages fit in a fragmented PSRAM heap.
 *
 * Output contract is identical to jpeg_sw_decode_rgb565(): tightly packed
 * RGB565 rows top-down, out_w/out_h are the image's real pixel dimensions (the
 * HW MCU padding is removed), 128-byte aligned PSRAM, caller frees with free().
 * *out_size is the ALLOCATION size (w*h*2 rounded up to 128 B once the HW
 * path has packed and trimmed its decode buffer, but never assume that);
 * never derive a stride or a pixel count from it, use out_w/out_h. Both HW
 * paths (GRAY8 and RGB888) pack to RGB565 through a 4x4 ordered dither, so a
 * smooth gradient does not band on the 5/6-bit panel. Needs about 10 KB of
 * caller stack.
 *
 * @return true on success (from either path)
 */
bool jpeg_decode_rgb565(const uint8_t *jpg, size_t len, uint8_t **out_buf,
                        uint32_t *out_w, uint32_t *out_h, size_t *out_size);

/**
 * @brief Probe a JPEG's pixel dimensions from its header WITHOUT decoding it.
 * Wraps stbi_info_from_memory(); reads only the header, allocates nothing for
 * pixel data. Use to reject oversized images before the full decode allocation.
 *
 * @param jpg_data  JPEG compressed data
 * @param jpg_size  Size in bytes
 * @param out_w     Receives image width in pixels
 * @param out_h     Receives image height in pixels
 * @return true if dimensions were read successfully
 */
bool jpeg_probe_dimensions(const uint8_t *jpg_data, size_t jpg_size,
                           uint32_t *out_w, uint32_t *out_h);

/**
 * @brief Identify an image by its magic bytes and read its dimensions from the
 * header WITHOUT decoding it.
 *
 * Gate for fetched payloads: IMG_FMT_UNKNOWN means the bytes are not one of the
 * formats stb_image is compiled for (HTML error pages included) and must not be
 * handed to the decoder. Every read is bounds-checked against @p size, so a
 * truncated response is safe to pass in.
 *
 * @param data   Image bytes (may be short/truncated)
 * @param size   Byte count
 * @param out_w  Receives width in pixels, or 0 if the header was too short
 * @param out_h  Receives height in pixels, or 0 if the header was too short
 * @return the detected format; IMG_FMT_UNKNOWN if none matched. On a recognised
 *         format with 0x0 dimensions the caller should skip any size cap.
 */
img_fmt_t image_probe_format_dims(const uint8_t *data, size_t size,
                                  uint32_t *out_w, uint32_t *out_h);

/**
 * @brief Scale an RGB565 image buffer using the PPA hardware accelerator.
 * Allocates a new 128-byte-aligned output buffer in PSRAM.
 * Caller takes ownership of the returned buffer (free with free()).
 *
 * @param src        Source RGB565 buffer (must be DMA-accessible, e.g. PSRAM)
 * @param src_w      Source content width in pixels (region to scale)
 * @param src_h      Source content height in pixels (region to scale)
 * @param src_stride Source buffer stride in pixels (0 = same as src_w).
 *                   Use when the buffer has MCU padding beyond the content area.
 * @param dst_w      Target width in pixels
 * @param dst_h      Target height in pixels
 * @param out_size   Set to the allocated buffer size on success
 * @return Pointer to scaled RGB565 buffer, or NULL on failure (caller should fallback to SW scaling)
 */
uint8_t *ppa_scale_rgb565(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                           uint32_t src_stride,
                           uint32_t dst_w, uint32_t dst_h, size_t *out_size);

/**
 * @brief Scale an RGB565 image into a pre-allocated output buffer using PPA hardware.
 * Unlike ppa_scale_rgb565(), this does not allocate — caller provides the destination.
 * Buffer must be 128-byte aligned and in PSRAM (DMA-accessible).
 *
 * @param src        Source RGB565 buffer
 * @param src_w      Source content width in pixels
 * @param src_h      Source content height in pixels
 * @param src_stride Source buffer stride in pixels (0 = same as src_w)
 * @param dst_w      Target width in pixels
 * @param dst_h      Target height in pixels
 * @param dst_buf    Pre-allocated destination buffer (must be 128-byte aligned, PSRAM)
 * @param dst_buf_size Size of dst_buf in bytes
 * @param out_size   Set to the actual used size on success
 * @return dst_buf on success, NULL on failure
 */
uint8_t *ppa_scale_rgb565_into(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint32_t src_stride,
                                uint32_t dst_w, uint32_t dst_h,
                                uint8_t *dst_buf, size_t dst_buf_size,
                                size_t *out_size);

/**
 * @brief Like ppa_scale_rgb565_into() but does NOT clear the destination first
 * and emits no per-call INFO log.
 * Use only when the scale is an EXACT integer ratio so every output pixel is
 * overwritten by the transfer (otherwise stale data shows in the uncovered
 * remainder). Intended for the moon drag loop (240->720 = 3.0x, per frame), to
 * avoid a ~1MB/frame memset. Same buffer/alignment requirements as
 * ppa_scale_rgb565_into().
 *
 * @return dst_buf on success, NULL on failure
 */
uint8_t *ppa_scale_rgb565_into_noclear(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                        uint32_t src_stride,
                                        uint32_t dst_w, uint32_t dst_h,
                                        uint8_t *dst_buf, size_t dst_buf_size,
                                        size_t *out_size);

/**
 * @brief Software BILINEAR RGB565 rescale, straight orientation (row 0 -> row 0).
 *
 * Use instead of the PPA scalers when image quality matters more than speed:
 * the SRM path resamples by pixel drop/duplicate (visible banding on non-integer
 * ratios) and does not preserve orientation, whereas this is a plain
 * centre-aligned bilinear filter that copies rows top-down.
 *
 * All per-pixel arithmetic is 16.16 fixed point (no float). Both buffers are
 * tightly packed (stride == width); src and dst must not overlap.
 *
 * @param src  Source RGB565 pixels
 * @param sw   Source width in pixels (>0)
 * @param sh   Source height in pixels (>0)
 * @param dst  Destination RGB565 pixels, at least dw*dh*2 bytes
 * @param dw   Destination width in pixels (>0)
 * @param dh   Destination height in pixels (>0)
 */
void sw_scale_rgb565_bilinear(const uint16_t *src, int sw, int sh,
                              uint16_t *dst, int dw, int dh);

/**
 * @brief One PPA SRM pass: crop + mirror + rotate + scale, RGB565 in/out.
 *
 * Everything the six software passes on the image pages do today, in one DMA
 * transaction. Scale is a 1/16-step factor on BOTH axes (the hardware truncates
 * to 1/16 regardless), so pick an exact ratio or over-provision the source block
 * and show a window of the result.
 */
typedef struct {
    const uint8_t *src;        /* RGB565 */
    uint32_t src_stride_px;    /* pixels per source row (pic_w) */
    uint32_t src_h;            /* pic_h */
    uint32_t block_x, block_y, block_w, block_h;   /* source crop window */
    uint8_t  rotate_cw;        /* 0..3 = 0/90/180/270 degrees CLOCKWISE (page convention) */
    bool     hflip, vflip;     /* applied to the source before rotation, same as the page's SW passes */
    uint8_t  *dst;             /* 128 B aligned PSRAM */
    size_t   dst_buf_size;     /* 128 B multiple */
    uint32_t dst_w, dst_h;     /* destination picture size */
    uint32_t dst_x, dst_y;     /* where the scaled block lands in dst */
    uint8_t  scale_n16;        /* scale = n/16, 1..255 (both axes) */
    bool     clear_dst;        /* memset+C2M the whole dst first */
    uint32_t out_w, out_h;     /* filled: size of the written block as the driver computes it */
} ppa_srm_job_t;

/**
 * @brief Run one ppa_srm_job_t (BLOCKING). Fills job->out_w / job->out_h.
 * @return ESP_OK, or the driver's error / ESP_ERR_INVALID_ARG on a bad job.
 */
esp_err_t ppa_srm_rgb565(ppa_srm_job_t *job);

/**
 * @brief One PPA Blend pass: out = bg*(1 - a) + fg*a, RGB565 throughout.
 *
 * All three pictures are @p w x @p h with no offsets (the Blend engine cannot
 * scale, rotate or mirror). @p fg_alpha is the mix, 0 = @p bg, 255 = @p fg,
 * applied as the hardware's PPA_ALPHA_FIX_VALUE — an RGB565 foreground carries
 * no alpha of its own, so without it the blend would be a plain copy.
 *
 * @p out must be 128-byte aligned PSRAM (or internal RAM) of at least
 * align128(w*h*2) bytes; the driver checks the pointer and the size it is
 * given, not the allocation. @p bg and @p fg need no alignment and may live
 * anywhere, including flash. The driver does its own cache maintenance (writes
 * the inputs back, invalidates the output), so no esp_cache_msync() is needed
 * around this — but the CPU must not be holding unflushed writes to @p out, and
 * must not write @p out while the call is running (it BLOCKS until the DMA
 * finishes). @p out may alias @p bg or @p fg; the ring playback does not.
 *
 * @return ESP_OK, ESP_ERR_INVALID_SIZE for a zero or over-8191 px axis,
 *         ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE, or the driver's error.
 */
esp_err_t ppa_blend_rgb565(const uint8_t *bg, const uint8_t *fg, uint8_t *out,
                           uint32_t w, uint32_t h, uint8_t fg_alpha);

/**
 * @brief Pack an RGB888 buffer (stb byte order: R,G,B) into RGB565 on the PPA.
 *
 * 1:1, no rotation. @p dst565 must be 128-byte aligned PSRAM and @p dst_size a
 * 128-byte multiple of at least w*h*2 (the driver rejects anything else).
 *
 * @return true if the hardware did it; false means the caller must run its own
 *         software pack.
 */
bool ppa_rgb888_to_rgb565(const uint8_t *rgb888, uint32_t w, uint32_t h,
                          uint8_t *dst565, size_t dst_size);
