/**
 * @file moon_interaction.c
 * @brief Single-finger drag-to-rotate state for the Moon page.
 *
 * The render (poll) task that rebuilds the Moon sphere reads the CURRENT
 * (eased) yaw/pitch; the UI/touch task (PRESSED / PRESSING / RELEASED events in
 * nina_image_display.c) writes a TARGET yaw/pitch from the raw finger delta.
 * Decoupling target from current lets the render task ease toward the target
 * each frame (exponential smoothing), which both filters the noisy ~40Hz touch
 * stream and gives a magnitude-proportional snap-back when the finger releases
 * (target returns to 0, current eases home).
 *
 * Because RISC-V single-precision float loads/stores are not guaranteed atomic
 * across cores and we read several related fields together, the float state is
 * guarded by a portMUX_TYPE spinlock — the same pattern nina_toast.c uses for
 * its pending queue. Critical sections only touch plain floats/bools, so they
 * stay short.
 */

#include "moon_interaction.h"
#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include <math.h>

/* Sensitivity: degrees of rotation per pixel of finger travel. */
#define MOON_DRAG_SENSITIVITY   0.35f
/* Movement past this many pixels counts as a deliberate rotate (not a tap/flick). */
#define MOON_DRAG_ROTATE_PX     8.0f
/* Pitch clamp so the pole never flips through. */
#define MOON_PITCH_MIN          (-89.0f)
#define MOON_PITCH_MAX          (89.0f)

/* The per-frame exponential easing factor (cur += (target-cur)*alpha) is passed
 * IN to moon_drag_advance() by the render loop — see MOON_DRAG_EASE_A in tasks.c
 * (~0.35 at ~45fps: responsive but smooth). It lives there so the renderer owns
 * its own pacing/feel without recompiling this TU. */
/* Below this many degrees of residual the eased value snaps exactly to target,
 * so the disc never crawls forever toward a sub-pixel difference. */
#define MOON_DRAG_DEADBAND_DEG  0.05f
/* "Settled" threshold: |cur| under this (deg) for both axes counts as home, so
 * the render loop can stop spinning and do the crisp full-res resting render. */
#define MOON_DRAG_SETTLE_DEG    0.10f

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* TARGET rotation — where the finger wants the disc (written by moon_drag_move,
 * driven to 0 on release). */
static float s_target_yaw   = 0.0f;
static float s_target_pitch = 0.0f;
/* CURRENT rotation — what the render actually shows; eases toward target. */
static float s_cur_yaw      = 0.0f;
static float s_cur_pitch    = 0.0f;
/* Snapshot of the TARGET yaw/pitch at finger-down, plus the down position. */
static float s_base_yaw   = 0.0f;
static float s_base_pitch = 0.0f;
static float s_start_x    = 0.0f;
static float s_start_y    = 0.0f;
/* True while a finger is down and dragging. */
static bool  s_active     = false;
/* True once the current/last gesture moved far enough to be a rotate. */
static bool  s_was_rotate = false;
/* Free-spin (moon_spin_mode == 1): once armed by moon_drag_end() after a
 * rotate-release, the disc holds its spun orientation and s_return_at_us marks
 * the deadline (release time + moon_spin_return_s) after which the render loop
 * triggers the eased return home. Cleared on re-touch (begin), reset, or once
 * the return is triggered. */
static bool    s_return_pending = false;
static int64_t s_release_us     = 0;

void moon_drag_begin(float x_px, float y_px)
{
    portENTER_CRITICAL(&s_lock);
    s_active     = true;
    s_was_rotate = false;
    /* A new finger-down cancels any armed free-spin return (the gesture re-anchors
     * on the held orientation below, so the disc stays put and the countdown is
     * dropped until the next release). */
    s_return_pending = false;
    s_start_x    = x_px;
    s_start_y    = y_px;
    /* Anchor the gesture on the CURRENT eased orientation so picking the disc
     * back up mid-ease feels continuous (no jump to a stale target). */
    s_base_yaw    = s_cur_yaw;
    s_base_pitch  = s_cur_pitch;
    s_target_yaw   = s_cur_yaw;
    s_target_pitch = s_cur_pitch;
    portEXIT_CRITICAL(&s_lock);
}

