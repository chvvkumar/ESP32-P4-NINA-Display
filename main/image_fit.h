/**
 * @file image_fit.h
 * @brief Pure geometry for fitting a decoded image onto the 720 px panel with
 *        ONE PPA SRM pass, so LVGL is handed a picture that is exactly panel
 *        width at scale 256 and only has to blit it.
 *
 * Why this exists: the PPA SRM scale factor is n/16 with n an integer, and the
 * written block width is TRUNCATED (`out = floor(block * n / 16)`). There is no
 * n that turns an arbitrary source width into exactly 720. Two ways out:
 *   - round the scale UP (n = ceil(720*16/lw)), which overshoots 720, and trim
 *     the source block symmetrically until the output lands on <= 720. Costs a
 *     small centre crop.
 *   - round the scale DOWN, which undershoots 720, and centre the result with
 *     black bands left and right. Costs nothing but shows bands.
 * The rule below takes the crop while it is small (<= 4 % of the width) and the
 * bands otherwise.
 *
 * Header-only, integer-only, no ESP includes: host-tested by
 * test/host/test_image_fit.c.
 *
 * All widths/heights are pixels. "LOGICAL" space means POST-rotation: the
 * caller has already applied its config crop to the source and knows what the
 * rotated result would measure. image_fit_logical_to_source() maps the trim
 * window back to the source rectangle the PPA job must read.
 */
#ifndef IMAGE_FIT_H
#define IMAGE_FIT_H

#include <stdbool.h>
#include <stdint.h>

/* Crop budget for the round-up branch, in thousandths of the source width. */
#define IMAGE_FIT_MAX_CROP_PERMILLE 40u    /* 4 %: 500, 600 and 1080 wide sources fill; 3 % put 1080 on side bands */

typedef struct {
    uint8_t  n16;        /* PPA scale numerator: scale = n16/16, 1..255        */
    uint32_t block_x;    /* trim window inside the LOGICAL picture             */
    uint32_t block_y;
    uint32_t block_w;
    uint32_t block_h;
    uint32_t out_w;      /* = block_w * n16 / 16, always <= target             */
    uint32_t out_h;      /* = block_h * n16 / 16, always <= target             */
    uint32_t dst_x;      /* = (target - out_w) / 2: centres the bands          */
} image_fit_t;

/**
 * @brief Pick the SRM scale and the source trim for a @p lw x @p lh logical
 *        picture landing on a @p target wide destination.
 *
 * Formulas, in order:
 *   n_up    = ceil(target * 16 / lw)                 scale that overshoots
 *   w_up    = floor(target * 16 / n_up)              source width that fits at n_up
 *   crop    = (lw - w_up) / lw                       what that costs
 *   n16     = (crop <= 4 %) ? n_up : floor(target * 16 / lw)
 *   n16     = clamp(n16, 1, 255)                     PPA register range
 *   block_w = lw, shrunk to floor(target*16/n16) if it would overshoot
 *   block_h = lh, same shrink (this is the vertical centre crop the old
 *             center_image_y + LVGL clip produced for over-tall sources)
 *   out_w   = floor(block_w * n16 / 16), out_h = floor(block_h * n16 / 16)
 *   block_x = (lw - block_w) / 2, block_y = (lh - block_h) / 2   (centred)
 *   dst_x   = (target - out_w) / 2
 *
 * dst_y is deliberately NOT produced: the destination picture is
 * target x out_h and the CALLER positions that object vertically (the existing
 * center_image_y() at scale 256), exactly as it did before.
 *
 * @return false if any input is 0 or the result would be degenerate (out_w or
 *         out_h == 0); @p out is then untouched.
 */
