#pragma once

/**
 * @file voice_store.h
 * @brief Custom voice-clip overrides persisted on the "storage" SPIFFS
 *        partition (mounted at /spiffs_store).
 *
 * voice_store_init() spawns a background task that mounts the partition
 * (formatting it on the very first mount, ~70 s), then loads every
 * /spiffs_store/voice/<clip>.pcm into PSRAM and hot-applies it via
 * audio_alert_set_override().  Until that task finishes, voice_store_ready()
 * is false and every mutating call fails with VOICE_STORE_ERR_NOT_READY.
 *
 * Thread-safe: save/reset/stats are guarded by an internal mutex and may be
 * called from httpd worker tasks.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Total budget for all custom clips combined (bytes on SPIFFS). */
#define VOICE_STORE_BUDGET (4 * 1024 * 1024)

/** Per-clip size caps (bytes of 16 kHz/16-bit/mono PCM). */
#define VOICE_STORE_CAP_JINGLE 480000   /* "boot_jingle" only */
#define VOICE_STORE_CAP_CLIP   320000   /* every other clip */

/** Error returns of voice_store_save() / voice_store_reset*(). */
typedef enum {
    VOICE_STORE_OK               = 0,
    VOICE_STORE_ERR_NOT_READY    = -1,  /* store not mounted/scanned yet */
    VOICE_STORE_ERR_UNKNOWN_CLIP = -2,  /* name matches no audio_alert clip */
    VOICE_STORE_ERR_TOO_LARGE    = -3,  /* len zero, odd, or above the per-clip cap */
    VOICE_STORE_ERR_OVER_BUDGET  = -4,  /* total custom bytes would exceed VOICE_STORE_BUDGET */
    VOICE_STORE_ERR_IO           = -5,  /* SPIFFS/heap failure */
} voice_store_err_t;

/** Spawn the background mount+scan task.  Call once, after audio_alert_init(). */
void voice_store_init(void);

/** True once the mount+scan task has finished successfully. */
bool voice_store_ready(void);

/** True while the initial mount (and possible ~70 s first-boot format) runs. */
bool voice_store_formatting(void);

/** SPIFFS used/total bytes plus the summed size of files in voice/. */
void voice_store_stats(size_t *used_bytes, size_t *total_bytes, size_t *custom_bytes);

/**
 * Persist a custom clip (headerless 16 kHz/16-bit/mono PCM) and hot-apply it.
 * @return VOICE_STORE_OK or a negative voice_store_err_t value.
 */
int voice_store_save(const char *clip_name, const uint8_t *data, size_t len);

/** Delete a custom clip and revert playback to the embedded original. */
int voice_store_reset(const char *clip_name);

/** voice_store_reset() for every clip. */
int voice_store_reset_all(void);

/** True if a custom file exists for the clip; *size_out gets its size. */
bool voice_store_is_custom(const char *clip_name, size_t *size_out);

/** Remove all custom clips (factory reset).  No-op when the store is not ready. */
void voice_store_wipe(void);

#ifdef __cplusplus
}
#endif
