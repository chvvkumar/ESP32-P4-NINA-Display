/**
 * @file voice_store.c
 * @brief Custom voice-clip overrides persisted on the "storage" littlefs
 *        partition (see voice_store.h).
 *
 * Mount point is /spiffs_store: crash_log.c already owns /spiffs for its own
 * small "crashlog" partition, so this module uses a distinct base path.
 * (The paths keep their historical "spiffs" names; the filesystem is littlefs.)
 *
 * An unformatted partition (or a SPIFFS leftover from older firmware) fails
 * the mount and is auto-formatted.  littlefs formats lazily — it writes the
 * superblocks and erases blocks on first use — so the old ~70 s SPIFFS
 * first-boot format ordeal is gone.  The mount still runs on a dedicated
 * background task so boot is never blocked.  The task's stack MUST stay in
 * internal RAM (plain xTaskCreate): flash erase/write windows run with the
 * CPU cache disabled, and a PSRAM stack would fault when touched during them.
 */

#include "voice_store.h"
#include "audio_alert.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_littlefs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "voice_store";

#define VOICE_STORE_MOUNT     "/spiffs_store"
#define VOICE_STORE_DIR       VOICE_STORE_MOUNT "/voice"
#define VOICE_STORE_PARTITION "storage"

/* Longest CLIP_LIST name is 22 chars ("meridian_flip_starting"); %.40s bounds
 * the expansion so -Werror=format-truncation can prove the paths fit. */
#define VOICE_PATH_MAX 96
#define CLIP_NAME_FMT  "%.40s"

static SemaphoreHandle_t s_mutex = NULL;   /* guards scan vs save vs stats */
static volatile bool     s_ready = false;

/* Cached filesystem stats.  esp_littlefs_info() traverses the filesystem
 * (flash reads with the CPU cache disabled → potential one-frame display
 * freeze), so it must never run on an HTTP request path — littlefs is much
 * faster than SPIFFS here, but the freeze-avoidance rationale stands.
 * The background scan computes them once; save/reset keep them current
 * incrementally.  s_used_cache drifts by filesystem metadata overhead between
 * boots, which is acceptable for a UI statistic.  Guarded by s_mutex. */
/* Must stay > CLIP_COUNT (61 as of the per-event phrases).  Every index is
 * runtime-guarded below, so an overflow does not corrupt anything -- it just
 * silently drops the s_clip_size cache entry for the clips past the end, which
 * makes their custom uploads invisible to the stats.  The scan logs loudly if
 * that ever happens; raise this and the log goes away. */
#define VOICE_MAX_CLIPS 96
static size_t s_used_cache = 0;
static size_t s_total_cache = 0;
static size_t s_custom_cache = 0;
static size_t s_clip_size[VOICE_MAX_CLIPS];   /* 0 = built-in (no custom file) */
static volatile bool     s_formatting = false;
static volatile bool     s_wipe_pending = false;   /* factory reset requested
                                                    * before the store mounted;
                                                    * honoured by the mount task
                                                    * right after its scan */

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int clip_index(const char *name) {
    if (!name || !name[0]) return -1;
    int n = audio_alert_clip_count();
    for (int i = 0; i < n; i++) {
        const char *cn = audio_alert_clip_name(i);
        if (cn && strcmp(cn, name) == 0) return i;
    }
    return -1;
}

static void clip_path(char *out, size_t out_size, const char *name, const char *suffix) {
    /* Both %-expansions are precision-bounded so 20 + 40 + 12 + NUL always
     * fits VOICE_PATH_MAX under -Werror=format-truncation.
     *
     * littlefs file names are capped at CONFIG_LITTLEFS_OBJ_NAME_LEN
     * (default 64) bytes.  The tmp suffix stays ".t" (not ".tmp"/".pcm.tmp")
     * from the SPIFFS 32-byte era — no reason to rename existing files:
     * current worst case is "profile_changed.pcm" = 19 chars + NUL. */
    snprintf(out, out_size, VOICE_STORE_DIR "/" CLIP_NAME_FMT "%.12s", name, suffix);
}

/* Record clip idx's file now being new_size bytes and adjust the cached
 * totals by the difference.  Call with s_mutex held. */
static void cache_set_clip_size(int idx, size_t new_size) {
    if (idx < 0 || idx >= VOICE_MAX_CLIPS) return;
    size_t old = s_clip_size[idx];
    s_clip_size[idx] = new_size;
    s_custom_cache = (s_custom_cache >= old) ? s_custom_cache - old + new_size
                                             : new_size;
    s_used_cache = (s_used_cache >= old) ? s_used_cache - old + new_size
                                         : new_size;
}

/* Load voice/<name>.pcm into PSRAM and hand it to audio_alert.  Ownership of
 * the buffer transfers to audio_alert_set_override (freed via its retire
 * queue on the next replace/clear).  Call with s_mutex held. */
