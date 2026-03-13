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
 * @brief Scale an RGB565 image buffer using the PPA hardware accelerator.
 * Allocates a new 128-byte-aligned output buffer in PSRAM.
 * Caller takes ownership of the returned buffer (free with free()).
 *
 * @param src      Source RGB565 buffer (must be DMA-accessible, e.g. PSRAM)
 * @param src_w    Source width in pixels
 * @param src_h    Source height in pixels
 * @param dst_w    Target width in pixels
 * @param dst_h    Target height in pixels
 * @param out_size Set to the allocated buffer size on success
 * @return Pointer to scaled RGB565 buffer, or NULL on failure (caller should fallback to SW scaling)
 */
uint8_t *ppa_scale_rgb565(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                           uint32_t dst_w, uint32_t dst_h, size_t *out_size);
