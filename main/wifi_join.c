/**
 * @file wifi_join.c
 * @brief On-device WiFi scan + join + rejoin backend for the Panel Mode
 *        settings hub.
 *
 * One persistent parked worker task ("wifi_join", PSRAM stack) runs every
 * esp_wifi call — never the LVGL task. This module registers NO event
 * handlers: main.c's single event_handler stays the WiFi event authority and
 * forwards into wifi_join_note_disconnect()/wifi_join_note_got_ip() while
 * wifi_join_active() is true. The UI is told about state changes through a
 * callback posted onto the LVGL task with lv_async_call().
 */

#include "wifi_join.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"      /* lvgl_port_lock/unlock (esp_lvgl_port) */
#include "lvgl.h"             /* lv_async_call */
#include "app_config.h"
#include "wifi_manager.h"
#include "tasks.h"            /* psram_task_ensure */

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "wifi_join";

#define JOIN_SCAN_MAX_RECORDS   30
#define JOIN_SCAN_MAX_RESULTS   20
#define JOIN_CONNECT_TIMEOUT_US (20LL * 1000000LL)

/* Worker commands — written under s_join_mutex, consumed by the worker. */
typedef enum {
    JOIN_CMD_NONE = 0,
    JOIN_CMD_SCAN,
    JOIN_CMD_CONNECT,
} join_cmd_t;

static SemaphoreHandle_t s_join_mutex;                 /* params + scan results */
static TaskHandle_t      s_join_task;
static portMUX_TYPE      s_join_spawn_mux = portMUX_INITIALIZER_UNLOCKED;

/* State machine. int32_t, never a byte atomic (RISC-V subword clobber rule). */
static _Atomic int32_t s_state = WIFI_JOIN_IDLE;
static _Atomic bool    s_join_active = false;   /* gates main.c's event handler */
static _Atomic int32_t s_join_reason = 0;       /* last disconnect reason this attempt */
static _Atomic bool    s_join_got_ip = false;
static _Atomic bool    s_cancel_requested = false;

/* Under s_join_mutex: */
static join_cmd_t     s_cmd = JOIN_CMD_NONE;
static char           s_req_ssid[33];
static char           s_req_password[65];
static wifi_join_ap_t s_scan_results[JOIN_SCAN_MAX_RESULTS];
static int            s_scan_count = 0;

static int8_t s_success_rssi = 0;   /* written by worker before SUCCESS is posted */

/* Set and read on the LVGL task only (the worker just posts the trampoline). */
static void (*s_notify_cb)(void) = NULL;

/* ── UI notification ─────────────────────────────────────────────── */

static void join_notify_trampoline(void *unused)
{
    (void)unused;
    if (s_notify_cb) {
        s_notify_cb();
    }
}

/* Worker-side: hand the state change to the LVGL task. The callback runs
 * later on the LVGL task and re-reads state through the getters. */
static void join_notify_ui(void)
{
    lvgl_port_lock(0);
    lv_async_call(join_notify_trampoline, NULL);
    lvgl_port_unlock();
}

/* ── Scan ────────────────────────────────────────────────────────── */

static int join_compare_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;
    return ap_b->rssi - ap_a->rssi;
}