static bool load_and_apply(const char *name, int idx) {
    char path[VOICE_PATH_MAX];
    clip_path(path, sizeof(path), name, ".pcm");

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 1) return false;
    size_t sz = (size_t)st.st_size & ~(size_t)1;   /* PCM16: force even */

    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OOM loading %s (%u bytes)", path, (unsigned)sz);
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        heap_caps_free(buf);
        return false;
    }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    if (got != sz) {
        ESP_LOGW(TAG, "Short read on %s", path);
        heap_caps_free(buf);
        return false;
    }

    audio_alert_set_override(idx, buf, sz);
    return true;
}

/* ── Background mount + scan ────────────────────────────────────────────── */

static void voice_store_task(void *arg) {
    (void)arg;

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = VOICE_STORE_MOUNT,
        .partition_label        = VOICE_STORE_PARTITION,
        /* First-ever mount (or a SPIFFS leftover) formats.  littlefs formats
         * lazily — superblocks only, blocks erased on first use — so this is
         * quick, not the old ~70 s SPIFFS ordeal. */
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    s_formatting = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s — custom clips disabled this boot",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* littlefs has real directories: create voice/ if absent (EEXIST is fine,
     * so the result is ignored). */
    mkdir(VOICE_STORE_DIR, 0777);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int applied = 0;
    DIR *d = opendir(VOICE_STORE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *dot = strrchr(e->d_name, '.');
            char path[VOICE_PATH_MAX];
            snprintf(path, sizeof(path), VOICE_STORE_DIR "/" CLIP_NAME_FMT, e->d_name);
            if (dot && strcmp(dot, ".t") == 0) {
                /* Stale temp from an interrupted save: discard. */
                remove(path);
                continue;
            }
            if (!dot || strcmp(dot, ".pcm") != 0) continue;

            /* Account every .pcm (orphans included, matching the old
             * whole-dir walk) toward the cached custom total. */
            struct stat st;
            size_t fsz = (stat(path, &st) == 0) ? (size_t)st.st_size : 0;
            s_custom_cache += fsz;

            char stem[48];
            size_t stem_len = (size_t)(dot - e->d_name);
            if (stem_len == 0 || stem_len >= sizeof(stem)) continue;
            memcpy(stem, e->d_name, stem_len);
            stem[stem_len] = '\0';

            int idx = clip_index(stem);
            if (idx < 0) continue;
            if (idx < VOICE_MAX_CLIPS) s_clip_size[idx] = fsz;
            if (load_and_apply(stem, idx)) applied++;
        }
        closedir(d);
    }
    /* Fail loudly instead of quietly: past VOICE_MAX_CLIPS the size cache is
     * skipped above, so a custom upload for a high-index clip would play but
     * never show up in the stats.  Adding clips is what trips this. */
    if (audio_alert_clip_count() > VOICE_MAX_CLIPS) {
        ESP_LOGE(TAG, "VOICE_MAX_CLIPS (%d) < clip count (%d): raise it",
                 VOICE_MAX_CLIPS, audio_alert_clip_count());
    }

    /* One-time full-partition walk; every later stats request serves this
     * cached figure instead. */
    esp_littlefs_info(VOICE_STORE_PARTITION, &s_total_cache, &s_used_cache);
    s_ready = true;
    xSemaphoreGive(s_mutex);

    /* A factory reset issued while the store was still mounting/formatting
     * deferred its wipe here (see voice_store_wipe). */
    if (s_wipe_pending) {
        s_wipe_pending = false;
        ESP_LOGW(TAG, "Deferred factory-reset wipe of custom clips");
        voice_store_reset_all();
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t used = s_used_cache;
    size_t total = s_total_cache;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Ready: %d custom clip(s) applied, %u/%u bytes used",
             applied, (unsigned)used, (unsigned)total);
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void voice_store_init(void) {
    if (s_mutex) return;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Mutex alloc failed");
        return;
    }

    s_formatting = true;
    /* Internal-RAM stack is mandatory here (see file header). */
    if (xTaskCreate(voice_store_task, "voice_store", 6144, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        s_formatting = false;
    }
}

bool voice_store_ready(void) {
    return s_ready;
}

bool voice_store_formatting(void) {
    return s_formatting;
}

void voice_store_stats(size_t *used_bytes, size_t *total_bytes, size_t *custom_bytes) {
    size_t used = 0, total = 0, custom = 0;
    if (s_ready) {
        /* Cached figures only: never walk the filesystem on an HTTP request
         * path (a long flash op freezes the display for a frame). */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        used = s_used_cache;
        total = s_total_cache;
        custom = s_custom_cache;
        xSemaphoreGive(s_mutex);
    }
    if (used_bytes)   *used_bytes = used;
    if (total_bytes)  *total_bytes = total;
    if (custom_bytes) *custom_bytes = custom;
}

