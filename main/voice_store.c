/**
 * @file voice_store.c
 * @brief Custom voice-clip overrides persisted on the "storage" SPIFFS
 *        partition (see voice_store.h).
 *
 * Mount point is /spiffs_store: crash_log.c already owns /spiffs for its own
 * small "crashlog" partition, so this module uses a distinct base path.
 *
 * The very first mount of the (never-before-formatted) 15.9 MB partition
 * formats it, which takes ~70 s.  That work runs on a dedicated background
 * task so boot is never blocked.  The task's stack MUST stay in internal RAM
 * (plain xTaskCreate): SPIFFS format erases flash with the CPU cache
 * disabled, and a PSRAM stack would fault when touched during those windows.
 */

#include "voice_store.h"
#include "audio_alert.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "voice_store";

#define VOICE_STORE_MOUNT     "/spiffs_store"
#define VOICE_STORE_DIR       VOICE_STORE_MOUNT "/voice"
#define VOICE_STORE_PARTITION "storage"

/* Longest CLIP_LIST name is 15 chars; %.40s bounds the expansion so
 * -Werror=format-truncation can prove the paths fit. */
#define VOICE_PATH_MAX 96
#define CLIP_NAME_FMT  "%.40s"

static SemaphoreHandle_t s_mutex = NULL;   /* guards scan vs save vs stats */
static volatile bool     s_ready = false;
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
     * SPIFFS object names (the path minus the mount point) are capped at
     * CONFIG_SPIFFS_OBJ_NAME_LEN = 32 bytes including the NUL.  The tmp
     * suffix is ".t" (not ".tmp"/".pcm.tmp") to stay under that ceiling:
     * current worst case is "/voice/profile_changed.pcm" = 26 chars + NUL. */
    snprintf(out, out_size, VOICE_STORE_DIR "/" CLIP_NAME_FMT "%.12s", name, suffix);
}

/* Summed size of every voice/<clip>.pcm.  Call with s_mutex held. */
static size_t custom_bytes_locked(void) {
    size_t sum = 0;
    DIR *d = opendir(VOICE_STORE_DIR);
    if (!d) return 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".pcm") != 0) continue;
        char path[VOICE_PATH_MAX];
        snprintf(path, sizeof(path), VOICE_STORE_DIR "/" CLIP_NAME_FMT, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0) sum += (size_t)st.st_size;
    }
    closedir(d);
    return sum;
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

    esp_vfs_spiffs_conf_t conf = {
        .base_path              = VOICE_STORE_MOUNT,
        .partition_label        = VOICE_STORE_PARTITION,
        .max_files              = 4,
        .format_if_mount_failed = true,   /* first-ever mount formats: ~70 s */
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    s_formatting = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s — custom clips disabled this boot",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* SPIFFS has a flat namespace: '/' in filenames just works and mkdir is
     * unsupported.  Attempt it anyway for VFS layers that care; ignore result. */
    mkdir(VOICE_STORE_DIR, 0777);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int applied = 0;
    DIR *d = opendir(VOICE_STORE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *dot = strrchr(e->d_name, '.');
            if (dot && strcmp(dot, ".t") == 0) {
                /* Stale temp from an interrupted save: discard. */
                char path[VOICE_PATH_MAX];
                snprintf(path, sizeof(path), VOICE_STORE_DIR "/" CLIP_NAME_FMT, e->d_name);
                remove(path);
                continue;
            }
            if (!dot || strcmp(dot, ".pcm") != 0) continue;

            char stem[48];
            size_t stem_len = (size_t)(dot - e->d_name);
            if (stem_len == 0 || stem_len >= sizeof(stem)) continue;
            memcpy(stem, e->d_name, stem_len);
            stem[stem_len] = '\0';

            int idx = clip_index(stem);
            if (idx < 0) continue;
            if (load_and_apply(stem, idx)) applied++;
        }
        closedir(d);
    }
    s_ready = true;
    xSemaphoreGive(s_mutex);

    /* A factory reset issued while the store was still mounting/formatting
     * deferred its wipe here (see voice_store_wipe). */
    if (s_wipe_pending) {
        s_wipe_pending = false;
        ESP_LOGW(TAG, "Deferred factory-reset wipe of custom clips");
        voice_store_reset_all();
    }

    size_t used = 0, total = 0;
    esp_spiffs_info(VOICE_STORE_PARTITION, &total, &used);
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
        esp_spiffs_info(VOICE_STORE_PARTITION, &total, &used);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        custom = custom_bytes_locked();
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
     * existing file. */
    struct stat st;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t existing = 0;
    if (stat(path, &st) == 0) existing = (size_t)st.st_size;
    size_t custom = custom_bytes_locked();
    xSemaphoreGive(s_mutex);
    if (custom - existing + len > VOICE_STORE_BUDGET) {
        return VOICE_STORE_ERR_OVER_BUDGET;
    }

    /* Phase 2 (NO mutex): the tmp write can take seconds on SPIFFS under GC
     * (up to 480 KB); holding s_mutex here would stall stats/scan callers.
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
    existing = 0;
    if (stat(path, &st) == 0) existing = (size_t)st.st_size;
    custom = custom_bytes_locked();
    if (custom - existing + len > VOICE_STORE_BUDGET) {
        remove(tmp);
        xSemaphoreGive(s_mutex);
        return VOICE_STORE_ERR_OVER_BUDGET;
    }
    remove(path);   /* SPIFFS rename does not overwrite an existing target */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        xSemaphoreGive(s_mutex);
        return VOICE_STORE_ERR_IO;
    }

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
        audio_alert_set_override(i, NULL, 0);
    }
    xSemaphoreGive(s_mutex);
    return VOICE_STORE_OK;
}

bool voice_store_is_custom(const char *clip_name, size_t *size_out) {
    if (size_out) *size_out = 0;
    if (!s_ready) return false;
    if (clip_index(clip_name) < 0) return false;

    char path[VOICE_PATH_MAX];
    clip_path(path, sizeof(path), clip_name, ".pcm");

    struct stat st;
    bool exists = (stat(path, &st) == 0 && st.st_size > 0);
    if (exists && size_out) *size_out = (size_t)st.st_size;
    return exists;
}

void voice_store_wipe(void) {
    if (!s_ready) {
        /* Store not mounted yet (factory reset during the ~70 s first
         * format): defer; the mount task performs the wipe right after its
         * scan completes, so an early factory reset cannot leave custom
         * clips behind. */
        s_wipe_pending = true;
        return;
    }
    voice_store_reset_all();
}
