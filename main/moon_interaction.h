#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Single-finger drag-to-rotate state for the Moon page. Thread-safe: written by
 * the UI/touch task, read by the render (poll) task. */
void moon_drag_begin(float x_px, float y_px);   /* finger down: snapshot start    */
void moon_drag_move(float x_px, float y_px);     /* finger move: accumulate yaw/pitch */
void moon_drag_end(void);                        /* finger up: stop active drag    */
bool moon_drag_active(void);                     /* true while a finger is dragging */
void moon_drag_get(float *yaw_deg, float *pitch_deg);  /* current accumulated rotation */
void moon_drag_set(float yaw_deg, float pitch_deg);    /* set rotation (snap-back easing) */
/* True if the in-progress / just-finished touch moved far enough to count as a
 * rotate (so the page-swipe handler should NOT also change pages). Reading it
 * does not clear it; it resets on the next moon_drag_begin. */
bool moon_drag_was_rotate(void);
#ifdef __cplusplus
}
#endif