int voice_store_save(const char *clip_name, const uint8_t *data, size_t len) {
    if (!s_ready) return VOICE_STORE_ERR_NOT_READY;
    if (!data) return VOICE_STORE_ERR_IO;
    int idx = clip_index(clip_name);
    if (idx < 0) return VOICE_STORE_ERR_UNKNOWN_CLIP;

    size_t cap = (strcmp(clip_name, "boot_jingle") == 0) ? VOICE_STORE_CAP_JINGLE
                                                         : VOICE_STORE_CAP_CLIP;
    if (len == 0 || (len & 1) || len > cap) return VOICE_STORE_ERR_TOO_LARGE;

    char path[VOICE_PATH_MAX], tmp[VOICE_PATH_MAX];
    clip_path(path, sizeof(path), clip_name, ".pcm");
    clip_path(tmp, sizeof(tmp), clip_name, ".t");

    /* Phase 1 (mutex, brief): budget headroom after replacing this clip's
     * existing file, from the cache — no filesystem access. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t existing = (idx < VOICE_MAX_CLIPS) ? s_clip_size[idx] : 0;
    size_t custom = s_custom_cache;
    xSemaphoreGive(s_mutex);
    if (custom - existing + len > VOICE_STORE_BUDGET) {
        return VOICE_STORE_ERR_OVER_BUDGET;
    }

    /* Phase 2 (NO mutex): the tmp write (up to 480 KB) is still seconds of
     * flash I/O; holding s_mutex here would stall stats/scan callers.
     * httpd is single-task, so no concurrent save can race for the same tmp.
     * Write-then-rename so an interrupted save never corrupts the live file. */
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return VOICE_STORE_ERR_IO;
    }
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    if (wr != len) {
        remove(tmp);
        return VOICE_STORE_ERR_IO;
    }

    /* Phase 3 (mutex): re-check the budget — state may have changed while the
     * mutex was released — then commit, hot-apply and account. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    existing = (idx < VOICE_MAX_CLIPS) ? s_clip_size[idx] : 0;
    custom = s_custom_cache;
    if (custom - existing + len > VOICE_STORE_BUDGET) {
        remove(tmp);
        xSemaphoreGive(s_mutex);
        return VOICE_STORE_ERR_OVER_BUDGET;
    }
    /* littlefs rename atomically replaces an existing target (unlike SPIFFS),
     * so no pre-remove: the live clip survives a power cut mid-commit, and on
     * rename failure the old file — and its cache accounting — stay intact. */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        xSemaphoreGive(s_mutex);
        return VOICE_STORE_ERR_IO;
    }
    cache_set_clip_size(idx, len);

    /* Hot-apply from the caller's data (no need to re-read the file). */
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf) {
        memcpy(buf, data, len);
        audio_alert_set_override(idx, buf, len);
    } else {
        /* File is saved; the override will apply on next boot's scan. */
        ESP_LOGW(TAG, "OOM hot-applying %s; saved, applies on reboot", clip_name);
    }
    xSemaphoreGive(s_mutex);
    return VOICE_STORE_OK;
}

int voice_store_reset(const char *clip_name) {
    if (!s_ready) return VOICE_STORE_ERR_NOT_READY;
    int idx = clip_index(clip_name);
    if (idx < 0) return VOICE_STORE_ERR_UNKNOWN_CLIP;

    char path[VOICE_PATH_MAX];
    clip_path(path, sizeof(path), clip_name, ".pcm");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    remove(path);   /* missing file is fine: clearing a default clip is a no-op */
    cache_set_clip_size(idx, 0);
    audio_alert_set_override(idx, NULL, 0);
    xSemaphoreGive(s_mutex);
    return VOICE_STORE_OK;
}

int voice_store_reset_all(void) {
    if (!s_ready) return VOICE_STORE_ERR_NOT_READY;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = audio_alert_clip_count();
    for (int i = 0; i < n; i++) {
        const char *name = audio_alert_clip_name(i);
        if (!name) continue;
        char path[VOICE_PATH_MAX];
        clip_path(path, sizeof(path), name, ".pcm");
        remove(path);
        cache_set_clip_size(i, 0);
        audio_alert_set_override(i, NULL, 0);
    }
    xSemaphoreGive(s_mutex);
    return VOICE_STORE_OK;
}

bool voice_store_is_custom(const char *clip_name, size_t *size_out) {
    if (size_out) *size_out = 0;
    if (!s_ready) return false;
    int idx = clip_index(clip_name);
    if (idx < 0 || idx >= VOICE_MAX_CLIPS) return false;

    /* Cached presence: no per-clip stat() on the request path. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t sz = s_clip_size[idx];
    xSemaphoreGive(s_mutex);
    if (size_out) *size_out = sz;
    return sz > 0;
}

void voice_store_wipe(void) {
    if (!s_ready) {
        /* Store not mounted yet (factory reset while the background mount
         * task is still running): defer; the mount task performs the wipe
         * right after its scan completes, so an early factory reset cannot
         * leave custom clips behind. */
        s_wipe_pending = true;
        return;
    }
    voice_store_reset_all();
}
