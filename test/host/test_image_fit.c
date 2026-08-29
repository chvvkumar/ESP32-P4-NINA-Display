/* Host test for main/image_fit.h -- the pure geometry behind the single PPA SRM
 * pass that lands every image page on a 720 px wide picture at LVGL scale 256.
 * Header-only, integer-only, no ESP-IDF dependency; assert-style like
 * test/host/test_radar_play.c. The buffer ownership and PPA plumbing it feeds
 * live in ui/nina_image_page.c and cannot be host-compiled. */
#include "image_fit.h"
#include <stdio.h>

#define TARGET 720u

static int fails = 0;

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-64s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_u32(const char *label, uint32_t got, uint32_t expect) {
    printf("%-64s got=%-10u expect=%-10u %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

/* Every invariant image_fit_pick() promises, for one logical size against one
 * destination width. */
static void check_pick_t(uint32_t lw, uint32_t lh, uint32_t target) {
    char label[96];
    image_fit_t f;
    snprintf(label, sizeof(label), "pick %ux%u -> %u: succeeds", lw, lh, target);
    if (!image_fit_pick(lw, lh, target, &f)) {
        check_bool(label, false, true);
        return;
    }

    uint32_t n_up = (target * 16u + lw - 1u) / lw;
    bool ceil_branch = (f.n16 == n_up);
    uint32_t w_up = target * 16u / n_up;
    uint32_t crop_permille = (lw > w_up) ? (lw - w_up) * 1000u / lw : 0u;
    /* Same exact comparison the header makes (products, not the floored
     * permille above, which is display only: 1080 rounds 30.5 down to 30). */
    bool over_budget = lw > w_up &&
                       (lw - w_up) * 1000u > IMAGE_FIT_MAX_CROP_PERMILLE * lw;

    printf("  lw=%-5u lh=%-5u dst=%-4u n16=%-4u block=%ux%u@(%u,%u) out=%ux%u dst_x=%-3u %s crop=%u.%u%%\n",
           lw, lh, target, f.n16, f.block_w, f.block_h, f.block_x, f.block_y,
           f.out_w, f.out_h, f.dst_x, ceil_branch ? "fill " : "bands",
           crop_permille / 10u, crop_permille % 10u);

    /* The upper bound is the uint8_t itself (the PPA register is 8 bit), so
     * only the lower one is worth asserting: a 0 scale would write nothing. */
    snprintf(label, sizeof(label), "pick %ux%u -> %u: n16 >= 1", lw, lh, target);
    check_bool(label, f.n16 >= 1u, true);
    snprintf(label, sizeof(label), "pick %ux%u -> %u: out_w <= dst", lw, lh, target);
    check_bool(label, f.out_w <= target, true);
    snprintf(label, sizeof(label), "pick %ux%u -> %u: out_h <= dst", lw, lh, target);
    check_bool(label, f.out_h <= target, true);
    snprintf(label, sizeof(label), "pick %ux%u -> %u: out_w = block_w*n/16", lw, lh, target);
    check_u32(label, f.out_w, f.block_w * f.n16 / 16u);
    snprintf(label, sizeof(label), "pick %ux%u -> %u: out_h = block_h*n/16", lw, lh, target);
    check_u32(label, f.out_h, f.block_h * f.n16 / 16u);
    snprintf(label, sizeof(label), "pick %ux%u -> %u: block inside logical picture", lw, lh, target);
    check_bool(label, f.block_x + f.block_w <= lw && f.block_y + f.block_h <= lh, true);
    snprintf(label, sizeof(label), "pick %ux%u -> %u: block centred", lw, lh, target);
    check_bool(label, f.block_x == (lw - f.block_w) / 2u &&
                      f.block_y == (lh - f.block_h) / 2u, true);
    snprintf(label, sizeof(label), "pick %ux%u -> %u: dst_x centres the bands", lw, lh, target);
    check_u32(label, f.dst_x, (target - f.out_w) / 2u);
    /* The whole point of the rule: when the round-up branch was taken, the
     * centre crop it paid for must be within budget. */
    if (ceil_branch) {
        snprintf(label, sizeof(label), "pick %ux%u -> %u: fill branch crop <= 4%%", lw, lh, target);
        check_bool(label, !over_budget, true);
    } else {
        snprintf(label, sizeof(label), "pick %ux%u -> %u: bands branch was over budget", lw, lh, target);
        check_bool(label, over_budget, true);
    }
}

static void check_pick(uint32_t lw, uint32_t lh) { check_pick_t(lw, lh, TARGET); }

/* Map the logical trim back through each rotation and check it stays inside the
 * source crop window with the axes swapped where the rotation swaps them. */
static void check_map(uint32_t cw, uint32_t ch, uint8_t rot) {
    char label[96];
    uint32_t lw = (rot & 1u) ? ch : cw;
    uint32_t lh = (rot & 1u) ? cw : ch;
    image_fit_t f;
    if (!image_fit_pick(lw, lh, TARGET, &f)) {
        snprintf(label, sizeof(label), "map %ux%u rot%u: pick succeeds", cw, ch, rot);
        check_bool(label, false, true);
        return;
    }
    uint32_t x, y, w, h;
    snprintf(label, sizeof(label), "map %ux%u rot%u: maps", cw, ch, rot);
    check_bool(label, image_fit_logical_to_source(&f, cw, ch, rot, &x, &y, &w, &h), true);
    printf("  src %ux%u rot%u -> block %ux%u@(%u,%u)\n", cw, ch, rot, w, h, x, y);

    snprintf(label, sizeof(label), "map %ux%u rot%u: window inside source", cw, ch, rot);
    check_bool(label, x + w <= cw && y + h <= ch, true);
    snprintf(label, sizeof(label), "map %ux%u rot%u: extents match the logical block", cw, ch, rot);
    check_bool(label, (rot & 1u) ? (w == f.block_h && h == f.block_w)
                                 : (w == f.block_w && h == f.block_h), true);
}

int main(void) {
    printf("== image_fit_pick: square sources ==\n");
    const uint32_t sizes[] = {500, 600, 720, 900, 1024, 1080};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        check_pick(sizes[i], sizes[i]);
    }

    printf("\n== image_fit_pick: non-square sources ==\n");
    check_pick(600, 300);       /* 2:1 wide */
    check_pick(300, 600);       /* 1:2 tall: the height needs its own trim */
    check_pick(600, 392);       /* the CONUS radar tile */

    printf("\n== exact values the render path depends on ==\n");
    {
        image_fit_t f;
        /* 720 wide is already a whole 1.0x: no scale, no crop, no bands. */
        check_bool("720x720: picks", image_fit_pick(720, 720, TARGET, &f), true);
        check_u32 ("720x720: n16 = 16 (1.0x)", f.n16, 16u);
        check_u32 ("720x720: out_w = 720", f.out_w, 720u);
        check_u32 ("720x720: out_h = 720", f.out_h, 720u);
        check_u32 ("720x720: dst_x = 0", f.dst_x, 0u);
        check_u32 ("720x720: no crop", f.block_w, 720u);

        /* 900: ceil(11520/900) = 13, floor(11520/13) = 886, crop 1.5 % -> fill. */
        check_bool("900x900: picks", image_fit_pick(900, 900, TARGET, &f), true);
        check_u32 ("900x900: n16 = 13", f.n16, 13u);
        check_u32 ("900x900: block_w = 886", f.block_w, 886u);
        check_u32 ("900x900: block_x = 7", f.block_x, 7u);

        /* 600: ceil = 20 would cost a 4 % crop -> bands at n = 19. */
        check_bool("600x600: picks", image_fit_pick(600, 600, TARGET, &f), true);
        /* 4 % budget: 600 -> n16 20 (1.25x), block 576 = exactly 4 % trim, fills 720 */
        check_u32 ("600x600: n16 = 20", f.n16, 20u);
        check_u32 ("600x600: out_w = 720", f.out_w, 720u);
        check_u32 ("600x600: dst_x = 0", f.dst_x, 0u);
        check_u32 ("600x600: crop to 576", f.block_w, 576u);

        /* Tall source: the width fills the panel (n = 39, a 1.7 % centre crop)
         * and the height is centre-cropped to what that scale can show. */
        check_bool("300x600: picks", image_fit_pick(300, 600, TARGET, &f), true);
        check_u32 ("300x600: n16 = 39", f.n16, 39u);
        check_u32 ("300x600: block_h = 295 (centre-cropped)", f.block_h, 295u);
        check_u32 ("300x600: block_y = 152", f.block_y, 152u);
        check_u32 ("300x600: out_h = 719", f.out_h, 719u);
    }

    printf("\n== degenerate inputs ==\n");
    {
        image_fit_t f;
        check_bool("zero width rejected",  image_fit_pick(0, 720, TARGET, &f), false);
        check_bool("zero height rejected", image_fit_pick(720, 0, TARGET, &f), false);
        check_bool("zero target rejected", image_fit_pick(720, 720, 0, &f), false);
        /* Absurdly large source: clamps to 1/16 and centre-crops instead. */
        check_bool("12000 wide picks", image_fit_pick(12000, 12000, TARGET, &f), true);
        check_u32 ("12000 wide: n16 = 1", f.n16, 1u);
        check_u32 ("12000 wide: out_w = 720", f.out_w, 720u);
        /* Tiny source: n would exceed the register, so it clamps and bands. */
        check_bool("40 wide picks", image_fit_pick(40, 40, TARGET, &f), true);
        check_u32 ("40 wide: n16 = 255", f.n16, 255u);
        check_bool("40 wide: out_w <= 720", f.out_w <= TARGET, true);
    }

    printf("\n== image_fit_logical_to_source: every rotation ==\n");
    for (uint8_t rot = 0; rot < 4; rot++) {
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            check_map(sizes[i], sizes[i], rot);
        }
        check_map(600, 300, rot);
        check_map(300, 600, rot);
    }

    printf("\n== rotation mapping: known corners ==\n");
    {
        /* 1024 square: n = 11 (bands), no trim at all, so every rotation must
         * map to the whole source block. */
        image_fit_t f;
        uint32_t x, y, w, h;
        check_bool("1024: picks", image_fit_pick(1024, 1024, TARGET, &f), true);
        check_u32 ("1024: block_w = 1024 (untrimmed)", f.block_w, 1024u);
        for (uint8_t rot = 0; rot < 4; rot++) {
            char label[96];
            image_fit_logical_to_source(&f, 1024, 1024, rot, &x, &y, &w, &h);
            snprintf(label, sizeof(label), "1024 rot%u: origin (0,0)", rot);
            check_bool(label, x == 0u && y == 0u, true);
        }
        /* 900 square: trimmed by 14 px, 7 each side, so the origin is (7,7)
         * whichever way it is turned (the trim is symmetric). */
        check_bool("900: picks", image_fit_pick(900, 900, TARGET, &f), true);
        for (uint8_t rot = 0; rot < 4; rot++) {
            char label[96];
            image_fit_logical_to_source(&f, 900, 900, rot, &x, &y, &w, &h);
            snprintf(label, sizeof(label), "900 rot%u: origin (7,7)", rot);
            check_bool(label, x == 7u && y == 7u, true);
        }
        /* Wide 600x300 turned 90 CW: logical is 300x600, so the logical WIDTH
         * (the 300 px source height) sets the scale and the logical HEIGHT (the
         * 600 px source width) is what gets centre-cropped. The mapped window is
         * therefore taller than it is wide in source terms: src_w = block_h. */
        check_bool("600x300 rot1: picks", image_fit_pick(300, 600, TARGET, &f), true);
        image_fit_logical_to_source(&f, 600, 300, 1, &x, &y, &w, &h);
        check_u32 ("600x300 rot1: src_w = block_h (295)", w, f.block_h);
        check_u32 ("600x300 rot1: src_h = block_w (295)", h, f.block_w);
        check_bool("600x300 rot1: window inside source", x + w <= 600u && y + h <= 300u, true);
    }

    printf("\n== image_fit_pick: 800 px round panel (3.4C) ==\n");
    {
        const uint32_t sizes800[] = {500, 600, 720, 800, 900, 1024, 1080};
        for (unsigned i = 0; i < sizeof(sizes800) / sizeof(sizes800[0]); i++) {
            check_pick_t(sizes800[i], sizes800[i], 800u);
        }
        check_pick_t(600, 392, 800u);    /* the CONUS radar tile on an 800 panel */
        check_pick_t(300, 600, 800u);    /* tall source, height needs its own trim */

        image_fit_t f;
        /* 800 wide on an 800 panel: 1.0x, no scale, no crop, no bands. */
        check_bool("800x800 -> 800: picks", image_fit_pick(800, 800, 800u, &f), true);
        check_u32 ("800x800 -> 800: n16 = 16", f.n16, 16u);
        check_u32 ("800x800 -> 800: out_w = 800", f.out_w, 800u);
        check_u32 ("800x800 -> 800: dst_x = 0", f.dst_x, 0u);
        check_u32 ("800x800 -> 800: no crop", f.block_w, 800u);

        /* 900 -> 800: ceil(12800/900) = 15, floor(12800/15) = 853, a 5.2 %
         * crop, over the 4 % budget, so it bands at n = 14. */
        check_bool("900x900 -> 800: picks", image_fit_pick(900, 900, 800u, &f), true);
        check_u32 ("900x900 -> 800: n16 = 14", f.n16, 14u);
        check_u32 ("900x900 -> 800: out_w = 787", f.out_w, 787u);
        check_u32 ("900x900 -> 800: dst_x = 6", f.dst_x, 6u);
        check_u32 ("900x900 -> 800: no crop", f.block_w, 900u);

        /* 600 -> 800: ceil(12800/600) = 22, floor(12800/22) = 581, a 3.2 %
         * crop, inside the budget, so it fills. */
        check_bool("600x600 -> 800: picks", image_fit_pick(600, 600, 800u, &f), true);
        check_u32 ("600x600 -> 800: n16 = 22", f.n16, 22u);
        check_u32 ("600x600 -> 800: crop to 581", f.block_w, 581u);
        check_u32 ("600x600 -> 800: block_x = 9", f.block_x, 9u);
        check_u32 ("600x600 -> 800: out_w = 798", f.out_w, 798u);
        check_u32 ("600x600 -> 800: dst_x = 1", f.dst_x, 1u);

        /* The CONUS radar tile: the width fills as above and the height, being
         * under the fit limit, is not trimmed at all. */
        check_bool("600x392 -> 800: picks", image_fit_pick(600, 392, 800u, &f), true);
        check_u32 ("600x392 -> 800: block_h = 392 (untrimmed)", f.block_h, 392u);
        check_u32 ("600x392 -> 800: block_y = 0", f.block_y, 0u);
        check_u32 ("600x392 -> 800: out_h = 539", f.out_h, 539u);
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
