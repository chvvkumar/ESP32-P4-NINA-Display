/**
 * @file nina_sky_words.h
 * @brief Wording and number decisions shared by the two sky layouts (8 and 9),
 *        so a person switching layouts sees the same count, flip line and
 *        seconds. Header-only, no state.
 */
#ifndef NINA_SKY_WORDS_H
#define NINA_SKY_WORDS_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "nina_client.h"
#include "nina_sky_scene.h"

/* Picture count, the same meaning the Image-forward, Night rail, Two rings and
 * round overlay pages show: exposure_count of exposure_iterations (pictures
 * done in the running loop, capped at the loop length). Returns false when
 * there is nothing to say; @p buf needs 32 bytes. */
static inline bool nina_sky_words_count(const nina_client_t *d, char *buf, size_t n)
{
    int done  = d->exposure_count;
    int total = d->exposure_iterations;
    if (done < 0) {
        done = 0;
    }
    if (total > 0) {
        if (done > total) {
            done = total;
        }
        snprintf(buf, n, "%d of %d", done, total);
        return true;
    }
    if (done > 0) {
        snprintf(buf, n, "%d", done);
        return true;
    }
    return false;
}

/* "flip in N min" up to 30 min ahead, else "flip HH:MM" when the clock is
 * trusted, else the minutes again. */
static inline void nina_sky_words_flip(const nina_sky_scene_t *s, float minutes,
                                       char *buf, size_t n)
{
    if (minutes < 0.0f) {
        minutes = 0.0f;
    }
    if (minutes > 99999.0f) {
        minutes = 99999.0f;
    }
    if (minutes > 30.0f && s) {
        char t[8];
        if (nina_sky_scene_clock_after(s, minutes / 60.0f, t, sizeof(t))) {
            snprintf(buf, n, "flip %s", t);
            return;
        }
    }
    int m = (int)ceilf(minutes);
    if (m < 1) {
        m = 1;
    }
    snprintf(buf, n, "flip in %d min", m);
}

/* Elapsed seconds of the running picture, or -1 when none should be shown.
 * @p running   a picture is running now (not between pictures)
 * @p tick_secs the spine's 200 ms tick value, -1 when it has not run
 * @p polled    the last polled exposure_current, the same seconds from the
 *              poll when the tick has no clock yet
 * Both layouts call this, so they cannot disagree. */
static inline int nina_sky_words_secs(bool running, int tick_secs, float polled)
{
    if (!running) {
        return -1;
    }
    int s = tick_secs;
    if (s < 0) {
        if (polled < 0.0f) {
            return -1;
        }
        if (polled > 9999.0f) {
            polled = 9999.0f;
        }
        s = (int)polled;
    }
    return (s > 9999) ? 9999 : s;
}

#endif /* NINA_SKY_WORDS_H */