void moon_drag_move(float x_px, float y_px)
{
    portENTER_CRITICAL(&s_lock);
    float dx = x_px - s_start_x;
    float dy = y_px - s_start_y;

    /* Finger delta sets the TARGET; the render task eases CURRENT toward it. */
    s_target_yaw = s_base_yaw + MOON_DRAG_SENSITIVITY * dx;

    float pitch = s_base_pitch + MOON_DRAG_SENSITIVITY * dy;
    if (pitch < MOON_PITCH_MIN) pitch = MOON_PITCH_MIN;
    if (pitch > MOON_PITCH_MAX) pitch = MOON_PITCH_MAX;
    s_target_pitch = pitch;

    if (sqrtf(dx * dx + dy * dy) > MOON_DRAG_ROTATE_PX) {
        s_was_rotate = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

void moon_drag_end(void)
{
    /* Read config OUTSIDE the spinlock (app_config_get() takes its own locks) and
     * sample the release timestamp before entering the critical section, per the
     * "no esp_timer/app_config inside the lock" discipline. */
    uint8_t spin_mode = app_config_get()->moon_spin_mode;
    int64_t now_us    = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    s_active = false;
    /* Free-spin only applies to an actual rotate; a tap (or any non-rotate touch)
     * always rubber-bands home so tap handling (thumbnail/info) is unaffected. */
    if (spin_mode == 1 && s_was_rotate) {
        /* Free spin: leave the TARGET where the finger last set it (do NOT zero it,
         * and do NOT collapse it onto the lagging CURRENT — keeping the finger's
         * target lets the render loop ease the last few degrees of follow-through to
         * exactly where the finger pointed). Arm a pending return; the render loop
         * waits for CURRENT to converge on TARGET, commits the crisp held frame,
         * then triggers moon_drag_trigger_return() once moon_spin_return_s has
         * elapsed since this release. */
        s_return_pending = true;
        s_release_us     = now_us;
    } else {
        /* Rubber band (default): drive the target home and let the render task ease
         * CURRENT back to the live sky orientation. Do NOT touch s_cur_* — the
         * eased advance does that, and zeroing it here would cause an abrupt jump. */
        s_target_yaw   = 0.0f;
        s_target_pitch = 0.0f;
        s_return_pending = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

void moon_drag_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    /* Hard reset of all drag state. Called on Image Display page leave so a visit
     * that ends mid-settle (the render loop breaks on !image_display_page_active
     * with a non-zero eased orientation) does not leave s_cur_* stale: otherwise
     * the next visit sees moon_drag_settled() false and snaps the disc from the
     * stale orientation to home on the first frame. */
    s_active       = false;
    s_was_rotate   = false;
    s_cur_yaw      = 0.0f;
    s_cur_pitch    = 0.0f;
    s_target_yaw   = 0.0f;
    s_target_pitch = 0.0f;
    /* Drop any armed free-spin hold so a fresh page visit starts at the live view. */
    s_return_pending = false;
    portEXIT_CRITICAL(&s_lock);
}

void moon_drag_advance(float alpha)
{
    portENTER_CRITICAL(&s_lock);
    /* Exponential smoothing toward the target, with a dead-band so residual
     * sub-MOON_DRAG_DEADBAND_DEG differences snap exactly (avoids infinite crawl). */
    float dyaw = s_target_yaw - s_cur_yaw;
    if (fabsf(dyaw) < MOON_DRAG_DEADBAND_DEG) s_cur_yaw = s_target_yaw;
    else                                      s_cur_yaw += dyaw * alpha;

    float dpitch = s_target_pitch - s_cur_pitch;
    if (fabsf(dpitch) < MOON_DRAG_DEADBAND_DEG) s_cur_pitch = s_target_pitch;
    else                                        s_cur_pitch += dpitch * alpha;
    portEXIT_CRITICAL(&s_lock);
}

bool moon_drag_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

void moon_drag_get(float *yaw_deg, float *pitch_deg)
{
    portENTER_CRITICAL(&s_lock);
    float yaw   = s_cur_yaw;       /* render the CURRENT (eased) orientation */
    float pitch = s_cur_pitch;
    portEXIT_CRITICAL(&s_lock);
    if (yaw_deg)   *yaw_deg   = yaw;
    if (pitch_deg) *pitch_deg = pitch;
}

bool moon_drag_settled(void)
{
    portENTER_CRITICAL(&s_lock);
    /* Settled = no finger down AND the eased current orientation is home. The
     * render loop uses this to know it can stop spinning and render the resting
     * full-res frame. */
    bool settled = !s_active &&
                   fabsf(s_cur_yaw)   < MOON_DRAG_SETTLE_DEG &&
                   fabsf(s_cur_pitch) < MOON_DRAG_SETTLE_DEG;
    portEXIT_CRITICAL(&s_lock);
    return settled;
}

bool moon_drag_was_rotate(void)
{
    portENTER_CRITICAL(&s_lock);
    bool was = s_was_rotate;
    portEXIT_CRITICAL(&s_lock);
    return was;
}

bool moon_drag_freespin_pending(void)
{
    portENTER_CRITICAL(&s_lock);
    /* A hold is only active while the finger is up; a re-touch clears it in
     * moon_drag_begin(), but guard on !s_active too for clarity. */
    bool pending = s_return_pending && !s_active;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

bool moon_drag_freespin_converged(void)
{
    portENTER_CRITICAL(&s_lock);
    /* A held disc is "converged" once the eased CURRENT orientation has reached
     * the spun TARGET (within the same dead-band the advance uses). The render
     * loop waits for this before committing the crisp held frame so it never
     * locks in a half-eased orientation on the first post-release frame. */
    bool converged = s_return_pending && !s_active &&
                     fabsf(s_cur_yaw   - s_target_yaw)   < MOON_DRAG_DEADBAND_DEG &&
                     fabsf(s_cur_pitch - s_target_pitch) < MOON_DRAG_DEADBAND_DEG;
    portEXIT_CRITICAL(&s_lock);
    return converged;
}

bool moon_drag_freespin_elapsed(uint8_t return_s)
{
    /* Sample the timer OUTSIDE the spinlock, then capture release time/flag inside. */
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool    pending    = s_return_pending && !s_active;
    int64_t release_us = s_release_us;
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return false;
    return (now_us - release_us) >= (int64_t)return_s * 1000000LL;
}

void moon_drag_trigger_return(void)
{
    portENTER_CRITICAL(&s_lock);
    /* Begin the eased snap-back home and disarm the hold. The render loop's
     * existing settle path takes over from here (target == 0 -> ease CURRENT
     * home, then moon_drag_settled() true -> crisp resting render). */
    s_target_yaw     = 0.0f;
    s_target_pitch   = 0.0f;
    s_return_pending = false;
    portEXIT_CRITICAL(&s_lock);
}
