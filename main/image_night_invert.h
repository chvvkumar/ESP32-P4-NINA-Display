#pragma once

/**
 * @file image_night_invert.h
 * @brief Neutral-only inversion for the NWS radar tile (night readability).
 *
 * Header-only, no ESP-IDF includes -- host-testable in isolation
 * (test/host/test_night_invert.c), same precedent as image_probe.h,
 * poll_backoff.h and radar_play.h. Integer arithmetic only: the P4 FPU is
 * single-precision and this runs 330k times per frame, so no float appears
 * here at all.
 *
 * WHY NEUTRAL-ONLY, NOT A PLAIN INVERSION
 * ---------------------------------------
 * Measured over a real radar.weather.gov RIDGE still (KLSX_0.gif): mean luma
 * 234/255, 89.9% of pixels at luma >= 200, 81.9% of pixels exactly
 * RGB(254,254,254). The tile is a white sheet -- painful in a dark
 * observatory. But the six dominant colours are ALL neutral greys:
 *
 *     (254,254,254)  (217,217,217)  (50,50,50)
 *     (230,230,230)  (81,81,81)     (243,243,243)
 *
 * That is the whole basemap: white background, grey state/county lines, grey
 * city labels, grey dBZ legend text, grey timestamp. Only the weather echoes
 * carry chroma. So inverting the greys and ONLY the greys turns the sheet
 * black and the dark map furniture light, while the dBZ colour ramp survives
 * bit for bit. A naive full-frame inversion would turn green echoes magenta
 * and make the legend lie about what the operator is looking at.
 *
 * THRESHOLD
 * ---------
 * Saturation is (max - min) across R,G,B after expanding 5/6/5 to a common
 * 8-bit depth -- the expansion is required, because comparing raw r5/g6/b5
 * would read every grey as saturated by the extra green bit alone. A true
 * grey does not survive RGB565 exactly: the 5-bit and 6-bit channels round
 * differently, and the worst-case spread over all 256 grey levels is 4
 * (e.g. 243 -> r8=247, g8=243). IMAGE_NIGHT_INVERT_SAT_MAX is set to 8,
 * double that worst case, so it catches all six colours above (their measured
 * spreads are 0,3,1,0,1,4) plus antialiased near-neutral label edges.
 *
 * The margin above is large. The least saturated colour in the NWS dBZ ramp is
 * the 70 dBZ purple 0x9854C6, spread 114; the greens, yellows, oranges and reds
 * are all above 190. Nothing on the ramp comes within 14x of the threshold.
 *
 * Known and accepted: 75 dBZ in the RIDGE legend is white, which this inverts
 * to black like the background. 75 dBZ does not occur in practice, and the
 * legend swatch for it is unreadable against the white sheet today anyway.
 */

#include <stdint.h>
#include <stddef.h>

/** Max (max-min) channel spread, in 8-bit units, still treated as neutral. */
#define IMAGE_NIGHT_INVERT_SAT_MAX 8

/**
 * Invert one RGB565 pixel if it is near-neutral; pass it through untouched if
 * it carries chroma.
 *
 * Neutral pixels come back as a canonical grey of the inverted luminance
 * (BT.601 integer weights, the same 77/150/29 image_red_remap.c uses).
 *
 * Not an exact involution: applying it twice returns the original within a
 * couple of levels, not bit for bit, because luma and the 5/6/5 write-back
 * each round. That is a documented property, not a load-bearing one -- the one
 * caller applies it once, to a freshly decoded buffer, before that buffer is
 * handed to the radar ring.
 */
static inline uint16_t image_night_invert_px(uint16_t px)
{
    uint8_t r5 = (uint8_t)((px >> 11) & 0x1f);
    uint8_t g6 = (uint8_t)((px >> 5) & 0x3f);
    uint8_t b5 = (uint8_t)(px & 0x1f);
    /* Replicate the high bits into the low ones so all three channels land on
     * the same 0..255 scale (bit-identical to image_red_remap_rgb565_force). */
    uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

    uint8_t mx = (r8 > g8) ? r8 : g8;
    if (b8 > mx) {
        mx = b8;
    }
    uint8_t mn = (r8 < g8) ? r8 : g8;
    if (b8 < mn) {
        mn = b8;
    }
    if ((uint8_t)(mx - mn) > IMAGE_NIGHT_INVERT_SAT_MAX) {
        return px;   /* weather echo: leave it EXACTLY as it is */
    }

    uint8_t luma = (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
    uint8_t inv  = (uint8_t)(255 - luma);
    return (uint16_t)(((uint16_t)(inv >> 3) << 11) |
                      ((uint16_t)(inv >> 2) << 5)  |
                       (uint16_t)(inv >> 3));
}

/**
 * Apply image_night_invert_px() over @p px_count pixels in place.
 * NULL @p buf or a zero count is a no-op.
 */
static inline void image_night_invert_rgb565(uint16_t *buf, size_t px_count)
{
    if (buf == NULL || px_count == 0) {
        return;
    }
    for (size_t i = 0; i < px_count; i++) {
        buf[i] = image_night_invert_px(buf[i]);
    }
}
