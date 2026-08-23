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
#include <stddef.h>
#include <stdint.h>

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

/* ── Event announcements (nina_websocket.c) ─────────────────────────────────
 * category_bit uses the toast_notify_mask bit layout: 0 Equipment Connects,
 * 1 Equipment Disconnects, 2 Sequence, 3 Focuser, 4 Mount, 5 Meridian Flip,
 * 6 Guider, 7 Safety, 8 Error, 9 Profile, 10 Dome, 11 Flat Device.
 * equipment_idx is an equipment_type_t value (nina_websocket.c) and is only
 * meaningful for categories 0/1; pass -1 otherwise. */

/**
 * Queue a spoken event announcement (live path).  Thread-safe; gated on
 * alert_voice_enabled, alert_voice_muted[instance], the voice_notify_mask
 * category bit, and a per-(category,instance) 30 s cooldown.
 */
void audio_alert_speak_event(int category_bit, int instance_idx, int equipment_idx);

/**
 * Queue one grouped equipment announcement for an aggregated burst
 * (live path).  category_bit must be 0 (connects) or 1 (disconnects);
 * eq_mask holds one bit per equipment_type_t value.  Speaks
 * "instance N <eq>, <eq> and <eq> connected/disconnected"; a single set bit
 * degenerates to the audio_alert_speak_event sentence.  Same gates and the
 * same per-(category,instance) 30 s cooldown as audio_alert_speak_event.
 */
void audio_alert_speak_equipment_group(int category_bit, int instance_idx,
                                       uint16_t eq_mask);

/**
 * Speak the same event sentence bypassing every gate except queue existence
 * (web preview endpoint; mirrors audio_alert_test_speak).
 */
void audio_alert_preview_event(int category_bit, int instance_idx, int equipment_idx);

/* ── Per-event spoken phrases (nina_websocket.c) ────────────────────────────
 * Distinct phrases for the events worth naming out loud; every other event
 * keeps the generic per-category clip above.  The mask bit for event e is
 * (VOICE_EVENT_BIT_BASE + e) in voice_notify_mask; bits 0-11 remain the
 * per-category gates.
 *
 * GATING: the per-event bit is checked ALONE.  It is NOT ANDed with the
 * event's category bit -- a user who ticks "Sequence finished" must hear it,
 * and the category bits keep governing only the generic fallback clips.  The
 * web UI groups the per-event checkboxes under their category heading so the
 * relationship reads naturally without being an enforced dependency.
 *
 * RESERVED: VOICE_EV_CONDITIONS_UNSAFE (value 8, mask bit 20) has NO clip and
 * NO producer.  The unsafe direction is already announced by the breach/alert
 * engine, which speaks the older `unsafe` clip ("Unsafe conditions.") and
 * repeats while the condition persists, so a per-event phrase here would
 * announce the same transition twice.  The enum value is kept in place -- do
 * NOT renumber: CONDITIONS_SAFE must stay value 9 / bit 21, and the config
 * migration sets bits 12-26 as a block.  Reviving it takes BOTH restoring the
 * clip (re-render conditions_unsafe.pcm via tools/gen_voice_clips.py, re-add
 * it to CLIP_LIST and EMBED_FILES) AND adding a producer.  Until then the
 * live and preview paths both refuse it and audio_alert_event_clip_name()
 * returns NULL for it. */
typedef enum {
    VOICE_EV_SEQUENCE_STARTED = 0,
    VOICE_EV_SEQUENCE_FINISHED,
    VOICE_EV_SEQUENCE_STEP_FAILED,
    VOICE_EV_AUTOFOCUS_COMPLETE,
    VOICE_EV_AUTOFOCUS_FAILED,
    VOICE_EV_MERIDIAN_FLIP_STARTING,
    VOICE_EV_MERIDIAN_FLIP_COMPLETE,
    VOICE_EV_MOUNT_PARKED,
    VOICE_EV_CONDITIONS_UNSAFE,     /* reserved: no clip, no producer */
    VOICE_EV_CONDITIONS_SAFE,
    VOICE_EV_GUIDING_STOPPED,
    VOICE_EV_DOME_SHUTTER_CLOSED,
    VOICE_EV_DOME_SHUTTER_OPENED,
    VOICE_EV_PLATESOLVE_FAILED,
    VOICE_EV_CAMERA_TIMEOUT,
    VOICE_EV_COUNT
} voice_event_t;

