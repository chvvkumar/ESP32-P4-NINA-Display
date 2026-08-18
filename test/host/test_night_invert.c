/* Host test for main/image_night_invert.h -- basemap-only darkening applied to
 * the NWS radar tile before it enters the animation ring. No ESP-IDF
 * dependency; assert-style like test/host/test_poll_backoff.c.
 *
 * The load-bearing assertion in this file is the ECHO PASSTHROUGH block: every
 * chromatic dBZ ramp colour must come back bit for bit. If that ever fails the
 * radar page is lying about reflectivity, which is worse than a bright screen.
 *
 * Colour data below was measured 2026-08-17 from eight live RIDGE-2 products
 * (SOUTHEAST, CONUS, HAWAII, PACNORTHWEST, KLSX, NORTHEAST, PACSOUTHWEST,
 * ALASKA), classified basemap-vs-echo by frame-to-frame persistence over each
 * product's ten-frame animated loop: a pixel identical in all ten frames is
 * basemap furniture, a pixel that changes over the 50-minute loop is weather.
 * See the header for the population statistics and the threshold margins. */
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
static uint8_t mn_of(uint16_t px) {
    uint8_t r = r8_of(px), g = g8_of(px), b = b8_of(px);
    uint8_t mn = r < g ? r : g; if (b < mn) mn = b;
    return mn;
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

    /* == THE COASTAL BUG ======================================================
     * Reported from the device: on HAWAII the pale blue ocean wash is 96.6% of
     * the tile at saturation 49. The old neutral-only rule (sat <= 8) passed it
     * straight through, so the land inverted to black and the ocean stayed
     * blinding. It must now darken -- but NOT to black, or the coastline, which
     * is most of what a radar map is for, disappears with it. */
    {
        uint16_t ocean_in  = pack(194, 234, 240);   /* measured ocean wash      */
        uint16_t land_in   = pack(254, 254, 254);   /* measured land fill       */
        uint16_t ocean_out = image_night_invert_px(ocean_in);
        uint16_t land_out  = image_night_invert_px(land_in);

        /* classification inputs, so a future threshold edit shows its working */
        check_u32("ocean RGB(194,234,240) 565 saturation", sat_of(ocean_in), 49);
        check_u32("ocean RGB(194,234,240) 565 min channel", mn_of(ocean_in), 198);
        check_u32("ocean RGB(194,234,240) 565 luma", luma_of(ocean_in), 225);
        check_true("ocean is NOT neutral (the old rule's blind spot)",
                   sat_of(ocean_in) > IMAGE_NIGHT_INVERT_SAT_MAX);
        check_true("ocean saturation is inside WASH_SAT_MAX",
                   sat_of(ocean_in) <= IMAGE_NIGHT_INVERT_WASH_SAT_MAX);
        check_true("ocean min channel is inside WASH_MIN_CH",
                   mn_of(ocean_in) >= IMAGE_NIGHT_INVERT_WASH_MIN_CH);
        check_true("ocean is blue-dominant (b > r)", b8_of(ocean_in) > r8_of(ocean_in));

        /* exact output contract: the white component subtracted away */
        check_u32("ocean -> exactly 0x0126", ocean_out, 0x0126);
        check_u32("ocean -> RGB(0,36,49)",
                  ((uint32_t)r8_of(ocean_out) << 16) |
                  ((uint32_t)g8_of(ocean_out) << 8) | b8_of(ocean_out),
                  ((uint32_t)0 << 16) | ((uint32_t)36 << 8) | 49);
        check_u32("ocean output luma", luma_of(ocean_out), 26);
        check_true("ocean got much darker (luma 225 -> under 48)",
                   luma_of(ocean_out) < 48);
        check_true("ocean did NOT go to black", ocean_out != 0x0000);

        /* land and ocean must remain TELLABLE APART: the coastline survives */
        check_u32("land -> exactly 0x0000", land_out, 0x0000);
        check_true("land and ocean outputs differ", ocean_out != land_out);
        check_true("ocean keeps its hue: output saturation >= 32",
                   sat_of(ocean_out) >= 32);
        check_u32("ocean output saturation", sat_of(ocean_out), 49);
        check_u32("land output saturation (flat black)", sat_of(land_out), 0);
        check_true("ocean/land luma separation >= 16",
                   abs_diff(luma_of(ocean_out), luma_of(land_out)) >= 16);
        check_true("ocean/land blue-channel separation >= 32",
                   abs_diff(b8_of(ocean_out), b8_of(land_out)) >= 32);
        check_true("ocean reads BLUE, not grey: b - r >= 32",
                   (int)b8_of(ocean_out) - (int)r8_of(ocean_out) >= 32);

        /* the ocean/land antialias band darkens the same way */
        static const struct { uint8_t r, g, b; } wash[] = {
            { 216, 241, 245 }, { 231, 246, 249 }, { 188, 227, 233 },
            { 192, 232, 238 }, { 193, 232, 238 }, { 182, 219, 228 },
        };
        for (size_t i = 0; i < sizeof(wash) / sizeof(wash[0]); i++) {
            uint16_t in  = pack(wash[i].r, wash[i].g, wash[i].b);
            uint16_t out = image_night_invert_px(in);
            char lbl[80];
            snprintf(lbl, sizeof(lbl), "wash(%u,%u,%u) darkened to luma < 48",
                     wash[i].r, wash[i].g, wash[i].b);
            check_true(lbl, out != in && luma_of(out) < 48);
        }
    }

    /* -- ECHO PASSTHROUGH: the whole point of basemap-only darkening ----------
     * Real NWS RIDGE dBZ ramp colours. Every one must return bit for bit; a
     * full-frame inversion would turn the greens magenta and make the legend
     * misreport reflectivity. First the legacy discrete NWS ramp... */
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
        /* Least saturated LEGACY ramp entry. Retained for continuity, but note
         * it is NOT the least saturated colour of the live RIDGE-2 product --
         * that continuous ramp descends through near-neutral grey, which is why
         * saturation alone can no longer carry the whole rule. */
        check_u32("least saturated legacy ramp colour (70 dBZ purple) spread",
                  sat_of(pack(0x98, 0x54, 0xc6)), 113);
    }

    /* ...then the LIVE RIDGE-2 echoes, measured in the map body and confirmed
     * as weather by zero frame-to-frame persistence across a ten-frame loop.
     * These are the colours that actually reach the panel. */
    {
        static const struct { const char *what; uint8_t r, g, b; } echo[] = {
            /* weak blue end -- the closest echoes to the water in appearance */
            { "weak blue (lowest sat echo measured)", 119, 133, 173 },
            { "blue",                                 101, 119, 168 },
            { "blue",                                 102, 120, 169 },
            { "blue",                                 103, 121, 169 },
            { "blue",                                 102, 120, 168 },
            { "blue",                                  92, 113, 166 },
            { "blue",                                  84, 107, 164 },
            { "deep blue",                             67,  94, 158 },
            { "deep blue",                             77, 106, 165 },
            { "deep blue",                             72, 100, 162 },
            { "deep blue",                             79, 107, 165 },
            { "deep blue",                             78, 107, 165 },
            { "deep blue",                             77, 107, 165 },
            /* THE misclassification risk: light blue over water, HAWAII */
            { "light blue over ocean (KEY)",           92, 167, 203 },
            { "light blue",                            92, 177, 201 },
            { "light blue",                            90, 185, 193 },
            { "blue-cyan",                             81, 136, 184 },
            { "blue-cyan",                             87, 153, 194 },
            { "blue-cyan",                             87, 153, 195 },
            { "blue-cyan",                             91, 174, 200 },
            { "blue-cyan",                             91, 175, 200 },
            { "teal",                                  86, 197, 180 },
            { "teal",                                  87, 196, 181 },
            { "sea-green",                             84, 204, 171 },
            { "sea-green",                             84, 204, 172 },
            { "sea-green",                             82, 211, 164 },
            { "sea-green",                             82, 211, 165 },
            { "green",                                 69, 213, 136 },
            { "green",                                 68, 213, 135 },
            { "dark green",                             9, 103,  10 },
            { "dark green",                            10, 112,  11 },
            { "dark green",                            10, 119,  12 },
            { "green",                                 11, 141,  14 },
            { "green",                                 11, 151,  16 },
            { "green",                                 12, 176,  17 },
            { "bright green",                          13, 199,  19 },
            { "bright green",                          14, 205,  20 },
            { "yellow",                               241, 208,   0 },
            { "yellow",                               245, 209,   0 },
            { "red 50 dBZ",                           250,   0,   0 },
            { "cyan 80 dBZ",                            0, 225, 236 },
            { "blue 85 dBZ",                           51,  50, 204 },
            { "magenta 65 dBZ",                       255,  10, 230 },
            { "purple 70 dBZ",                        171,   0, 254 },
        };
        for (size_t i = 0; i < sizeof(echo) / sizeof(echo[0]); i++) {
            uint16_t in = pack(echo[i].r, echo[i].g, echo[i].b);
            char lbl[90];
            snprintf(lbl, sizeof(lbl), "echo %s (%u,%u,%u) bit-for-bit",
                     echo[i].what, echo[i].r, echo[i].g, echo[i].b);
            check_u32(lbl, image_night_invert_px(in), in);
        }
    }

    /* -- THE PALE RAMP COLOURS: the reason `b > r` exists ---------------------
     * The RIDGE-2 ramp is continuous and passes through pale, low-saturation
     * bands that land inside the sat/min-channel box the ocean occupies. Each
     * of these is a real reflectivity colour and must survive untouched. The
     * 60 dBZ pale magenta is the sharpest case: saturation 52 and min channel
     * 203, against the ocean's 49 and 198 -- numerically almost the same pixel,
     * separated only by the fact that the ocean's blue exceeds its red. */
    {
        static const struct { const char *what; uint8_t r, g, b; } pale[] = {
            { "60 dBZ pale magenta (sat 52, mn 203)", 255, 202, 255 },
            { "-10 dBZ blue-grey (highest mn of any b>r ramp colour)",
                                                      158, 165, 166 },
            { "-10 dBZ blue-grey",                    156, 165, 167 },
            { "-8 dBZ blue-grey",                     152, 166, 168 },
            { "-5 dBZ pale olive",                    197, 198, 173 },
            { "-4 dBZ pale olive",                    200, 203, 177 },
            { "-3 dBZ pale olive",                    207, 210, 178 },
            { "-6 dBZ pale olive",                    189, 193, 174 },
            { "-15 dBZ blue-grey",                    138, 149, 161 },
            { "-18 dBZ blue-grey",                    122, 136, 151 },
        };
        for (size_t i = 0; i < sizeof(pale) / sizeof(pale[0]); i++) {
            uint16_t in = pack(pale[i].r, pale[i].g, pale[i].b);
            char lbl[90];
            /* every one of these must be chromatic, or it would fall to the
             * neutral rule and this test would be proving nothing */
            snprintf(lbl, sizeof(lbl), "pale ramp %s is chromatic", pale[i].what);
            check_true(lbl, sat_of(in) > IMAGE_NIGHT_INVERT_SAT_MAX);
            snprintf(lbl, sizeof(lbl), "pale ramp %s passes through", pale[i].what);
            check_u32(lbl, image_night_invert_px(in), in);
        }
        /* the specific numbers the header's margin claim rests on */
        check_u32("60 dBZ pale magenta saturation", sat_of(pack(255, 202, 255)), 52);
        check_u32("60 dBZ pale magenta min channel", mn_of(pack(255, 202, 255)), 203);
        check_true("...and it is excluded ONLY by b > r failing",
                   sat_of(pack(255, 202, 255)) <= IMAGE_NIGHT_INVERT_WASH_SAT_MAX &&
                   mn_of(pack(255, 202, 255)) >= IMAGE_NIGHT_INVERT_WASH_MIN_CH &&
                   !(b8_of(pack(255, 202, 255)) > r8_of(pack(255, 202, 255))));
        check_u32("-10 dBZ blue-grey min channel (binding constraint)",
                  mn_of(pack(158, 165, 166)), 156);
        check_u32("...margin below WASH_MIN_CH",
                  IMAGE_NIGHT_INVERT_WASH_MIN_CH - mn_of(pack(158, 165, 166)), 20);
    }

    /* -- RED ROADS pass through, and that is deliberate -----------------------
     * The interstate red and the ~62 dBZ ramp red are the same colour to the
     * bit, so no test can separate them and the ramp wins. Roads stay red;
     * their core is already dark (luma 55). Asserted so the behaviour is a
     * decision on the record rather than an oversight. */
    {
        static const struct { uint8_t r, g, b; } road[] = {
            { 155,  14,  14 }, { 157,  18,  18 }, { 161,  27,  27 },
            { 164,  38,  38 }, { 170,  54,  54 }, { 179,  71,  71 },
            { 183,  87,  87 }, { 199, 120, 120 }, { 213, 153, 153 },
            { 218, 166, 166 }, { 235, 208, 208 }, { 245, 232, 232 },
        };
        for (size_t i = 0; i < sizeof(road) / sizeof(road[0]); i++) {
            uint16_t in = pack(road[i].r, road[i].g, road[i].b);
            char lbl[80];
            snprintf(lbl, sizeof(lbl), "road red (%u,%u,%u) passes through",
                     road[i].r, road[i].g, road[i].b);
            check_u32(lbl, image_night_invert_px(in), in);
        }
        check_true("road core (155,14,14) is already dark (luma <= 64)",
                   luma_of(pack(155, 14, 14)) <= 64);
    }

    /* -- BOUNDARY PAIRS: just inside and just outside every threshold --------- */

    /* (1) IMAGE_NIGHT_INVERT_SAT_MAX, the neutral bound.
     * (0,8,0) expands to r8=0,g8=8,b8=0 -> spread 8 == threshold -> inverted.
     * (0,12,0) expands to g8=12 -> spread 12 > threshold -> not neutral, and
     * min channel 0 puts it nowhere near the wash class -> passthrough. */
    {
        uint16_t at   = pack(0, 8, 0);
        uint16_t over = pack(0, 12, 0);
        check_u32("bound1: spread of pack(0,8,0)", sat_of(at), 8);
        check_true("bound1: spread == SAT_MAX is INVERTED",
                   image_night_invert_px(at) != at);
        check_true("bound1: inverted result is neutral",
                   sat_of(image_night_invert_px(at)) <= IMAGE_NIGHT_INVERT_SAT_MAX);
        check_u32("bound1: spread of pack(0,12,0)", sat_of(over), 12);
        check_u32("bound1: spread > SAT_MAX PASSES THROUGH",
                  image_night_invert_px(over), over);
    }

    /* (2) IMAGE_NIGHT_INVERT_WASH_SAT_MAX (56). The nearest representable pair
     * straddling it in 565: saturation 54 (in) and 58 (out). Both hold the
     * min-channel and b>r conditions constant, so only saturation decides. */
    {
        uint16_t in_  = pack(181, 235, 189);   /* sat 54, mn 181, b > r */
        uint16_t out_ = pack(181, 239, 189);   /* sat 58, mn 181, b > r */
        check_u32("bound2: saturation of pack(181,235,189)", sat_of(in_), 54);
        check_u32("bound2: min channel (held constant)", mn_of(in_), 181);
        check_true("bound2: b > r (held constant)", b8_of(in_) > r8_of(in_));
        check_true("bound2: sat 54 <= WASH_SAT_MAX is DARKENED",
                   image_night_invert_px(in_) != in_);
        check_true("bound2: ...and lands dark", luma_of(image_night_invert_px(in_)) < 64);

        check_u32("bound2: saturation of pack(181,239,189)", sat_of(out_), 58);
        check_u32("bound2: min channel (held constant)", mn_of(out_), 181);
        check_true("bound2: b > r (held constant)", b8_of(out_) > r8_of(out_));
        check_u32("bound2: sat 58 > WASH_SAT_MAX PASSES THROUGH",
                  image_night_invert_px(out_), out_);
    }

    /* (3) IMAGE_NIGHT_INVERT_WASH_MIN_CH (176). The 5-bit red ladder steps
     * 173 -> 181 straight across it, so this pair straddles it exactly. */
    {
        uint16_t in_  = pack(181, 203, 231);   /* mn 181, sat 50, b > r */
        uint16_t out_ = pack(173, 195, 222);   /* mn 173, sat 49, b > r */
        check_u32("bound3: min channel of pack(181,203,231)", mn_of(in_), 181);
        check_u32("bound3: saturation (inside WASH_SAT_MAX)", sat_of(in_), 50);
        check_true("bound3: mn 181 >= WASH_MIN_CH is DARKENED",
                   image_night_invert_px(in_) != in_);
        check_true("bound3: ...and lands dark", luma_of(image_night_invert_px(in_)) < 64);

        check_u32("bound3: min channel of pack(173,195,222)", mn_of(out_), 173);
        check_u32("bound3: saturation (still inside WASH_SAT_MAX)", sat_of(out_), 49);
        check_true("bound3: b > r (held constant)", b8_of(out_) > r8_of(out_));
        check_u32("bound3: mn 173 < WASH_MIN_CH PASSES THROUGH",
                  image_night_invert_px(out_), out_);
    }

    /* (4) the b > r condition. Same saturation and min channel on both sides;
     * only the hue direction differs. Blue-dominant darkens, red-dominant and
     * the exact tie do not. */
    {
        uint16_t cool = pack(198, 235, 247);   /* b 247 > r 198 -> wash        */
        uint16_t warm = pack(231, 203, 198);   /* b 198 < r 231 -> passthrough */
        uint16_t tie  = pack(255, 202, 255);   /* b 255 == r 255 -> passthrough*/
        check_true("bound4: cool b > r", b8_of(cool) > r8_of(cool));
        check_true("bound4: cool is DARKENED", image_night_invert_px(cool) != cool);
        check_true("bound4: warm b < r", b8_of(warm) < r8_of(warm));
        check_true("bound4: warm min channel is inside WASH_MIN_CH",
                   mn_of(warm) >= IMAGE_NIGHT_INVERT_WASH_MIN_CH);
        check_true("bound4: warm saturation is inside WASH_SAT_MAX",
                   sat_of(warm) <= IMAGE_NIGHT_INVERT_WASH_SAT_MAX);
        check_u32("bound4: warm PASSES THROUGH (only b > r excludes it)",
                  image_night_invert_px(warm), warm);
        check_true("bound4: tie b == r", b8_of(tie) == r8_of(tie));
        check_u32("bound4: exact tie PASSES THROUGH", image_night_invert_px(tie), tie);
    }

    /* -- idempotency ---------------------------------------------------------
     * DOUBLE-APPLY IS NOT A NO-OP AND NOT AN EXACT INVOLUTION on neutrals: it
     * returns approximately the original grey, off by a level or two, because
     * the luma sum and the 5/6/5 write-back each round. Chromatic pixels ARE
     * exactly idempotent (passthrough twice). A wash becomes idempotent from
     * the second application on: its min channel is already 0, so it no longer
     * qualifies for the wash class and passes through.
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
        uint16_t ocean1 = image_night_invert_px(pack(194, 234, 240));
        check_u32("double-apply on the ocean is stable from the 2nd pass",
                  image_night_invert_px(ocean1), ocean1);
    }

    /* -- buffer loop, plus the degenerate inputs ----------------------------- */
    {
        uint16_t buf[5] = {
            pack(254, 254, 254),   /* background white  -> black */
            pack(0x02, 0xfd, 0x02),/* 20 dBZ green      -> unchanged */
            pack(50, 50, 50),      /* county line grey  -> light */
            pack(0xf8, 0x00, 0xfd),/* 65 dBZ magenta    -> unchanged */
            pack(194, 234, 240),   /* ocean wash        -> dark blue */
        };
        image_night_invert_rgb565(buf, 5);
        check_u32("buffer: white  -> 0x0000", buf[0], 0x0000);
        check_u32("buffer: green  unchanged", buf[1], pack(0x02, 0xfd, 0x02));
        check_true("buffer: dark grey brightened", luma_of(buf[2]) > 200);
        check_u32("buffer: magenta unchanged", buf[3], pack(0xf8, 0x00, 0xfd));
        check_u32("buffer: ocean  -> 0x0126", buf[4], 0x0126);

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
