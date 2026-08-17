#pragma once

/**
 * @file image_night_invert.h
 * @brief Basemap-only darkening for the NWS radar tile (night readability).
 *
 * Header-only, no ESP-IDF includes -- host-testable in isolation
 * (test/host/test_night_invert.c), same precedent as image_probe.h,
 * poll_backoff.h and radar_play.h. Integer arithmetic only: the P4 FPU is
 * single-precision and this runs 330k times per frame, so no float appears
 * here at all. No division on any path.
 *
 * WHAT THE TILE ACTUALLY CONTAINS
 * -------------------------------
 * Measured 2026-08-17 over the map body (header and legend bands excluded) of
 * eight RIDGE-2 products -- SOUTHEAST, CONUS, HAWAII, PACNORTHWEST, KLSX,
 * NORTHEAST, PACSOUTHWEST, ALASKA -- and cross-checked against all ten frames
 * of each product's animated loop (20.4M pixel samples). A pixel whose value is
 * identical in all ten frames is basemap furniture; a pixel that changes over
 * the 50-minute loop is weather. That persistence statistic is bimodal, so it
 * classifies the palette without guesswork.
 *
 * Four populations, quoted in the 8-bit values these RGB565 words expand to:
 *
 *   BASEMAP NEUTRAL   sat 0..8      52.4% of pixels   persist 45..100%
 *       Land fill RGB(255,255,255) alone is 45.6%. Plus state/county lines,
 *       roads' grey casing, city dots, label and legend text.
 *
 *   BASEMAP WATER     sat 24..50    41.9% of pixels   persist 93..99%
 *       The pale blue ocean/lake wash RGB(198,235,247) is 41.8% by itself --
 *       96.6% of the HAWAII tile. Plus its antialias against the white land
 *       fill: (231,247,255), (222,243,247), (189,227,239).
 *
 *   BASEMAP ROAD      sat 12..148    1.8% of pixels   persist 42..100%
 *       Interstates, drawn as red lines: RGB(156,12,8) core through a pale
 *       antialias ladder up to (247,235,239).
 *
 *   WEATHER ECHO      sat 58..199    1.2% of pixels   persist 0..3%
 *       The dBZ ramp, from the weak blue (115,134,173) to saturated greens
 *       and reds.
 *
 * WHY THE OLD NEUTRAL-ONLY RULE FAILED
 * ------------------------------------
 * The previous rule inverted only sat <= 8, derived from KLSX, whose basemap is
 * a white sheet. That is correct for an inland site and catastrophic for a
 * coastal or island one: on HAWAII the ocean wash is 96.6% of the tile at
 * sat=49, so it was passed through untouched. The land inverted to black and
 * the ocean stayed a blinding pale blue over the whole panel.
 *
 * THE RULE
 * --------
 * Three classes, tested in order:
 *
 *  1. NEUTRAL   sat <= IMAGE_NIGHT_INVERT_SAT_MAX
 *               -> inverted to the canonical grey of (255 - luma).
 *               Unchanged from the previous behaviour: the white sheet goes
 *               black and the dark map furniture comes up light and readable.
 *
 *  2. PALE COOL WASH
 *               sat <= IMAGE_NIGHT_INVERT_WASH_SAT_MAX
 *               AND min channel >= IMAGE_NIGHT_INVERT_WASH_MIN_CH
 *               AND blue > red
 *               -> the white component is subtracted: (r-mn, g-mn, b-mn).
 *               The ocean lands on RGB(0,36,49): dark, but unmistakably blue.
 *
 *  3. EVERYTHING ELSE -> returned bit for bit. Echoes and roads.
 *
 * WHY SUBTRACT THE MINIMUM RATHER THAN FLATTEN TO BLACK
 * -----------------------------------------------------
 * Turning water black too would lose the coastline, which is most of what a
 * radar map is for -- an echo offshore reads completely differently from the
 * same echo over land. Subtracting the minimum channel removes exactly the
 * white component that makes the wash bright and keeps exactly the chroma that
 * makes it water. It costs three subtractions, no multiply and no divide, and
 * it is self-scaling: a pale wash has little chroma, so the result is
 * intrinsically dark without a brightness constant to tune.
 *
 *     land  RGB(255,255,255) -> 0x0000  RGB(0, 0, 0)   luma 0
 *     ocean RGB(198,235,247) -> 0x0126  RGB(0,36,49)   luma 26
 *
 * Black land against a deep blue-green sea, with the coastline intact.
 *
 * THRESHOLDS AND MEASURED MARGINS (all in the 8-bit expansion of RGB565)
 * ---------------------------------------------------------------------
 * The two populations are NOT separable by saturation alone, and no single
 * axis separates them with a comfortable margin. Saturation:
 *
 *     water   24 .. 50          echo   58 .. 199        gap of 8
 *
 * Eight levels is too thin to bet the ramp on, so the wash class needs a
 * second, independent axis. The minimum channel -- the white component, which
 * is precisely what "pale" means -- separates far better, and it is already
 * computed for the saturation test, so it is free:
 *
 *     water  189 .. 231         echo     0 .. 115       gap of 74
 *
 * Thresholds are placed inside both gaps:
 *
 *     IMAGE_NIGHT_INVERT_WASH_SAT_MAX  56   water max 50 (margin 6 below)
 *                                           echo  min 58 (margin 2 above)
 *     IMAGE_NIGHT_INVERT_WASH_MIN_CH  176   water min 189 (margin 13 above)
 *                                           echo  max 115 (margin 61 below)
 *
 * Both must hold, so misclassifying an echo requires it to breach both at once.
 * The closest echo to that corner is the weak blue (115,134,173): it misses the
 * saturation bound by 2 and the min-channel bound by 61.
 *
 * THE blue > red TEST, AND WHY IT IS NOT DECORATION
 * -------------------------------------------------
 * Two ramp colours sit inside the sat/min-channel box and would otherwise be
 * clipped:
 *
 *   - The 60 dBZ pale magenta RGB(255,203,255): sat 52, min channel 203. It is
 *     numerically almost identical to the ocean wash (sat 49, min channel 198).
 *     Its blue equals its red, so `b > r` excludes it.
 *   - The pale olive band around -5 dBZ, e.g. (189,193,174) and (207,210,178):
 *     red exceeds blue in all of them, so `b > r` excludes them too.
 *
 * With `b > r` applied, the highest min channel of any ramp colour that could
 * still reach the wash class is the -10 dBZ blue-grey (156,166,165) at 156 --
 * 20 below the 176 threshold. That band, not the body echoes, is the binding
 * constraint on how low IMAGE_NIGHT_INVERT_WASH_MIN_CH may go.
 *
 * `b > r` also excludes the entire red road family, which is deliberate: see
 * below.
 *
 * KNOWN AND ACCEPTED
 * ------------------
 *  - RED ROADS ARE NOT DARKENED. They cannot be. The interstate red
 *    RGB(156,12,8) and the ~62 dBZ ramp red RGB(156,12,8) are the same colour
 *    to the bit; no test can separate them, and the ramp wins. Roads therefore
 *    pass through as dark red lines (luma 55 at the core), exactly as they did
 *    before this change. Their pale outer antialias, around (222,166,165) at
 *    luma 183, also passes through, but that is under 0.1% of the tile.
 *  - The coastline antialias between min channel 154 and 174 passes through
 *    unmodified -- roughly 0.3% of pixels, a thin bright fringe on the coast.
 *    Darkening it would put the threshold within 4 levels of a real ramp
 *    colour, and the ramp is not negotiable.
 *  - The legend's low-dBZ grey band (-20 to 0 dBZ) is genuinely neutral and is
 *    inverted along with the basemap greys, as is the white 75 dBZ swatch.
 *    Both were unreadable against the white sheet before.
 */