static void join_do_scan(void)
{
    s_join_active = true;
    s_join_reason = 0;
    s_state = WIFI_JOIN_SCANNING;
    join_notify_ui();

    /* Same active-scan shape the web handler runs today (web_handlers_wifi.c),
     * blocking on this worker — never on the LVGL task. */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    bool ok = false;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
    } else {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > JOIN_SCAN_MAX_RECORDS) {
            ap_count = JOIN_SCAN_MAX_RECORDS;
        }

        if (ap_count == 0) {
            xSemaphoreTake(s_join_mutex, portMAX_DELAY);
            s_scan_count = 0;
            xSemaphoreGive(s_join_mutex);
            ok = true;   /* an empty neighbourhood is still a successful scan */
        } else {
            wifi_ap_record_t *recs =
                heap_caps_malloc((size_t)ap_count * sizeof(*recs), MALLOC_CAP_SPIRAM);
            if (!recs) {
                esp_wifi_clear_ap_list();
                ESP_LOGE(TAG, "scan: out of memory for %u records", (unsigned)ap_count);
            } else {
                esp_wifi_scan_get_ap_records(&ap_count, recs);
                qsort(recs, ap_count, sizeof(*recs), join_compare_rssi_desc);

                /* Dedupe by SSID keeping the strongest, drop hidden/empty
                 * SSIDs, keep the top JOIN_SCAN_MAX_RESULTS. */
                xSemaphoreTake(s_join_mutex, portMAX_DELAY);
                s_scan_count = 0;
                for (int i = 0; i < (int)ap_count && s_scan_count < JOIN_SCAN_MAX_RESULTS; i++) {
                    const char *ssid = (const char *)recs[i].ssid;
                    if (ssid[0] == '\0') {
                        continue;
                    }
                    bool duplicate = false;
                    for (int j = 0; j < s_scan_count; j++) {
                        if (strcmp(s_scan_results[j].ssid, ssid) == 0) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (duplicate) {
                        continue;
                    }
                    strlcpy(s_scan_results[s_scan_count].ssid, ssid,
                            sizeof(s_scan_results[s_scan_count].ssid));
                    s_scan_results[s_scan_count].rssi = recs[i].rssi;
                    /* Enterprise/WAPI networks are listed but marked secured;
                     * joining them fails with FAIL_AUTH (accepted mislabel). */
                    s_scan_results[s_scan_count].secured =
                        (recs[i].authmode != WIFI_AUTH_OPEN);
                    s_scan_count++;
                }
                xSemaphoreGive(s_join_mutex);
                heap_caps_free(recs);
                ok = true;
            }
        }
    }

    s_join_active = false;
    if (atomic_exchange(&s_join_reason, 0) != 0) {
        /* A disconnect landed while the scan gated main.c's event handler;
         * re-arm the reconnect walk it would normally have started. */
        wifi_resume_auto_reconnect();
    }
    s_state = ok ? WIFI_JOIN_SCAN_DONE : WIFI_JOIN_SCAN_FAILED;
    join_notify_ui();
}

/* ── Connect ─────────────────────────────────────────────────────── */

/* Persist the joined network. Slot pick: existing SSID match first (password
 * update), else first empty slot. Synchronous save — a new network must
 * survive an immediate power cut. Returns the slot, or -1 if nothing was
 * saved. */
