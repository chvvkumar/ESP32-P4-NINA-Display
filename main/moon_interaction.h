#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Single-finger drag-to-rotate state for the Moon page. Thread-safe: the TARGET
 * is written by the UI/touch task, the CURRENT (eased) orientation is advanced
 * and read by the render (poll) task. The render shows CURRENT, which eases
 * toward TARGET (set by the finger; driven home on release for snap-back). */
void moon_drag_begin(float x_px, float y_px);   /* finger down: snapshot start    */
void moon_drag_move(float x_px, float y_px);     /* finger move: set target yaw/pitch */
void moon_drag_end(void);                        /* finger up: target -> 0 (snap-back) */
void moon_drag_advance(float alpha);             /* render frame: ease current -> target */
/* Hard-reset all drag state (active/target/current -> home). Call on Image Display
 * page leave so a visit that ends mid-settle does not carry a stale orientation
 * into the next visit (which would snap the disc home on the first frame). */
void moon_drag_reset(void);
bool moon_drag_active(void);                     /* true while a finger is dragging */
void moon_drag_get(float *yaw_deg, float *pitch_deg);  /* CURRENT (eased) rotation to render */
/* True once the finger is up AND the eased current orientation is back home, so
 * the render loop can stop spinning and do the crisp full-res resting render. */
bool moon_drag_settled(void);
/* True if the in-progress / just-finished touch moved far enough to count as a
 * rotate (so the page-swipe handler should NOT also change pages). Reading it
 * does not clear it; it resets on the next moon_drag_begin. */
bool moon_drag_was_rotate(void);
#ifdef __cplusplus
}
#endif