#include <stdint.h>
#include <stddef.h>

/** Max (max-min) channel spread, in 8-bit units, still treated as neutral. */
#define IMAGE_NIGHT_INVERT_SAT_MAX 8

/** Max channel spread, in 8-bit units, still eligible for the pale-wash class. */
#define IMAGE_NIGHT_INVERT_WASH_SAT_MAX 56

/** Min channel value, in 8-bit units, required for the pale-wash class. */
#define IMAGE_NIGHT_INVERT_WASH_MIN_CH 176

/**
 * Darken one RGB565 basemap pixel; pass a weather echo through untouched.
 *
 * Near-neutral pixels come back as a canonical grey of the inverted luminance
 * (BT.601 integer weights, the same 77/150/29 image_red_remap.c uses). Pale
 * cool washes come back as their own chroma on black. Everything else, echoes
 * and roads alike, comes back bit for bit.
 *
 * Not an exact involution on the transformed classes: applying it twice returns
 * a neutral within a couple of levels, not bit for bit, because luma and the
 * 5/6/5 write-back each round; a wash is idempotent from the second
 * application on, since subtracting an already-zero minimum is a no-op. That is
 * a documented property, not a load-bearing one -- the one caller applies it
 * once, to a freshly decoded buffer, before that buffer is handed to the radar
 * ring.
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
    uint8_t sat = (uint8_t)(mx - mn);

    if (sat <= IMAGE_NIGHT_INVERT_SAT_MAX) {
        /* Basemap neutral: white sheet, borders, labels. Invert it. */
        uint8_t luma = (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
        uint8_t inv  = (uint8_t)(255 - luma);
        return (uint16_t)(((uint16_t)(inv >> 3) << 11) |
                          ((uint16_t)(inv >> 2) << 5)  |
                           (uint16_t)(inv >> 3));
    }

    if (sat <= IMAGE_NIGHT_INVERT_WASH_SAT_MAX &&
        mn >= IMAGE_NIGHT_INVERT_WASH_MIN_CH &&
        b8 > r8) {
        /* Basemap pale cool wash: the ocean. Strip the white component and keep
         * the chroma, so water stays water and the coastline survives. */
        r8 = (uint8_t)(r8 - mn);
        g8 = (uint8_t)(g8 - mn);
        b8 = (uint8_t)(b8 - mn);
        return (uint16_t)(((uint16_t)(r8 >> 3) << 11) |
                          ((uint16_t)(g8 >> 2) << 5)  |
                           (uint16_t)(b8 >> 3));
    }

    return px;   /* weather echo (and red roads): leave it EXACTLY as it is */
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
