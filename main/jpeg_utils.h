#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Fetch a prepared JPEG image from NINA, hardware-decode it, and display it as a thumbnail.
 * @param base_url NINA API base URL for the instance
 * @return true if image was successfully fetched, decoded, and handed to the dashboard
 */
bool fetch_and_show_thumbnail(const char *base_url);

/**
 * @brief Software JPEG decode fallback (stb_image).
 * Handles CMYK, progressive, and other JPEGs that the HW decoder rejects.
 * Output is RGB565 in PSRAM.  Caller takes ownership (free with free()).
 *
 * @param jpg_data  JPEG compressed data
 * @param jpg_size  Size in bytes
 * @param out_buf   Receives allocated RGB565 buffer
 * @param out_w     Receives image width (rounded up to 16)
 * @param out_h     Receives image height (rounded up to 16)
 * @param out_size  Receives buffer size in bytes
 * @return true on success
 */
bool jpeg_sw_decode_rgb565(const uint8_t *jpg_data, size_t jpg_size,
                           uint8_t **out_buf, uint32_t *out_w, uint32_t *out_h,
                           size_t *out_size);

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
