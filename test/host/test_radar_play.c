/* Host test for main/radar_play.h -- the pure decisions behind the Weather
 * Radar animation ring: frame dedupe, the playback cursor (oldest -> newest,
 * wrapping at the newest) and the per-frame dwell. Header-only, no ESP-IDF
 * dependency; assert-style like test/host/test_poll_backoff.c. */
#include "radar_play.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void check_int(const char *label, int got, int expect) {
    printf("%-60s got=%-6d expect=%-6d %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_u32(const char *label, uint32_t got, uint32_t expect) {
    printf("%-60s got=%-10u expect=%-10u %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-60s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void) {
    /* -- token validation = the {TOKEN}_loop.gif ban -------------------------
     * The URL builder formats "%s_%d.gif", so the frame index can never spell
     * "loop"; the reachable attack is a token carrying a query or fragment,
     * e.g. "KTLX_loop.gif?" -> ".../KTLX_loop.gif?_0.gif". Every such token
     * must be rejected so the caller falls back to CONUS. */
    check_bool("token: site id KTLX",          radar_token_valid("KTLX"), true);
    check_bool("token: region SOUTHEAST",      radar_token_valid("SOUTHEAST"), true);
    check_bool("token: national CONUS",        radar_token_valid("CONUS"), true);
    check_bool("token: Alaska PABC",           radar_token_valid("PABC"), true);
    check_bool("token: digits allowed",        radar_token_valid("K0X9"), true);

    check_bool("BAN: _loop.gif via query string",
               radar_token_valid("KTLX_loop.gif?"), false);
    check_bool("BAN: _loop.gif via fragment",
               radar_token_valid("KTLX_loop.gif#"), false);
    check_bool("BAN: bare loop token",         radar_token_valid("loop"), false);
    check_bool("BAN: underscore (builds _loop suffixes)",
               radar_token_valid("KTLX_loop"), false);
    check_bool("BAN: path traversal",          radar_token_valid("../loop"), false);
    check_bool("BAN: slash in token",          radar_token_valid("KTLX/loop"), false);
    check_bool("token: lowercase rejected",    radar_token_valid("ktlx"), false);
    check_bool("token: dot rejected",          radar_token_valid("KTLX.gif"), false);
    check_bool("token: empty rejected",        radar_token_valid(""), false);
    check_bool("token: NULL rejected",         radar_token_valid(NULL), false);
    check_bool("token: too short rejected",    radar_token_valid("KT"), false);
    check_bool("token: 15 chars accepted",     radar_token_valid("ABCDEFGHIJKLMNO"), true);
    check_bool("token: 16 chars rejected",     radar_token_valid("ABCDEFGHIJKLMNOP"), false);

    /* -- FNV-1a: stable, order-sensitive, distinguishes near-identical data -- */
    {
        const char a[] = "radar frame bytes";
        const char b[] = "radar frame byteS";      /* one bit different */
        uint32_t ha = radar_fnv1a(a, sizeof(a) - 1);
        uint32_t hb = radar_fnv1a(b, sizeof(b) - 1);
        check_u32("hash: known vector \"\" (offset basis)",
                  radar_fnv1a("", 0), 2166136261u);
        check_u32("hash: known vector \"a\"", radar_fnv1a("a", 1), 0xe40c292cu);
        check_u32("hash: known vector \"foobar\"",
                  radar_fnv1a("foobar", 6), 0xbf9cf968u);
        check_u32("hash: same bytes hash the same", radar_fnv1a(a, sizeof(a) - 1), ha);
        check_bool("hash: one changed byte changes the hash", ha != hb, true);
        /* Byte order matters: a length-only or sum-based hash would collide. */
        check_bool("hash: transposed bytes differ",
                   radar_fnv1a("ab", 2) != radar_fnv1a("ba", 2), true);
    }

    /* -- dedupe -------------------------------------------------------------- */
    check_bool("dup: empty ring never dedupes", radar_frame_is_dup(0x1234u, 0, 0x1234u), false);
    check_bool("dup: identical to the neighbour is a duplicate",
               radar_frame_is_dup(0x1234u, 3, 0x1234u), true);
    check_bool("dup: different from the neighbour is kept",
               radar_frame_is_dup(0x1234u, 3, 0x1235u), false);

    /* -- playback cursor -----------------------------------------------------
     * Index 0 is the NEWEST frame; playback runs oldest -> newest, so the
     * cursor counts DOWN and wraps from 0 back to count-1. */
    check_int("play: full ring wraps 0 -> 9",   radar_play_next(0, 10), 9);
    check_int("play: 9 -> 8",                    radar_play_next(9, 10), 8);
    check_int("play: 2 -> 1",                    radar_play_next(2, 10), 1);
    check_int("play: 1 -> 0 (newest)",           radar_play_next(1, 10), 0);
    check_int("play: single frame stays at 0",   radar_play_next(0, 1), 0);
    check_int("play: empty ring stays at 0",     radar_play_next(0, 0), 0);
    check_int("play: two frames wrap 0 -> 1",    radar_play_next(0, 2), 1);
    check_int("play: two frames 1 -> 0",         radar_play_next(1, 2), 0);

    /* A cursor left past the end by a ring that shrank must land in range, not
     * index freed memory. */
    check_int("play: cursor past the end clamps to oldest",
              radar_play_next(9, 4), 3);
    check_int("play: negative cursor clamps to oldest",
              radar_play_next(-1, 4), 3);

    /* One full pass from the oldest frame visits every frame exactly once and
     * finishes on the newest, which is the frame that then gets the long hold
     * before the loop restarts. */
    {
        const int count = 10;
        int seen[10];
        memset(seen, 0, sizeof(seen));
        int idx = count - 1;            /* a fresh ring starts on the oldest */
        seen[idx]++;
        for (int step = 0; step < count - 1; step++) {
            idx = radar_play_next(idx, count);
            if (idx >= 0 && idx < count) seen[idx]++;
        }
        int visited_once = 1;
        for (int i = 0; i < count; i++) if (seen[i] != 1) visited_once = 0;
        check_bool("play: one pass visits all 10 frames exactly once",
                   visited_once != 0, true);
        check_int("play: the pass ends on the newest frame", idx, 0);
        check_u32("play: and that frame gets the long hold",
                  radar_play_period_ms(idx), RADAR_PLAY_NEWEST_MS);
        check_int("play: the step after that wraps to the oldest",
                  radar_play_next(idx, count), count - 1);
    }

    /* -- per-slot bake generation: two geometries can never BOTH be shown ------
     * A crop / dark-mode / Red-Night change re-bakes the ring one slot at a
     * time, so mid-pass the ring genuinely holds frames of two geometries.
     * Every frame is scaled to the panel WIDTH, so playing through them renders
     * the picture at two sizes and it visibly pumps. Rather than trying to keep
     * the ring momentarily consistent (it cannot be — the re-bake takes real
     * time, 18.4 s in the logged failure), playback only ever advances to slots
     * carrying the CURRENT bake generation. A stale slot is never displayed. */
    {
        const uint32_t G = 7;                 /* current bake generation */
        uint32_t all_cur[10];
        for (int i = 0; i < 10; i++) all_cur[i] = G;

        /* All current: identical to the plain cursor, at every position. */
        int same_as_plain = 1;
        for (int cur = 0; cur < 10; cur++) {
            if (radar_play_next_baked(cur, 10, all_cur, G) != radar_play_next(cur, 10))
                same_as_plain = 0;
        }
        check_bool("bake: all slots current -> normal advance, every position",
                   same_as_plain != 0, true);

        /* A full pass over an all-current ring still visits each slot once. */
        {
            int seen3[10];
            memset(seen3, 0, sizeof(seen3));
            int idx = 9;
            seen3[idx]++;
            for (int step = 0; step < 9; step++) {
                idx = radar_play_next_baked(idx, 10, all_cur, G);
                if (idx >= 0 && idx < 10) seen3[idx]++;
            }
            int once3 = 1;
            for (int i = 0; i < 10; i++) if (seen3[i] != 1) once3 = 0;
            check_bool("bake: all current - one pass still visits all 10 once",
                       once3 != 0, true);
        }

        /* Mid re-transform: the pass converts the ON-SCREEN slot first, so a
         * realistic mixed ring has a few current slots and the rest stale.
         * Slots 5,6,7 converted; the rest still hold the old geometry. */
        uint32_t mixed[10];
        for (int i = 0; i < 10; i++) mixed[i] = (i >= 5 && i <= 7) ? G : G - 1;
        check_int("bake: from 9 skips stale 8 to the nearest current slot 7",
                  radar_play_next_baked(9, 10, mixed, G), 7);
        check_int("bake: 7 -> 6, both current, plain step",
                  radar_play_next_baked(7, 10, mixed, G), 6);
        check_int("bake: 5 wraps past six stale slots back to 7",
                  radar_play_next_baked(5, 10, mixed, G), 7);
        /* NEGATIVE CONTROL for the skip: the plain cursor WOULD land on a stale
         * slot from both of those positions. If radar_play_next_baked ever
         * degenerates into radar_play_next, the two assertions above fail. */
        check_bool("bake: NEGATIVE CONTROL - plain cursor would show a stale slot",
                   mixed[radar_play_next(9, 10)] != G &&
                   mixed[radar_play_next(5, 10)] != G, true);
        {
            /* Exhaustive: from EVERY starting position, the answer is either a
             * current slot or an unchanged cursor (a hold). Never a stale slot. */
            int never_stale = 1;
            for (int cur = 0; cur < 10; cur++) {
                int n = radar_play_next_baked(cur, 10, mixed, G);
                if (n != cur && mixed[n] != G) never_stale = 0;
            }
            check_bool("bake: no start position ever advances onto a stale slot",
                       never_stale != 0, true);
        }

        /* ALL stale: hold on whatever is displayed. Not a blank screen, not an
         * out-of-range index -- the one case where freezing is correct. */
        uint32_t all_stale[10];
        for (int i = 0; i < 10; i++) all_stale[i] = G - 1;
        int held_all = 1;
        for (int cur = 0; cur < 10; cur++) {
            if (radar_play_next_baked(cur, 10, all_stale, G) != cur) held_all = 0;
        }
        check_bool("bake: no current slot anywhere -> hold, every position",
                   held_all != 0, true);

        /* ...and the hold ENDS as soon as one slot is re-baked or a fresh frame
         * lands at the head. This is the mid-sequence recovery. */
        uint32_t one_cur[10];
        for (int i = 0; i < 10; i++) one_cur[i] = G - 1;
        one_cur[0] = G;                        /* a fresh newest frame arrives */
        check_int("bake: a single re-baked slot ends the hold",
                  radar_play_next_baked(4, 10, one_cur, G), 0);
        check_int("bake: sitting ON the only current slot holds (no redraw)",
                  radar_play_next_baked(0, 10, one_cur, G), 0);
        one_cur[3] = G;                        /* the pass converts another */
        check_int("bake: as the pass converts slots they rejoin the loop",
                  radar_play_next_baked(0, 10, one_cur, G), 3);

        /* Single-slot ring. */
        uint32_t one[1] = { G };
        check_int("bake: single-slot ring, current -> stays on slot 0",
                  radar_play_next_baked(0, 1, one, G), 0);
        one[0] = G - 1;
        check_int("bake: single-slot ring, stale -> holds on slot 0",
                  radar_play_next_baked(0, 1, one, G), 0);

        /* Empty ring and a NULL table: return the cursor, touch nothing. */
        check_int("bake: empty ring returns the cursor unchanged",
                  radar_play_next_baked(3, 0, all_cur, G), 3);
        check_int("bake: NULL generation table returns the cursor unchanged",
                  radar_play_next_baked(3, 10, NULL, G), 3);

        /* A generation counter that wrapped must not read as current. */
        uint32_t wrapped[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        check_int("bake: wrapped generations read as stale (hold)",
                  radar_play_next_baked(1, 2, wrapped, 0u), 1);

        /* The count the log line reports. */
        check_int("bake: stale count over a mixed ring", radar_stale_count(mixed, 10, G), 7);
        check_int("bake: stale count over an all-current ring",
                  radar_stale_count(all_cur, 10, G), 0);
        check_int("bake: stale count over an all-stale ring",
                  radar_stale_count(all_stale, 10, G), 10);
        check_int("bake: stale count of an empty ring is 0",
                  radar_stale_count(all_stale, 0, G), 0);
    }

    /* -- re-transform slot order: the visible slot converts FIRST ------------- */
    {
        const int count = 10;
        const int start = 4;                 /* the slot on screen */
        check_int("order: step 0 is the slot currently on screen",
                  radar_retransform_idx(start, 0, count), start);
        /* NEGATIVE CONTROL: without the rotation the pass would start at slot 0
         * and the visible frame would convert LAST -- the pumping bug. */
        check_bool("order: NEGATIVE CONTROL - step 0 is not plain slot 0",
                   radar_retransform_idx(start, 0, count) != 0, true);
        int seen2[10];
        memset(seen2, 0, sizeof(seen2));
        for (int k = 0; k < count; k++) {
            int i = radar_retransform_idx(start, k, count);
            if (i >= 0 && i < count) seen2[i]++;
        }
        int once = 1;
        for (int i = 0; i < count; i++) if (seen2[i] != 1) once = 0;
        check_bool("order: one pass converts every slot exactly once", once != 0, true);
        check_int("order: it wraps past the newest back to slot 0",
                  radar_retransform_idx(start, count - start, count), 0);
        /* A cursor left dangling by a ring that shrank must still be in range. */
        int inrange = 1;
        for (int k = 0; k < 4; k++) {
            int i = radar_retransform_idx(99, k, 4);
            if (i < 0 || i >= 4) inrange = 0;
            int j = radar_retransform_idx(-1, k, 4);
            if (j < 0 || j >= 4) inrange = 0;
        }
        check_bool("order: out-of-range start still yields in-range slots",
                   inrange != 0, true);
        check_int("order: empty ring is a safe 0", radar_retransform_idx(0, 0, 0), 0);
    }

    /* -- dwell --------------------------------------------------------------- */
    check_u32("dwell: newest frame is held longer",
              radar_play_period_ms(0), RADAR_PLAY_NEWEST_MS);
    check_u32("dwell: history frame uses the short dwell",
              radar_play_period_ms(5), RADAR_PLAY_FRAME_MS);
    check_bool("dwell: the hold is longer than a normal frame",
               RADAR_PLAY_NEWEST_MS > RADAR_PLAY_FRAME_MS, true);

    /* -- ring ceiling matches what RIDGE actually serves (_0.._9) ------------- */
    check_int("ring: max frames is 10 (_10.gif is a 404)", RADAR_RING_MAX, 10);

    /* -- generation: the region-switch staleness gate -------------------------
     * A backfill lives ~9 s (nine frames, ~1 s apart). Switching region clears
     * the ring mid-backfill, and the in-flight backfill kept appending frames
     * from the OLD region into it, so the animation cycled both regions. The
     * ring now carries a generation, bumped by every reset, and the single
     * insert point rejects any frame carrying an older one. */
    check_bool("gen: current generation is accepted",
               radar_frame_is_stale(7u, 7u), false);
    check_bool("gen: an older generation is rejected",
               radar_frame_is_stale(7u, 8u), true);
    check_bool("gen: generation 0 is not special",
               radar_frame_is_stale(0u, 0u), false);
    /* The counter is uint32_t and only ever incremented, so it wraps rather
     * than saturating; the wrapped value must still read as a change. */
    check_bool("gen: wrap 0xFFFFFFFF -> 0 still reads as stale",
               radar_frame_is_stale(0xFFFFFFFFu, 0u), true);

    /* The interleaving that actually caused the bug. This models the ring
     * ITSELF (a slot array plus the generation), because the real insert path
     * -- image_page_radar_add() in main/ui/nina_image_page.c -- owns PSRAM
     * buffers, takes the LVGL display lock and drives lv_timer, none of which
     * host-compiles. What is shared with the firmware is the decision under
     * test: radar_frame_is_stale(), called at the same single insert point. */
    {
        int      ring[RADAR_RING_MAX];
        int      count = 0;
        uint32_t gen   = 0;

        /* One insert, mirroring the order in image_page_radar_add(): the
         * staleness test comes first, then the frame lands at the tail. */
        #define RING_INSERT(tag, fetched_gen)                                  \
            do {                                                               \
                if (!radar_frame_is_stale((fetched_gen), gen) &&               \
                    count < RADAR_RING_MAX) {                                  \
                    ring[count++] = (tag);                                     \
                }                                                              \
            } while (0)

        uint32_t old_gen = gen;          /* the backfill captures this... */
        RING_INSERT(100, old_gen);       /* ...and its first frame lands */
        check_int("gen: a frame under the current generation is accepted", count, 1);

        /* User switches region: the ring is reset and the generation bumped. */
        count = 0;
        gen++;
        check_bool("gen: invalidate changes the generation",
                   radar_frame_is_stale(old_gen, gen), true);

        /* The backfill for the OLD region is still alive and keeps inserting. */
        RING_INSERT(101, old_gen);
        RING_INSERT(102, old_gen);
        check_int("gen: stale frames after a reset are all rejected", count, 0);

        /* The new region's fetch, issued after the bump, is accepted. */
        uint32_t new_gen = gen;
        RING_INSERT(200, new_gen);

        check_int("gen: reset + stale insert + fresh insert leaves ONE frame", count, 1);
        check_int("gen: and that frame is the new region's",
                  count == 1 ? ring[0] : -1, 200);

        /* Interleaved the other way round: a stale insert arriving AFTER the
         * new region's frame must not append behind it either. */
        RING_INSERT(103, old_gen);
        check_int("gen: a late stale frame cannot append behind a fresh one", count, 1);
        #undef RING_INSERT
    }

    /* -- fit geometry (radar_fit_rect) ---------------------------------------
     * The panel is 720x720 and the render path scales every frame to the panel
     * WIDTH, then centres it vertically. So the only property that decides
     * whether black bars appear is the frame's ASPECT: square = no bars.
     * 600x550 is the site/regional tile, 600x392 the national (CONUS) one and
     * 600x571 the SOUTHEAST regional one.
     *
     * TWO modes. The middle "88% uniform trim" is gone: it threw away map area
     * and still left the bars, so it was strictly dominated. CROP now means
     * "drop the 24-row NOAA header and the 24-row dBZ legend, then take the
     * largest centred square of what is left" -- no chrome and no bars. */
    {
        uint16_t w = 0, h = 0, x = 0, y = 0;

        /* OFF: nothing is copied and nothing is written. */
        w = h = x = y = 0xEEEE;
        check_bool("fit OFF: 600x550 declines to crop",
                   radar_fit_rect(RADAR_FIT_OFF, 600, 550, &w, &h, &x, &y), false);
        check_int("fit OFF: outputs untouched", (int)w, 0xEEEE);

        check_int("fit CROP: chrome band is the measured 24 rows",
                  RADAR_NOAA_CHROME_PX, 24);

        check_bool("fit CROP: 600x550 (KLSX site) crops",
                   radar_fit_rect(RADAR_FIT_CROP, 600, 550, &w, &h, &x, &y), true);
        check_int("fit CROP: 600x550 -> w 502 (550 - 2*24)", (int)w, 502);
        check_int("fit CROP: 600x550 -> h 502", (int)h, 502);
        check_int("fit CROP: 600x550 -> x 49 (centred)", (int)x, 49);
        check_int("fit CROP: 600x550 -> y 24 (below the header)", (int)y, 24);
        check_bool("fit CROP: 600x550 is square, so no bars", w == h, true);
        check_int("fit CROP: 600x550 stops before the legend (y+h)",
                  (int)y + (int)h, 526);

        check_bool("fit CROP: 600x392 (CONUS) crops",
                   radar_fit_rect(RADAR_FIT_CROP, 600, 392, &w, &h, &x, &y), true);
        check_int("fit CROP: 600x392 -> w 344 (392 - 2*24)", (int)w, 344);
        check_int("fit CROP: 600x392 -> h 344", (int)h, 344);
        check_int("fit CROP: 600x392 -> x 128 (centred)", (int)x, 128);
        check_int("fit CROP: 600x392 -> y 24", (int)y, 24);
        check_bool("fit CROP: 600x392 is square", w == h, true);

        /* SOUTHEAST is a third height; 600-523 = 77 is odd, and the truncating
         * divide puts the spare column on the RIGHT, so x is 38 not 39. */
        check_bool("fit CROP: 600x571 (SOUTHEAST) crops",
                   radar_fit_rect(RADAR_FIT_CROP, 600, 571, &w, &h, &x, &y), true);
        check_int("fit CROP: 600x571 -> w 523 (571 - 2*24)", (int)w, 523);
        check_int("fit CROP: 600x571 -> h 523", (int)h, 523);
        check_int("fit CROP: 600x571 -> x 38 (77/2 truncates down)", (int)x, 38);
        check_int("fit CROP: 600x571 -> y 24", (int)y, 24);

        /* LEGACY STORED VALUES. A device may hold radar_crop == 2 (the retired
         * separate "fill screen") or == 1 (the retired 88% trim). Any non-zero
         * mode must resolve to the one surviving geometry, so an existing user
         * silently gets the better crop with no config migration. The clamp
         * lives in radar_fit_rect() itself, the one function every consumer
         * calls, so no load path can bypass it. */
        {
            uint16_t w2 = 0, h2 = 0, x2 = 0, y2 = 0;
            check_bool("legacy: stored 2 (old \"fill\") still crops",
                       radar_fit_rect(2, 600, 550, &w2, &h2, &x2, &y2), true);
            check_bool("legacy: stored 2 resolves to the SAME rect as 1",
                       w2 == 502 && h2 == 502 && x2 == 49 && y2 == 24, true);
            w2 = h2 = x2 = y2 = 0;
            check_bool("legacy: any other non-zero byte also resolves to crop",
                       radar_fit_rect(255, 600, 392, &w2, &h2, &x2, &y2), true);
            check_bool("legacy: and to the same rect as 1",
                       w2 == 344 && h2 == 344 && x2 == 128 && y2 == 24, true);
            check_bool("legacy: only 0 means \"show the whole picture\"",
                       radar_fit_rect(0, 600, 550, &w2, &h2, &x2, &y2), false);
        }

        /* Already square: still 48 rows shorter, because the chrome is there
         * too. A square OUT would mean chrome left ON, which is the old bug. */
        check_bool("fit CROP: 550x550 crops",
                   radar_fit_rect(RADAR_FIT_CROP, 550, 550, &w, &h, &x, &y), true);
        check_int("fit CROP: square in -> side 502, not 550", (int)w, 502);
        check_int("fit CROP: square in -> h 502", (int)h, 502);
        check_int("fit CROP: square in -> x 24", (int)x, 24);
        check_int("fit CROP: square in -> y 24", (int)y, 24);

        /* Taller than wide (no NWS tile is, but the maths must not pick w, and
         * the square must sit centred INSIDE the chrome-stripped band). */
        check_bool("fit CROP: 400x600 crops",
                   radar_fit_rect(RADAR_FIT_CROP, 400, 600, &w, &h, &x, &y), true);
        check_int("fit CROP: tall in -> side 400 (width binds)", (int)w, 400);
        check_bool("fit CROP: tall in is square", w == h, true);
        check_int("fit CROP: tall in -> x 0", (int)x, 0);
        check_int("fit CROP: tall in -> y 100 (24 + (552-400)/2)", (int)y, 100);
        check_bool("fit CROP: tall in stays clear of the legend",
                   (int)y + (int)h <= 600 - RADAR_NOAA_CHROME_PX, true);

        /* -- degenerate / defensive -----------------------------------------
         * The source is a remote server that can change the product size at any
         * time, so h - 48 must never underflow into a 65500-pixel crop. */
        check_bool("fit: zero width refused",
                   radar_fit_rect(RADAR_FIT_CROP, 0, 550, &w, &h, &x, &y), false);
        check_bool("fit: zero height refused",
                   radar_fit_rect(RADAR_FIT_CROP, 600, 0, &w, &h, &x, &y), false);
        check_bool("fit: 0x0 refused",
                   radar_fit_rect(RADAR_FIT_CROP, 0, 0, &w, &h, &x, &y), false);
        check_bool("fit OFF: zero size refused too (mode is tested first)",
                   radar_fit_rect(RADAR_FIT_OFF, 0, 0, &w, &h, &x, &y), false);

        /* h <= 48: too short to hold both chrome bands. Falls back to the plain
         * largest centred square rather than underflowing. Exactly 48 is the
         * boundary and must take the fallback, not produce a zero-height band. */
        check_bool("fit CROP: h == 48 falls back instead of underflowing",
                   radar_fit_rect(RADAR_FIT_CROP, 600, 48, &w, &h, &x, &y), true);
        check_int("fit CROP: h 48 -> side 48 (largest square)", (int)w, 48);
        check_int("fit CROP: h 48 -> h 48", (int)h, 48);
        check_int("fit CROP: h 48 -> y 0", (int)y, 0);
        check_int("fit CROP: h 48 -> x 276", (int)x, 276);

        check_bool("fit CROP: h == 49 uses the chrome path",
                   radar_fit_rect(RADAR_FIT_CROP, 600, 49, &w, &h, &x, &y), true);
        check_int("fit CROP: h 49 -> side 1 (49 - 48)", (int)w, 1);
        check_int("fit CROP: h 49 -> y 24", (int)y, 24);

        check_bool("fit CROP: 1x1 falls back and still crops",
                   radar_fit_rect(RADAR_FIT_CROP, 1, 1, &w, &h, &x, &y), true);
        check_int("fit CROP: 1x1 -> side 1", (int)w, 1);
        check_int("fit CROP: 1x1 -> y 0", (int)y, 0);

        /* -- the invariant the caller's memcpy depends on ---------------------
         * ox + ow <= w and oy + oh <= h, for EVERY mode (including the legacy
         * bytes) and every size we can think of. A rect that escapes this reads
         * past the decoded buffer. */
        {
            static const uint16_t sizes[][2] = {
                {600,550}, {600,392}, {600,571}, {550,550}, {400,600},
                {1,1}, {1,600}, {600,1}, {2,2}, {47,47}, {48,48}, {49,49},
                {600,47}, {600,48}, {600,49}, {600,96}, {600,97},
                {65535,65535}, {65535,1}, {1,65535}, {720,720}, {3,5},
            };
            static const uint8_t modes[] = {
                RADAR_FIT_OFF, RADAR_FIT_CROP, 2, 99, 255
            };
            int bad = 0, rects = 0;
            for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
                for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
                    uint16_t cw = 0, chh = 0, cx = 0, cy = 0;
                    if (!radar_fit_rect(modes[m], sizes[i][0], sizes[i][1],
                                        &cw, &chh, &cx, &cy)) continue;
                    rects++;
                    if (cw == 0 || chh == 0) bad++;
                    if ((uint32_t)cx + cw > sizes[i][0]) bad++;
                    if ((uint32_t)cy + chh > sizes[i][1]) bad++;
                    if (cw > sizes[i][0] || chh > sizes[i][1]) bad++;
                    if (cw != chh) bad++;                 /* every crop is square now */
                }
            }
            check_bool("fit: every rect produced was non-empty", rects > 0, true);
            check_int("fit: offset+side within bounds for every mode and size",
                      bad, 0);
        }

        /* CROP must actually clear the chrome on every REAL product size --
         * this is the assertion that fails if RADAR_NOAA_CHROME_PX is wrong. */
        {
            static const uint16_t real[][2] = {{600,550}, {600,392}, {600,571}};
            int chrome_left = 0;
            for (size_t i = 0; i < 3; i++) {
                uint16_t cw = 0, chh = 0, cx = 0, cy = 0;
                if (!radar_fit_rect(RADAR_FIT_CROP, real[i][0], real[i][1],
                                    &cw, &chh, &cx, &cy)) { chrome_left++; continue; }
                if (cy < RADAR_NOAA_CHROME_PX) chrome_left++;                       /* header visible */
                if (cy + chh > real[i][1] - RADAR_NOAA_CHROME_PX) chrome_left++;    /* legend visible */
                if (cw != chh) chrome_left++;                                       /* bars are back */
            }
            check_int("fit CROP: no NOAA chrome survives on any real product size",
                      chrome_left, 0);
        }
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