static inline bool image_fit_pick(uint32_t lw, uint32_t lh, uint32_t target,
                                  image_fit_t *out)
{
    if (!out || lw == 0 || lh == 0 || target == 0) return false;

    const uint32_t t16 = target * 16u;
    uint32_t n_up = (t16 + lw - 1u) / lw;              /* ceil */
    if (n_up == 0) n_up = 1u;
    const uint32_t w_up = t16 / n_up;                  /* source width that fits */
    uint32_t n16;
    if (w_up < lw && (lw - w_up) * 1000u > IMAGE_FIT_MAX_CROP_PERMILLE * lw) {
        n16 = t16 / lw;                                /* crop too dear: bands */
    } else {
        n16 = n_up;                                    /* cheap crop: fill */
    }
    if (n16 < 1u)   n16 = 1u;
    if (n16 > 255u) n16 = 255u;

    const uint32_t fit = t16 / n16;   /* largest source extent that stays <= target */
    uint32_t bw = lw, bh = lh;
    if (bw > fit) bw = fit;
    if (bh > fit) bh = fit;

    const uint32_t ow = bw * n16 / 16u;
    const uint32_t oh = bh * n16 / 16u;
    if (ow == 0 || oh == 0) return false;

    out->n16     = (uint8_t)n16;
    out->block_w = bw;
    out->block_h = bh;
    out->block_x = (lw - bw) / 2u;
    out->block_y = (lh - bh) / 2u;
    out->out_w   = ow;
    out->out_h   = oh;
    out->dst_x   = (target > ow) ? (target - ow) / 2u : 0u;
    return true;
}

/**
 * @brief Map the LOGICAL (post-rotation) trim window of @p fit back to a
 *        rectangle in the SOURCE block, which measures @p crop_w x @p crop_h.
 *
 * @p rotate_cw is 0..3 = 0/90/180/270 degrees CLOCKWISE, the page convention.
 * The inverse of each rotation, with logical column X and row Y:
 *   0:   src = (X, Y)                     logical is source
 *   1:   src = (Y, crop_h - 1 - X)        90 CW,  logical is crop_h x crop_w
 *   2:   src = (crop_w - 1 - X, crop_h - 1 - Y)
 *   3:   src = (crop_w - 1 - Y, X)        270 CW, logical is crop_h x crop_w
 * so a logical rectangle becomes a source rectangle with the axes swapped for
 * 90/270 and the origin reflected for 180/270 (x) and 90/180 (y).
 *
 * MIRRORING IS NOT APPLIED HERE. The PPA does hflip/vflip itself, and because
 * both trims are CENTRED the mirrored window is the same window, up to the one
 * pixel an odd (extent - block) split puts on the other side, which is below
 * anything visible on a 720 px panel.
 *
 * Outputs are clamped into the source block, so a caller that passed mismatched
 * dimensions gets a valid (if wrong) rectangle rather than an out-of-range PPA
 * job. Returns false only on a NULL argument or a zero-sized source.
 */
static inline bool image_fit_logical_to_source(const image_fit_t *fit,
                                               uint32_t crop_w, uint32_t crop_h,
                                               uint8_t rotate_cw,
                                               uint32_t *src_x, uint32_t *src_y,
                                               uint32_t *src_w, uint32_t *src_h)
{
    if (!fit || !src_x || !src_y || !src_w || !src_h) return false;
    if (crop_w == 0 || crop_h == 0) return false;

    const uint32_t X = fit->block_x, Y = fit->block_y;
    const uint32_t bw = fit->block_w, bh = fit->block_h;
    uint32_t x, y, w, h;

    switch (rotate_cw & 3u) {
    case 1:                                   /* 90 CW  */
        x = Y;              w = bh;
        y = (crop_h > X + bw) ? crop_h - X - bw : 0u;
        h = bw;
        break;
    case 2:                                   /* 180    */
        x = (crop_w > X + bw) ? crop_w - X - bw : 0u;
        w = bw;
        y = (crop_h > Y + bh) ? crop_h - Y - bh : 0u;
        h = bh;
        break;
    case 3:                                   /* 270 CW */
        x = (crop_w > Y + bh) ? crop_w - Y - bh : 0u;
        w = bh;
        y = X;              h = bw;
        break;
    default:                                  /* 0      */
        x = X;  y = Y;  w = bw;  h = bh;
        break;
    }

    if (x >= crop_w) x = crop_w - 1u;
    if (y >= crop_h) y = crop_h - 1u;
    if (w > crop_w - x) w = crop_w - x;
    if (h > crop_h - y) h = crop_h - y;

    *src_x = x; *src_y = y; *src_w = w; *src_h = h;
    return true;
}

#endif /* IMAGE_FIT_H */
