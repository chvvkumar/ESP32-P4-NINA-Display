#pragma once

/*
 * radar_play.h - pure decisions for the Weather Radar page.
 *
 * Header-only and free of ESP-IDF, FreeRTOS and LVGL, so the host test suite
 * can exercise the parts that are easy to get wrong (token validation, dedupe,
 * cursor wrap, the newest-frame hold) without a device. Same precedent as
 * poll_backoff.h. The buffer ownership, allocation and LVGL handoff live in
 * ui/nina_image_page.c.
 *
 * Ring convention: index 0 is the NEWEST frame, index count-1 the oldest.
 * Playback runs oldest -> newest and holds on the newest before looping, which
 * is what every public radar loop does so the current state stays readable.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* radar.weather.gov/ridge/standard serves {TOKEN}_0.gif .. {TOKEN}_9.gif;
 * _10.gif is a hard 404, so ten is the hard ceiling on the ring. */
#define RADAR_RING_MAX        10

/* Token alphabet limits: "KTLX" (4) up to "SOUTHEASTERN" and friends. */
#define RADAR_TOKEN_MIN_LEN    3
#define RADAR_TOKEN_MAX_LEN   15

/**
 * True if @p t is a usable RIDGE token: a site id ("KTLX"), a region token
 * ("SOUTHEAST") or "CONUS". Uppercase A-Z and 0-9 only, 3..15 characters.
 *
 * This is the ban on {TOKEN}_loop.gif, not cosmetic validation. The URL
 * builder formats "%s_%d.gif", so the frame index can never spell "loop", but
 * a token carrying a query or fragment can smuggle one past it: the token
 * "KTLX_loop.gif?" builds ".../KTLX_loop.gif?_0.gif", which serves the
 * animated loop with the remainder as a query string. Decoding that costs
 * 12.59 MiB in one allocation plus 2.83 MiB of scratch, reallocated up to a
 * 26.75 MiB double-peak. Restricting the alphabet also rules out path
 * traversal and every other form of URL surgery through the token field.
 */
