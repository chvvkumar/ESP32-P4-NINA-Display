/**
 * @file moon_interaction.c
 * @brief Single-finger drag-to-rotate state for the Moon page.
 *
 * The accumulated yaw/pitch is written from the UI/touch task (PRESSED /
 * PRESSING / RELEASED events in nina_image_display.c) and read from the render
 * (poll) task that rebuilds the Moon sphere frame. Because RISC-V single-
 * precision float loads/stores are not guaranteed atomic across cores and we
 * read several related fields together, the float state is guarded by a
 * portMUX_TYPE spinlock — the same pattern nina_toast.c uses for its pending
 * queue. Critical sections only touch plain floats/bools, so they stay short.
 */

#include "moon_interaction.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <math.h>

/* Sensitivity: degrees of rotation per pixel of finger travel. */
#define MOON_DRAG_SENSITIVITY   0.35f
/* Movement past this many pixels counts as a deliberate rotate (not a tap/flick). */
#define MOON_DRAG_ROTATE_PX     8.0f
/* Pitch clamp so the pole never flips through. */
#define MOON_PITCH_MIN          (-89.0f)
#define MOON_PITCH_MAX          (89.0f)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Accumulated rotation (read by the render task). */
static float s_yaw_deg   = 0.0f;
static float s_pitch_deg = 0.0f;
/* Snapshot of yaw/pitch at finger-down, plus the down position. */
static float s_base_yaw   = 0.0f;
static float s_base_pitch = 0.0f;
static float s_start_x    = 0.0f;
static float s_start_y    = 0.0f;
/* True while a finger is down and dragging. */
static bool  s_active     = false;
/* True once the current/last gesture moved far enough to be a rotate. */
static bool  s_was_rotate = false;

void moon_drag_begin(float x_px, float y_px)
{
    portENTER_CRITICAL(&s_lock);
    s_active     = true;
    s_was_rotate = false;
    s_start_x    = x_px;
    s_start_y    = y_px;
    s_base_yaw   = s_yaw_deg;
    s_base_pitch = s_pitch_deg;
    portEXIT_CRITICAL(&s_lock);
}

void moon_drag_move(float x_px, float y_px)
{
    portENTER_CRITICAL(&s_lock);
    float dx = x_px - s_start_x;
    float dy = y_px - s_start_y;

    s_yaw_deg = s_base_yaw + MOON_DRAG_SENSITIVITY * dx;

    float pitch = s_base_pitch + MOON_DRAG_SENSITIVITY * dy;
    if (pitch < MOON_PITCH_MIN) pitch = MOON_PITCH_MIN;
    if (pitch > MOON_PITCH_MAX) pitch = MOON_PITCH_MAX;
    s_pitch_deg = pitch;

    if (sqrtf(dx * dx + dy * dy) > MOON_DRAG_ROTATE_PX) {
        s_was_rotate = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

void moon_drag_end(void)
{
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    /* Leave yaw/pitch as-is; the render task eases them back. */
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
    float yaw   = s_yaw_deg;
    float pitch = s_pitch_deg;
    portEXIT_CRITICAL(&s_lock);
    if (yaw_deg)   *yaw_deg   = yaw;
    if (pitch_deg) *pitch_deg = pitch;
}

void moon_drag_set(float yaw_deg, float pitch_deg)
{
    portENTER_CRITICAL(&s_lock);
    s_yaw_deg   = yaw_deg;
    s_pitch_deg = pitch_deg;
    portEXIT_CRITICAL(&s_lock);
}

bool moon_drag_was_rotate(void)
{
    portENTER_CRITICAL(&s_lock);
    bool was = s_was_rotate;
    portEXIT_CRITICAL(&s_lock);
    return was;
}
