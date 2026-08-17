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

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