static int join_save_slot(const char *ssid, const char *password)
{
    app_config_t *snap = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!snap) {
        ESP_LOGE(TAG, "no PSRAM for config snapshot; '%s' not saved", ssid);
        return -1;
    }
    app_config_get_snapshot_into(snap);

    int slot = -1;
    for (int i = 0; i < 3; i++) {
        if (strcmp(snap->wifi_networks[i].ssid, ssid) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < 3; i++) {
            if (snap->wifi_networks[i].ssid[0] == '\0') {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        /* The UI disables ADD NETWORK when all slots are full, so this is not
         * reachable through the panel; refuse to overwrite anything. */
        ESP_LOGW(TAG, "no free WiFi slot for '%s'; credentials not saved", ssid);
        heap_caps_free(snap);
        return -1;
    }

    strlcpy(snap->wifi_networks[slot].ssid, ssid,
            sizeof(snap->wifi_networks[slot].ssid));
    strlcpy(snap->wifi_networks[slot].password, password,
            sizeof(snap->wifi_networks[slot].password));
    app_config_save(snap);
    heap_caps_free(snap);
    return slot;
}

static void join_do_connect(const char *ssid, const char *password)
{
    /* Rollback context. A live-link probe, not the WIFI_CONNECTED_BIT (that
     * bit is set once and never cleared). */
    int prev_slot = wifi_get_current_network_index();
    wifi_ap_record_t probe;
    bool had_link = (esp_wifi_sta_get_ap_info(&probe) == ESP_OK);

    /* From this moment main.c's STA_DISCONNECTED handler routes reasons here
     * and arms nothing. */
    s_join_active = true;
    wifi_suspend_auto_reconnect();

    s_cancel_requested = false;
    s_join_reason = 0;
    s_join_got_ip = false;
    s_state = WIFI_JOIN_CONNECTING;
    join_notify_ui();

    if (had_link) {
        esp_wifi_disconnect();
        for (int i = 0; i < 20 && atomic_load(&s_join_reason) == 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_join_reason = 0;

    /* wifi_config_t built exactly like main.c's wifi_connect_to_slot(). */
    wifi_config_t sta_cfg = {0};
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.rssi = -90;
    if (password[0] != '\0') {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();
    ESP_LOGI(TAG, "joining '%s' (prev slot %d, had_link=%d)", ssid, prev_slot, (int)had_link);

    /* Wait loop: 100 ms polls against a 20 s deadline. */
    int64_t deadline = esp_timer_get_time() + JOIN_CONNECT_TIMEOUT_US;
    wifi_join_state_t outcome = WIFI_JOIN_FAIL_TIMEOUT;
    bool cancelled = false;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (atomic_load(&s_cancel_requested)) {
            cancelled = true;
            break;
        }
        if (atomic_load(&s_join_got_ip)) {
            /* GOT_IP alone is not proof: a stale reconnect attempt to the OLD
             * network can complete during our join. Only accept the IP if the
             * live association is the SSID this join requested. */
            wifi_ap_record_t got;
            if (esp_wifi_sta_get_ap_info(&got) == ESP_OK &&
                strcmp((char *)got.ssid, ssid) == 0) {
                outcome = WIFI_JOIN_SUCCESS;
                break;
            }
            /* Spurious event — clear and keep waiting until deadline. */
            s_join_got_ip = false;
            continue;
        }
        int32_t reason = atomic_exchange(&s_join_reason, 0);
        if (reason == 0) {
            continue;
        }
        if (reason == WIFI_REASON_AUTH_FAIL ||
            reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_MIC_FAILURE ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            outcome = WIFI_JOIN_FAIL_AUTH;
            break;
        }
        if (reason == WIFI_REASON_NO_AP_FOUND) {
            outcome = WIFI_JOIN_FAIL_NO_AP;
            break;
        }
        /* Transient reason — one retry per disconnect note until deadline. */
        esp_wifi_connect();
    }

    if (outcome == WIFI_JOIN_SUCCESS && !cancelled) {
        int slot = join_save_slot(ssid, password);
        if (slot >= 0) {
            wifi_manager_adopt_slot(slot);
        }
        wifi_ap_record_t ap;
        s_success_rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        ESP_LOGI(TAG, "joined '%s' (%d dBm), saved to slot %d", ssid, (int)s_success_rssi, slot);
        s_join_active = false;
        s_state = WIFI_JOIN_SUCCESS;
        join_notify_ui();
        return;
    }

    /* Failure or cancel: re-target the previous network before posting the
     * terminal state. The normal reconnect machinery finishes the rejoin. */
    s_state = WIFI_JOIN_REJOINING;
    join_notify_ui();

    if (cancelled || outcome == WIFI_JOIN_FAIL_TIMEOUT) {
        /* Abort the still-in-flight attempt; the resulting disconnect event is
         * swallowed by the join gate (still active). */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        s_join_reason = 0;
    }

    const app_config_t *cfg = app_config_get();
    bool have_prev = (prev_slot >= 0 && prev_slot < 3 &&
                      cfg->wifi_networks[prev_slot].ssid[0] != '\0');
    if (have_prev) {
        ESP_LOGI(TAG, "join of '%s' failed (%d) — rejoining slot %d", ssid, (int)outcome, prev_slot);
        wifi_connect_to_slot(prev_slot);
    } else {
        ESP_LOGI(TAG, "join of '%s' failed (%d) — resuming reconnect walk", ssid, (int)outcome);
        wifi_resume_auto_reconnect();
    }

    s_join_active = false;
    s_state = cancelled ? WIFI_JOIN_IDLE : outcome;
    join_notify_ui();
}

/* ── Worker task ─────────────────────────────────────────────────── */

/* Parked persistent worker (psram_task_spawn stacks are never freed, so the
 * task must never delete itself — it waits for the next command instead). */
static void wifi_join_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        join_cmd_t cmd;
        char ssid[sizeof(s_req_ssid)];
        char password[sizeof(s_req_password)];
        xSemaphoreTake(s_join_mutex, portMAX_DELAY);
        cmd = s_cmd;
        s_cmd = JOIN_CMD_NONE;
        strlcpy(ssid, s_req_ssid, sizeof(ssid));
        strlcpy(password, s_req_password, sizeof(password));
        xSemaphoreGive(s_join_mutex);

        switch (cmd) {
            case JOIN_CMD_SCAN:
                join_do_scan();
                break;
            case JOIN_CMD_CONNECT:
                join_do_connect(ssid, password);
                break;
            default:
                break;
        }
    }
}

static bool join_ensure_worker(void)
{
    return psram_task_ensure(&s_join_task, &s_join_spawn_mux,
                             wifi_join_worker, "wifi_join", 6144, NULL, 3, 0) != NULL;
}

/* True while a scan or join is in flight — new commands are refused. */
static bool join_busy(void)
{
    int32_t st = atomic_load(&s_state);
    return (st == WIFI_JOIN_SCANNING ||
            st == WIFI_JOIN_CONNECTING ||
            st == WIFI_JOIN_REJOINING);
}

/* ── Public API ──────────────────────────────────────────────────── */

void wifi_join_init(void)
{
    if (!s_join_mutex) {
        s_join_mutex = xSemaphoreCreateMutex();
    }
}

bool wifi_join_start_scan(void)
{
    if (!s_join_mutex || join_busy()) {
        return false;
    }
    if (!join_ensure_worker()) {
        return false;
    }
    xSemaphoreTake(s_join_mutex, portMAX_DELAY);
    s_cmd = JOIN_CMD_SCAN;
    xSemaphoreGive(s_join_mutex);
    /* Stamp the state before the worker runs so an immediate UI read after
     * this call already shows the scan in progress. */
    s_state = WIFI_JOIN_SCANNING;
    xTaskNotifyGive(s_join_task);
    return true;
}

int wifi_join_get_scan_results(wifi_join_ap_t *out, int max)
{
    if (!s_join_mutex || !out || max <= 0) {
        return 0;
    }
    xSemaphoreTake(s_join_mutex, portMAX_DELAY);
    int n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan_results, (size_t)n * sizeof(*out));
    xSemaphoreGive(s_join_mutex);
    return n;
}

