#pragma once

/**
 * @file image_probe.h
 * @brief Pure magic-byte sniffing + PNG/GIF header dimension reads.
 *
 * Header-only, no ESP-IDF includes -- host-testable in isolation
 * (test/host/test_image_probe.c). jpeg_utils.c, the only firmware user, pulls
 * in driver/ppa.h, esp_cache.h and stb_image.h and therefore cannot be
 * host-compiled at all; this is the byte-offset/endianness logic lifted out of
 * it so the part that is worth testing can be.
 *
 * JPEG dimensions are deliberately NOT here: reading them means walking the
 * SOF marker chain, which jpeg_utils.c already delegates to stb_image
 * (jpeg_probe_dimensions()). That is not pure, so it stays put -- this unit
 * only reports IMG_FMT_JPEG and leaves the dimensions at 0 for the caller
 * to fill in.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/** Image container recognised by image_probe_sniff(). */
typedef enum {
    IMG_FMT_UNKNOWN = 0,  /**< not an image we decode (e.g. an HTML error page) */
    IMG_FMT_JPEG,
    IMG_FMT_PNG,
    IMG_FMT_GIF,
} img_fmt_t;

/**
 * Identify an image by its magic bytes and, for PNG and GIF, read its pixel
 * dimensions straight out of the header.
 *
 * Every read is bounds-checked against @p size, so a truncated buffer is safe
 * to pass in: it either fails to match a magic (IMG_FMT_UNKNOWN) or matches
 * and leaves the dimensions at 0 because the header stops short.
 *
 * @param data   Image bytes (may be NULL, may be short/truncated)
 * @param size   Byte count
 * @param out_w  Receives width in pixels, or 0 if unread/unreadable (optional)
 * @param out_h  Receives height in pixels, or 0 if unread/unreadable (optional)
 * @return the detected format; IMG_FMT_UNKNOWN if none matched. IMG_FMT_JPEG
 *         always comes back with 0x0 -- see the file comment.
 */
static inline img_fmt_t image_probe_sniff(const uint8_t *data, size_t size,
                                          uint32_t *out_w, uint32_t *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!data || size < 4) return IMG_FMT_UNKNOWN;

    /* JPEG: SOI marker FF D8. Dimensions are the caller's problem. */
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return IMG_FMT_JPEG;
    }

    /* PNG: 8-byte signature, then IHDR width/height as big-endian u32 at 16/20. */
    static const uint8_t png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (size >= sizeof(png_sig) && memcmp(data, png_sig, sizeof(png_sig)) == 0) {
        if (size >= 24) {
            if (out_w) {
                *out_w = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                         ((uint32_t)data[18] << 8)  |  (uint32_t)data[19];
            }
            if (out_h) {
                *out_h = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                         ((uint32_t)data[22] << 8)  |  (uint32_t)data[23];
            }
        }
        return IMG_FMT_PNG;
    }

    /* GIF: "GIF87a"/"GIF89a", then logical screen width/height as little-endian
     * u16 at offsets 6 and 8. */
    if (size >= 6 && memcmp(data, "GIF8", 4) == 0 &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        if (size >= 10) {
            if (out_w) *out_w = (uint32_t)data[6] | ((uint32_t)data[7] << 8);
            if (out_h) *out_h = (uint32_t)data[8] | ((uint32_t)data[9] << 8);
        }
        return IMG_FMT_GIF;
    }

    return IMG_FMT_UNKNOWN;
}
