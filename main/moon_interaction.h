#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Single-finger drag-to-rotate state for the Moon page. Thread-safe: the TARGET
 * is written by the UI/touch task, the CURRENT (eased) orientation is advanced
 * and read by the render (poll) task. The render shows CURRENT, which eases
 * toward TARGET (set by the finger; driven home on release for snap-back). */
void moon_drag_begin(float x_px, float y_px);   /* finger down: snapshot start    */
void moon_drag_move(float x_px, float y_px);     /* finger move: set target yaw/pitch */
void moon_drag_end(void);                        /* finger up: rubber band -> target 0, or free spin -> hold + arm return */
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
/* Free-spin support (moon_spin_mode == 1). After a rotate-release in free-spin
 * mode, moon_drag_end() leaves the disc at its spun orientation and arms a
 * pending return. The render loop polls moon_drag_freespin_pending() to know a
 * hold is active, then moon_drag_freespin_elapsed() (passing the configured
 * moon_spin_return_s) to learn when the hold window has expired, and finally
 * moon_drag_trigger_return() to start the eased snap-back home. A new
 * moon_drag_begin() (re-touch) or moon_drag_reset() (page leave) clears the
 * pending state. */
bool moon_drag_freespin_pending(void);           /* true while a free-spin hold is armed */
bool moon_drag_freespin_converged(void);         /* true once a held disc has eased onto its spun target */
bool moon_drag_freespin_elapsed(uint8_t return_s);/* true once >= return_s elapsed since release */
void moon_drag_trigger_return(void);              /* drive target -> 0 and disarm the hold */
#ifdef __cplusplus
}
#endif