/** First mask bit used by voice_event_t.  Bits 0-11 are the category gates. */
#define VOICE_EVENT_BIT_BASE 12

/**
 * Queue a spoken per-event phrase (live path).  Thread-safe.  Gated on
 * alert_voice_enabled, alert_voice_muted[instance], the
 * (VOICE_EVENT_BIT_BASE + ev) bit of voice_notify_mask, and a per-(event,
 * instance) 30 s cooldown that is INDEPENDENT of the category cooldown.
 *
 * Sentence shape: chime [+ warning] + instance_N + the event's phrase clip.
 * The warning prefix is added for the failure/unsafe events only
 * (SEQUENCE_STEP_FAILED, AUTOFOCUS_FAILED, CONDITIONS_UNSAFE,
 * PLATESOLVE_FAILED, CAMERA_TIMEOUT), mirroring the category 1/5/7/8 rule.
 */
void audio_alert_speak_event_id(voice_event_t ev, int instance_idx);

/**
 * Speak the same per-event sentence bypassing every gate except queue
 * existence (web preview endpoint; mirrors audio_alert_preview_event).
 */
void audio_alert_preview_event_id(voice_event_t ev, int instance_idx);

/**
 * Clip basename for an event id ("sequence_started", ...), NULL out of range.
 * VOICE_EV_MERIDIAN_FLIP_COMPLETE returns the existing "meridian_flip".
 */
const char *audio_alert_event_clip_name(voice_event_t ev);

/* ── NINA link announcements (nina_connection.c) ──────────────────────────── */

/**
 * Queue a spoken NINA link connect/disconnect announcement (live path).
 * Thread-safe; gated on alert_voice_enabled, alert_voice_muted[instance] and
 * the alert_voice_conn / alert_voice_disc toggle for the given edge.
 */
void audio_alert_speak_conn(int instance_idx, bool connected);

/**
 * Speak the same link sentence bypassing every gate except queue existence
 * (web preview endpoint; mirrors audio_alert_preview_event).
 */
void audio_alert_preview_conn(int instance_idx, bool connected);

/**
 * Queue the startup jingle (call once from app_main after audio_alert_init).
 * Gated on boot_jingle_enabled; the queue drain naturally delays playback
 * until the codec opens.  Thread-safe, never blocks.
 */
void audio_alert_play_boot_jingle(void);

/**
 * Queue the startup jingle bypassing the enable gate (web preview endpoint;
 * mirrors audio_alert_preview_event).
 */
void audio_alert_preview_jingle(void);

/* ── Clip overrides (voice_store.c) ─────────────────────────────────────────
 * Custom PCM loaded from SPIFFS replaces the embedded clip at playback time. */

/** Number of embedded clips (CLIP_COUNT). */
int audio_alert_clip_count(void);

/** CLIP_LIST name for index idx ("chime", "digit_7", ...), NULL out of range. */
const char *audio_alert_clip_name(int idx);

/**
 * Install (non-NULL pcm) or clear (NULL pcm) a playback override for clip idx.
 * pcm must be a heap_caps_malloc'd buffer whose ownership transfers here; the
 * previous override buffer (if any) is retired via a free-queue drained by the
 * playback task between sentences, never freed inline.  Callers must be
 * serialized (voice_store holds its mutex around every call).
 */
void audio_alert_set_override(int idx, const uint8_t *pcm, size_t len);

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