bool wifi_join_start_connect(const char *ssid, const char *password)
{
    if (!s_join_mutex || !ssid || ssid[0] == '\0' || join_busy()) {
        return false;
    }
    if (!join_ensure_worker()) {
        return false;
    }
    xSemaphoreTake(s_join_mutex, portMAX_DELAY);
    s_cmd = JOIN_CMD_CONNECT;
    strlcpy(s_req_ssid, ssid, sizeof(s_req_ssid));
    strlcpy(s_req_password, password ? password : "", sizeof(s_req_password));
    xSemaphoreGive(s_join_mutex);
    /* Stamp the state before the worker runs (same reason as start_scan). */
    s_state = WIFI_JOIN_CONNECTING;
    xTaskNotifyGive(s_join_task);
    return true;
}

void wifi_join_cancel(void)
{
    s_cancel_requested = true;
}

wifi_join_state_t wifi_join_get_state(void)
{
    return (wifi_join_state_t)atomic_load(&s_state);
}

int8_t wifi_join_success_rssi(void)
{
    return s_success_rssi;
}

bool wifi_join_active(void)
{
    return atomic_load(&s_join_active);
}

void wifi_join_ack_result(void)
{
    int32_t st = atomic_load(&s_state);
    if (st == WIFI_JOIN_SUCCESS ||
        st == WIFI_JOIN_FAIL_AUTH ||
        st == WIFI_JOIN_FAIL_NO_AP ||
        st == WIFI_JOIN_FAIL_TIMEOUT) {
        s_state = WIFI_JOIN_IDLE;
    }
}

void wifi_join_note_disconnect(uint8_t reason)
{
    s_join_reason = (int32_t)reason;
    s_join_got_ip = false;
}

void wifi_join_note_got_ip(void)
{
    s_join_got_ip = true;
}

void wifi_join_set_notify_cb(void (*cb)(void))
{
    s_notify_cb = cb;
}
