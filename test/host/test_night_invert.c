/* Host test for main/image_night_invert.h -- neutral-only inversion applied to
 * the NWS radar tile before it enters the animation ring. No ESP-IDF
 * dependency; assert-style like test/host/test_poll_backoff.c.
 *
 * The load-bearing assertion in this file is the ECHO PASSTHROUGH block: every
 * chromatic dBZ ramp colour must come back bit for bit. If that ever fails the
 * radar page is lying about reflectivity, which is worse than a bright screen. */
#include "image_night_invert.h"
#include <stdio.h>

static int fails = 0;

static void check_u32(const char *label, uint32_t got, uint32_t expect) {
    printf("%-62s got=%-10u expect=%-10u %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_true(const char *label, int cond) {
    printf("%-62s %s\n", label, cond ? "OK" : "FAIL");
    if (!cond) fails++;
}

/* ---- RGB888 <-> RGB565 helpers, matching the header's bit conventions ---- */

static uint16_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) << 5)  |
                       (uint16_t)(b >> 3));
}
static uint8_t r8_of(uint16_t px) {
    uint8_t v = (uint8_t)((px >> 11) & 0x1f);
    return (uint8_t)((v << 3) | (v >> 2));
}
static uint8_t g8_of(uint16_t px) {
    uint8_t v = (uint8_t)((px >> 5) & 0x3f);
    return (uint8_t)((v << 2) | (v >> 4));
}
static uint8_t b8_of(uint16_t px) {
    uint8_t v = (uint8_t)(px & 0x1f);
    return (uint8_t)((v << 3) | (v >> 2));
}
static uint8_t sat_of(uint16_t px) {
    uint8_t r = r8_of(px), g = g8_of(px), b = b8_of(px);
    uint8_t mx = r > g ? r : g; if (b > mx) mx = b;
    uint8_t mn = r < g ? r : g; if (b < mn) mn = b;
    return (uint8_t)(mx - mn);
}
static uint8_t luma_of(uint16_t px) {
    return (uint8_t)((77 * r8_of(px) + 150 * g8_of(px) + 29 * b8_of(px)) >> 8);
}
static int abs_diff(int a, int b) { return a > b ? a - b : b - a; }

