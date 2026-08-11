#pragma once

/**
 * @file audio_alert.h
 * @brief Spoken alert playback over the onboard ES8311 speaker (thread-safe).
 *
 * audio_alert_speak() assembles a sentence out of embedded PCM clips and
 * enqueues it; a dedicated task drains the queue, opens the codec, plays the
 * clips back to back and closes it again (PA standby) once the queue empties.
 * Safe to call from ANY FreeRTOS task -- it never blocks and never touches the
 * codec itself.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "nina_alerts.h"   /* alert_type_t */
#include <stdbool.h>

/** Create the sentence queue and playback task.  Call once at startup. */
void audio_alert_init(void);

/**
 * Queue a spoken alert.  Thread-safe; drops the sentence (with a warning) if
 * the queue is full or alert_voice_enabled is off.
 *
 * @param type          Alert type; ALERT_SAFETY speaks "unsafe conditions".
 * @param instance_idx  NINA instance 0-2 ("instance one" .. "instance three").
 * @param value         Breaching value, spoken as digits; ignored for ALERT_SAFETY.
 */
void audio_alert_speak(alert_type_t type, int instance_idx, float value);

/* ── Test hooks (web_test_audio.c) ──────────────────────────────────────────
 * Both bypass the alert_voice_enabled gate so the speaker can be exercised
 * while voice alerts are switched off.  alert_voice_volume still applies. */

/** Play a single clip by filename stem ("chime", "digit_7", ...).
 *  @return false if the name is unknown or audio_alert_init() has not run. */
bool audio_alert_test_clip(const char *name);

/** Speak a full alert sentence.  Same assembly as audio_alert_speak(). */
void audio_alert_test_speak(alert_type_t type, int instance_idx, float value);

#ifdef __cplusplus
}
#endif
