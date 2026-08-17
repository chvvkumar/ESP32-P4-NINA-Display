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