int main(void) {
    /* -- threshold justification: worst-case RGB565 spread of a TRUE grey -----
     * A grey v goes in as (v,v,v); the 5-bit and 6-bit channels round
     * differently, so it comes back with a nonzero saturation. The header sets
     * IMAGE_NIGHT_INVERT_SAT_MAX to 8 on the claim that this worst case is 4.
     * Brute-force all 256 levels rather than trusting the claim. */
    {
        uint8_t worst = 0;
        for (int v = 0; v < 256; v++) {
            uint8_t s = sat_of(pack((uint8_t)v, (uint8_t)v, (uint8_t)v));
            if (s > worst) worst = s;
        }
        check_u32("worst-case 565 spread over all 256 true greys", worst, 4);
        check_true("threshold (8) is >= that worst case",
                   IMAGE_NIGHT_INVERT_SAT_MAX >= worst);
    }

    /* -- the six measured KLSX_0.gif dominant colours ------------------------
     * All neutral: white sheet, grey lines, grey label/legend text. Each must
     * be classified neutral, inverted, and come back STILL neutral. */
    {
        static const uint8_t greys[6] = { 254, 217, 50, 230, 81, 243 };
        for (int i = 0; i < 6; i++) {
            uint8_t v = greys[i];
            uint16_t in  = pack(v, v, v);
            uint16_t out = image_night_invert_px(in);
            char lbl[80];

            snprintf(lbl, sizeof(lbl), "grey(%u,%u,%u) classified neutral", v, v, v);
            check_true(lbl, sat_of(in) <= IMAGE_NIGHT_INVERT_SAT_MAX);

            /* Exact contract: the output word is the canonical 565 grey of
             * (255 - luma). Reading the LUMA back is lossy (the 5/6/5
             * write-back re-quantizes, e.g. grey 217 -> inv 35 -> reads back
             * as 32), so assert the written word, not the round-tripped luma. */
            uint8_t inv = (uint8_t)(255 - luma_of(in));
            snprintf(lbl, sizeof(lbl), "grey(%u): luma %u -> neutral grey %u", v, luma_of(in), inv);
            check_u32(lbl, out, pack(inv, inv, inv));

            snprintf(lbl, sizeof(lbl), "grey(%u) inverted luma within 4 of %u", v, inv);
            check_true(lbl, abs_diff(luma_of(out), inv) <= 4);

            snprintf(lbl, sizeof(lbl), "grey(%u) result is still neutral", v);
            check_true(lbl, sat_of(out) <= IMAGE_NIGHT_INVERT_SAT_MAX);

            snprintf(lbl, sizeof(lbl), "grey(%u) actually changed (not a passthrough)", v);
            check_true(lbl, out != in);
        }
    }

    /* -- endpoints: the 81.9%-of-the-frame white, and pure black -------------- */
    {
        uint16_t white_in  = pack(254, 254, 254);
        uint16_t white_out = image_night_invert_px(white_in);
        check_u32("RGB(254,254,254) luma before", luma_of(white_in), 255);
        check_u32("RGB(254,254,254) -> near-black", luma_of(white_out), 0);
        check_u32("RGB(254,254,254) -> exactly 0x0000", white_out, 0x0000);

        uint16_t black_out = image_night_invert_px(pack(0, 0, 0));
        check_u32("RGB(0,0,0) -> near-white luma", luma_of(black_out), 255);
        check_u32("RGB(0,0,0) -> exactly 0xFFFF", black_out, 0xFFFF);
    }

    /* -- ECHO PASSTHROUGH: the whole point of neutral-only inversion ----------
     * Real NWS RIDGE dBZ ramp colours. Every one must return bit for bit; a
     * full-frame inversion would turn the greens magenta and make the legend
     * misreport reflectivity. */
    {
        static const struct { const char *name; uint8_t r, g, b; } ramp[] = {
            { "05 dBZ cyan",       0x04, 0xe9, 0xe7 },
            { "10 dBZ blue",       0x01, 0x9f, 0xf4 },
            { "15 dBZ deep blue",  0x03, 0x00, 0xf4 },
            { "20 dBZ green",      0x02, 0xfd, 0x02 },
            { "25 dBZ green",      0x01, 0xc5, 0x01 },
            { "30 dBZ dark green", 0x00, 0x8e, 0x00 },
            { "35 dBZ yellow",     0xfd, 0xf8, 0x02 },
            { "40 dBZ dark yellow",0xe5, 0xbc, 0x00 },
            { "45 dBZ orange",     0xfd, 0x95, 0x00 },
            { "50 dBZ red",        0xfd, 0x00, 0x00 },
            { "55 dBZ dark red",   0xd4, 0x00, 0x00 },
            { "60 dBZ darker red", 0xbc, 0x00, 0x00 },
            { "65 dBZ magenta",    0xf8, 0x00, 0xfd },
            { "70 dBZ purple",     0x98, 0x54, 0xc6 },
        };
        for (size_t i = 0; i < sizeof(ramp) / sizeof(ramp[0]); i++) {
            uint16_t in = pack(ramp[i].r, ramp[i].g, ramp[i].b);
            char lbl[80];
            snprintf(lbl, sizeof(lbl), "%s passes through UNCHANGED", ramp[i].name);
            check_u32(lbl, image_night_invert_px(in), in);
        }
        /* Least saturated ramp entry vs the threshold: states the margin.
         * 114 in RGB888, 113 once quantized to 565 -- still 14x the threshold. */
        check_u32("least saturated ramp colour (70 dBZ purple) spread",
                  sat_of(pack(0x98, 0x54, 0xc6)), 113);
    }

    /* -- boundary: just inside and just outside the threshold ----------------
     * Build pixels whose 8-bit spread straddles IMAGE_NIGHT_INVERT_SAT_MAX.
     * (0,8,0) expands to r8=0,g8=8,b8=0 -> spread 8 == threshold -> inverted.
     * (0,12,0) expands to g8=12 -> spread 12 > threshold -> passthrough. */
    {
        uint16_t at    = pack(0, 8, 0);
        uint16_t over  = pack(0, 12, 0);
        check_u32("boundary: spread of pack(0,8,0)", sat_of(at), 8);
        check_true("boundary: spread == threshold is INVERTED",
                   image_night_invert_px(at) != at);
        check_true("boundary: inverted result is neutral",
                   sat_of(image_night_invert_px(at)) <= IMAGE_NIGHT_INVERT_SAT_MAX);
        check_u32("boundary: spread of pack(0,12,0)", sat_of(over), 12);
        check_u32("boundary: spread > threshold PASSES THROUGH",
                  image_night_invert_px(over), over);
    }

    /* -- idempotency ---------------------------------------------------------
     * DOUBLE-APPLY IS NOT A NO-OP AND NOT AN EXACT INVOLUTION: it returns
     * approximately the original grey, off by a level or two, because the luma
     * sum and the 5/6/5 write-back each round. Chromatic pixels ARE exactly
     * idempotent (passthrough twice).
     *
     * Operationally this never happens: image_page_radar_add() is the single
     * call site, it runs once on a freshly decoded buffer before that buffer is
     * handed to the ring, and playback only BORROWS the stored buffer
     * (radar_show_idx -> swap_borrowed_buf) without re-transforming it. A ring
     * rebuild frees and re-fetches. So a double application is impossible, and
     * the near-involution below is a documented property, not a safety net. */
    {
        int worst = 0;
        for (int v = 0; v < 256; v++) {
            uint16_t in  = pack((uint8_t)v, (uint8_t)v, (uint8_t)v);
            uint16_t twice = image_night_invert_px(image_night_invert_px(in));
            int d = abs_diff(luma_of(in), luma_of(twice));
            if (d > worst) worst = d;
        }
        check_true("double-apply on greys returns within 4 luma levels", worst <= 4);
        check_true("double-apply on greys is NOT bit-exact (documented)", worst > 0);
        uint16_t echo = pack(0x02, 0xfd, 0x02);
        check_u32("double-apply on an echo colour is exactly idempotent",
                  image_night_invert_px(image_night_invert_px(echo)), echo);
    }

    /* -- buffer loop, plus the degenerate inputs ----------------------------- */
    {
        uint16_t buf[4] = {
            pack(254, 254, 254),   /* background white  -> black */
            pack(0x02, 0xfd, 0x02),/* 20 dBZ green      -> unchanged */
            pack(50, 50, 50),      /* county line grey  -> light */
            pack(0xf8, 0x00, 0xfd),/* 65 dBZ magenta    -> unchanged */
        };
        image_night_invert_rgb565(buf, 4);
        check_u32("buffer: white  -> 0x0000", buf[0], 0x0000);
        check_u32("buffer: green  unchanged", buf[1], pack(0x02, 0xfd, 0x02));
        check_true("buffer: dark grey brightened", luma_of(buf[2]) > 200);
        check_u32("buffer: magenta unchanged", buf[3], pack(0xf8, 0x00, 0xfd));

        /* px_count 0 must not touch the buffer; NULL must not crash. */
        uint16_t guard[1] = { 0xBEEF };
        image_night_invert_rgb565(guard, 0);
        check_u32("px_count 0 leaves the buffer alone", guard[0], 0xBEEF);
        image_night_invert_rgb565(NULL, 1000);
        image_night_invert_rgb565(NULL, 0);
        check_true("NULL buffer does not crash", 1);
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