static inline bool radar_token_valid(const char *t)
{
    if (t == NULL) return false;
    size_t n = strlen(t);
    if (n < RADAR_TOKEN_MIN_LEN || n > RADAR_TOKEN_MAX_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        bool ok = (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9');
        if (!ok) return false;
    }
    return true;
}

/**
 * True if a frame fetched under generation @p frame_gen must be REJECTED
 * because the ring has since been reset to generation @p ring_gen.
 *
 * The ring generation is bumped by every reset (region/frame-count/crop change,
 * page leave, page disable). Without this test, a backfill that is already in
 * flight when the user switches region keeps appending OLD-region frames into
 * the ring the switch just cleared, and the animation cycles both regions at
 * once. The check belongs at the single insert point every frame passes
 * through, not in each producer's loop: a per-caller abort is a convention the
 * next producer forgets.
 *
 * ORDERING CONTRACT for producers: read the generation BEFORE reading the
 * token/config the fetch will use. The writer updates config first and bumps
 * the generation second, so gen-then-token can only ever pair an old token with
 * an old generation (rejected, correct) or a new token with a new generation
 * (accepted, correct). Reading the token first can pair an OLD token with the
 * NEW generation, which is exactly the bug.
 */
static inline bool radar_frame_is_stale(uint32_t frame_gen, uint32_t ring_gen)
{
    return frame_gen != ring_gen;
}

#define RADAR_PLAY_FRAME_MS  400   /* dwell on a history frame */
#define RADAR_PLAY_NEWEST_MS 1200  /* longer hold on the newest frame */

/** FNV-1a over a byte range. Dedupe key for one radar frame. */
static inline uint32_t radar_fnv1a(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/**
 * True if a frame hashing to @p hash should be dropped instead of inserted.
 * @p neighbour_hash is the hash of the entry it would sit next to (the head
 * for a newest-frame push, the tail for a backfill append). An empty ring
 * (@p count == 0) never dedupes.
 */
static inline bool radar_frame_is_dup(uint32_t neighbour_hash, int count, uint32_t hash)
{
    return count > 0 && neighbour_hash == hash;
}

/**
 * Ring index shown after @p cur, given @p count resident frames. Walks DOWN
 * (oldest -> newest) and wraps from the newest (0) back to the oldest.
 * Defensive against a cursor left dangling by a ring that shrank.
 */
static inline int radar_play_next(int cur, int count)
{
    if (count <= 1) return 0;
    if (cur <= 0 || cur >= count) return count - 1;
    return cur - 1;
}

/** Dwell in ms for the frame at ring index @p idx. */
static inline uint32_t radar_play_period_ms(int idx)
{
    return (idx == 0) ? RADAR_PLAY_NEWEST_MS : RADAR_PLAY_FRAME_MS;
}

/**
 * Ring index the playback timer moves to, SKIPPING every slot that was not
 * baked under the current settings.
 *
 * A crop / dark-mode / Red-Night change re-bakes the ring ONE SLOT AT A TIME
 * (~1 s for ten, and longer if the pass has to wait behind a backfill), so
 * mid-pass the ring genuinely holds frames of two different geometries — and
 * since every frame is scaled to the panel WIDTH, letting playback run through
 * them renders the picture at two different sizes and it pumps until the last
 * slot converts.
 *
 * The cure is not to keep the ring momentarily consistent — nothing can, the
 * re-bake takes real time — but to make DISPLAYING two bakes impossible. Each
 * slot records the bake generation its pixels were produced under
 * (@p slot_gen); playback only ever advances to a slot matching @p cur_gen, so
 * a stale-bake slot is never shown at all, not merely shown less often.
 *
 * Returns @p cur unchanged when NO slot carries the current generation — the
 * one case where freezing is correct. The caller tests for "unchanged" to skip
 * the redraw, and it resolves the moment the first re-baked or newly inserted
 * frame lands. It is also what a single-current-slot ring settles on, since
 * re-showing the frame already on screen would be a redraw for nothing.
 *
 * Walks at most @p count steps, so a ring with no match costs one lap and
 * cannot spin.
 */
static inline int radar_play_next_baked(int cur, int count,
                                        const uint32_t *slot_gen, uint32_t cur_gen)
{
    if (count <= 0 || slot_gen == NULL) return cur;
    int idx = cur;
    for (int k = 0; k < count; k++) {
        idx = radar_play_next(idx, count);
        if (idx >= 0 && idx < count && slot_gen[idx] == cur_gen) return idx;
    }
    return cur;   /* nothing baked under the current settings yet: hold */
}

/** Slots of @p count carrying a bake generation other than @p cur_gen. Logged
 *  once per bake change so the re-bake is visible in /api/logs. */
static inline int radar_stale_count(const uint32_t *slot_gen, int count, uint32_t cur_gen)
{
    int n = 0;
    if (slot_gen == NULL) return 0;
    for (int i = 0; i < count; i++) if (slot_gen[i] != cur_gen) n++;
    return n;
}

/**
 * Slot to convert on step @p k of a re-transform pass over @p count frames,
 * starting at @p start — the slot currently ON SCREEN.
 *
 * Converting the visible slot first is what makes the change look atomic: the
 * picture resizes exactly once, promptly, and the other nine convert behind the
 * held playback where nobody sees them. Defensive against a @p start left
 * dangling by a ring that shrank.
 */
static inline int radar_retransform_idx(int start, int k, int count)
{
    if (count <= 0) return 0;
    if (start < 0 || start >= count) start = 0;
    return (start + k) % count;
}

/* ---- Fit modes: how a decoded frame is cropped before it enters the ring ----
 *
 * Cropping happens ONCE, at insert (ui/nina_image_page.c), which is why a mode
 * change rebuilds the whole ring. The geometry lives here because it is pure
 * integer maths that is easy to get subtly wrong and impossible to test on the
 * device side, where the same file pulls in LVGL and the BSP.
 *
 * Why a square is what "crop" means: the display path scales every frame to the
 * panel WIDTH and centres it vertically, so a frame is letterboxed exactly when
 * it is wider than it is tall. Crop to a square and "scale to fill width"
 * lands on 720x720 on its own -- no repositioning, no separate fill path.
 *
 * TWO modes, not three. There used to be a middle "88% uniform trim" mode; it
 * was strictly dominated -- it threw away map area AND still left the black
 * bars -- so it is gone rather than kept as an option nobody should pick.
 *
 * LEGACY STORED VALUES: a device may hold radar_crop == 2 (the old separate
 * "fill screen") or == 1 (the old 88% trim). Both must land on the surviving
 * geometry, so an existing user silently gets the better crop with no config
 * migration. TWO nets, deliberately:
 *   1. settings_table.h clamps radar_crop to 0..1 (a true CLAMP row, not RESET
 *      -- RESET would send a stored 2 to 0 and silently un-crop those devices).
 *      That runs on every validate_config(), so every load path is covered.
 *   2. radar_fit_rect() below treats ANY non-zero mode as CROP. This is the
 *      pure function every consumer calls, so a value that somehow reaches the
 *      render path unclamped still renders correctly rather than falling
 *      through to "unknown mode -> no crop".
 */
enum {
    RADAR_FIT_OFF  = 0,   /* whole image as published, bars top and bottom */
    RADAR_FIT_CROP = 1,   /* NOAA chrome dropped, then a centred SQUARE crop:
                             fills the panel, no bars, no header/legend */
};

/* Height of the NOAA chrome band on a RIDGE tile: the product header strip at
 * the TOP and the dBZ legend / timestamp strip at the BOTTOM, 24 rows each.
 *
 * MEASURED, not assumed. Sampled from the live radar.weather.gov/ridge/standard
 * products on 2026-08-17 across four tokens -- KLSX (site view, 600x550),
 * HAWAII (regional), CONUS (national, 600x392) and SOUTHEAST (600x571, a
 * different height from the rest) -- and the band was exactly 24 rows at both
 * ends of every one. On KLSX 600x550: rows 0..23 are chrome (dominant colours
 * 50,50,50 and 217,217,217), row 24 is the first map row (254,254,254 at 95%),
 * rows 526..549 are chrome (81,81,81 and 50,50,50) and row 525 is the last map
 * row. Re-measure here if a grey bar reappears at the top of a radar frame.
 *
 * A RUNTIME BAND DETECTOR -- scan inward from each edge for the first row that
 * is mostly the 254,254,254 basemap -- would be more robust if NOAA ever
 * changes the layout, and is the upgrade path. It is not worth it yet: the
 * failure mode of a stale constant is cosmetic, a visible sliver of chrome or a
 * few lost map rows, never a crash or an out-of-bounds read, because every rect
 * below is clamped to the source dimensions.
 *
 * ponytail: baked constant over an image scan, swap in a detector if NOAA
 * changes the product layout. */
#define RADAR_NOAA_CHROME_PX 24

/**
 * Centred crop rectangle for a @p w x @p h frame under fit mode @p mode.
 * Writes the output size to @p ow / @p oh and the source-pixel origin to
 * @p ox / @p oy. Returns false when nothing should be copied -- OFF, a
 * degenerate size, or a crop that would produce no pixels -- in which case the
 * outputs are untouched and the caller keeps the frame as decoded.
 *
 * Two modes over the same picture:
 *   OFF (0)  - everything, NOAA header and legend included, letterboxed.
 *   CROP (>=1) - drop RADAR_NOAA_CHROME_PX rows from each end, then take the
 *                largest square of what is left: no chrome AND no bars.
 *
 * ANY non-zero mode is CROP. That is the legacy-value clamp: a device holding
 * the retired "2 = fill screen" or "1 = 88% trim" resolves to this geometry
 * without a config migration, and it sits in the one pure function every
 * consumer calls rather than in a handler some load path could bypass.
 *
 * The loss is inherent to filling a square panel from a wide picture:
 * 600x550 -> 502x502, 600x392 -> 344x344, 600x571 -> 523x523.
 *
 * Every returned rect satisfies ox + ow <= w and oy + oh <= h, for every mode
 * and every input size, which is what makes the caller's row-wise memcpy safe.
 */
static inline bool radar_fit_rect(uint8_t mode, uint16_t w, uint16_t h,
                                  uint16_t *ow, uint16_t *oh,
                                  uint16_t *ox, uint16_t *oy)
{
    if (mode == RADAR_FIT_OFF) return false;
    if (w == 0 || h == 0) return false;

    /* Vertical band the crop may draw from: the frame minus the chrome.
     *
     * A frame too short to hold both chrome bands is not a real RIDGE tile.
     * Rather than underflow h - 48, fall back to the plain largest centred
     * square. That still honours what the mode promises (no bars), and the only
     * cost is keeping chrome the subtraction could not have removed anyway.
     * Falling back to "no crop" would instead break the promise and put the
     * bars back. */
    uint16_t ytop = 0, yspan = h;
    if (h > 2 * RADAR_NOAA_CHROME_PX) {
        ytop  = RADAR_NOAA_CHROME_PX;
        yspan = (uint16_t)(h - 2 * RADAR_NOAA_CHROME_PX);
    }
    uint16_t side = (w < yspan) ? w : yspan;
    if (side == 0) return false;

    *ow = side;
    *oh = side;
    *ox = (uint16_t)((w - side) / 2);
    /* side <= yspan by construction, so this cannot underflow, and
     * ytop + yspan <= h keeps oy + oh inside the source. Truncating division,
     * so an odd remainder leaves the extra row/column at the bottom/right. */
    *oy = (uint16_t)(ytop + (yspan - side) / 2);
    return true;
}
