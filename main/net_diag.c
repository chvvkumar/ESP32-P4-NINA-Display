/**
 * @file net_diag.c
 * @brief One-shot outage triage — ICMP-pings the WiFi gateway and the MQTT
 *        broker, then logs a single plain-language verdict line.
 *
 * The whole module is synchronous: net_diag_log_outage() runs entirely on the
 * caller's task and returns once both probes are done (worst case ~3.0 s: two
 * ICMP probes of 1 s timeout plus 0.5 s wait slack each). Name resolution adds
 * nothing to that bound — the broker host is either a numeric literal or goes
 * through the app-level cached resolver, never a raw getaddrinfo() whose lwIP
 * retries can stall for tens of seconds during the very outage being triaged.
 *
 * esp_ping spawns its own internal task per session; we wait on a statically
 * allocated binary semaphore signalled from the on_ping_end callback, so no
 * heap object and no stack pointer outlives the call.
 *
 * Concurrency: a spinlock protects both the 10 s repeat-suppression timestamp
 * and an s_busy flag held for the whole body. A second caller returns
 * immediately whether it arrives inside the 10 s window or while a slower run
 * is still in flight, so the module-static ping semaphore and reply flag are
 * only ever touched by one run at a time.
 *
 * Callback identity: esp_ping_delete_session() is asynchronous, so an abandoned
 * session's task can still fire callbacks into the next probe. Each probe takes
 * a fresh epoch, passes it by value through cb_args, and callbacks bearing a
 * stale epoch are discarded.
 */

#include "net_diag.h"
#include "app_config.h"
#include "nina_client_internal.h"   /* nina_client_resolve_host() */

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "net_diag";

/* Per-probe ICMP timeout. Two probes, each waited on for timeout + slack,
 * bound the whole call at ~3.0 s; nothing else here blocks. */
#define NET_DIAG_PING_TIMEOUT_MS   1000

/* Extra slack on top of the ICMP timeout when waiting for the session to end. */
#define NET_DIAG_WAIT_SLACK_MS     500

/* Ignore repeat calls made inside this window (misuse guard, not rate limiting). */
#define NET_DIAG_MIN_INTERVAL_MS   10000

/* Longest host name rendered into the log line. Kept as a literal precision so
 * -Wformat-truncation can bound every snprintf below at compile time. */
#define NET_DIAG_HOST_FMT          "%.63s"

/* Unix time for 2020-01-01; anything below means NTP has not synced yet. */
#define NET_DIAG_TIME_SYNC_EPOCH   ((time_t)1577836800)

/* Re-entry guard state. s_busy covers the whole body; s_last_run_ms/s_has_run
 * suppress repeat calls for NET_DIAG_MIN_INTERVAL_MS after one completes. */
static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static int64_t      s_last_run_ms = 0;
static bool         s_has_run = false;
static bool         s_busy = false;

/* Ping completion signalling. Statically allocated so nothing outlives a call. */
static StaticSemaphore_t s_ping_sem_buf;
static SemaphoreHandle_t s_ping_sem = NULL;
static volatile bool     s_ping_replied = false;

/* Identity of the probe currently being waited on. Written only from the
 * serialised body, read from the esp_ping callback task. */
static volatile uint32_t s_probe_epoch = 0;

/* Release the in-flight flag. Every exit path from net_diag_log_outage() that
 * claimed it must call this. */
static void net_diag_clear_busy(void) {
    taskENTER_CRITICAL(&s_guard);
    s_busy = false;
    taskEXIT_CRITICAL(&s_guard);
}

// =============================================================================
// ICMP probe — blocking single-shot wrapper around the esp_ping session API
// =============================================================================

/* cb_args carries the probe epoch by value (never a stack pointer). A callback
 * from a session we already abandoned reports a stale epoch and is dropped. */
static bool net_diag_cb_is_current(void *args) {
    return (uint32_t)(uintptr_t)args == s_probe_epoch;
}

static void net_diag_on_ping_success(esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    if (!net_diag_cb_is_current(args)) {
        return;
    }
    s_ping_replied = true;
}

static void net_diag_on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    (void)args;
}

static void net_diag_on_ping_end(esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    if (!net_diag_cb_is_current(args)) {
        return;
    }
    if (s_ping_sem) {
        xSemaphoreGive(s_ping_sem);
    }
}

/**
 * Send one ICMP echo request to a network-order IPv4 address and wait for the
 * session to finish. Returns true only if a reply came back.
 *
 * interval_ms is deliberately 0: with count == 1 the ping thread would
 * otherwise sleep one interval after the single echo before ending the session,
 * adding needless latency to a blocking caller.
 */
static bool net_diag_ping_ipv4(uint32_t addr_be) {
    if (!s_ping_sem) {
        s_ping_sem = xSemaphoreCreateBinaryStatic(&s_ping_sem_buf);
        if (!s_ping_sem) {
            ESP_LOGE(TAG, "failed to create ping semaphore");
            return false;
        }
    }

    /* New identity for this probe, claimed before anything else: from here on
     * no callback belonging to an earlier session can touch s_ping_replied or
     * the semaphore. Only ever incremented from the serialised body, so a plain
     * read-modify-write is sufficient. Starts at 1, which also makes a NULL
     * cb_args (epoch 0) impossible to mistake for a live probe. */
    uint32_t epoch = s_probe_epoch + 1;
    s_probe_epoch = epoch;

    /* Drop any give left by a previous session that ended after we stopped
     * waiting for it (possible only before the epoch bump above). */
    xSemaphoreTake(s_ping_sem, 0);
    s_ping_replied = false;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 1;
    cfg.interval_ms = 0;
    cfg.timeout_ms = NET_DIAG_PING_TIMEOUT_MS;
    ip_addr_set_ip4_u32(&cfg.target_addr, addr_be);

    esp_ping_callbacks_t cbs = {
        .cb_args = (void *)(uintptr_t)epoch,
        .on_ping_success = net_diag_on_ping_success,
        .on_ping_timeout = net_diag_on_ping_timeout,
        .on_ping_end = net_diag_on_ping_end,
    };

    esp_ping_handle_t hdl = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &hdl);
    if (err != ESP_OK || !hdl) {
        ESP_LOGW(TAG, "esp_ping_new_session failed: %s", esp_err_to_name(err));
        return false;
    }

    bool replied = false;
    if (esp_ping_start(hdl) == ESP_OK) {
        TickType_t wait = pdMS_TO_TICKS(NET_DIAG_PING_TIMEOUT_MS + NET_DIAG_WAIT_SLACK_MS);
        if (xSemaphoreTake(s_ping_sem, wait) == pdTRUE) {
            replied = s_ping_replied;
        } else {
            ESP_LOGW(TAG, "ping session did not signal completion in time");
            esp_ping_stop(hdl);
        }
    } else {
        ESP_LOGW(TAG, "esp_ping_start failed");
    }

    esp_ping_delete_session(hdl);
    return replied;
}

// =============================================================================
// Helpers — MQTT broker URL parsing and IPv4 resolution
// =============================================================================

/**
 * Extract the host portion of an MQTT broker URL such as
 * "mqtt://192.168.1.250", "mqtts://broker.lan:8883" or "broker.lan".
 *
 * Returns false when the URL is empty, has no host, or the host does not fit
 * host_out. IPv6 literals in brackets are not supported.
 */
static bool net_diag_parse_broker_host(const char *url, char *host_out, size_t host_len) {
    if (!url || !host_out || host_len == 0) {
        return false;
    }

    const char *p = strstr(url, "://");
    if (p) {
        p += 3;
    } else {
        p = url;
    }

    size_t n = 0;
    while (p[n] != '\0' && p[n] != ':' && p[n] != '/') {
        n++;
    }
    if (n == 0 || n >= host_len) {
        return false;
    }

    memcpy(host_out, p, n);
    host_out[n] = '\0';
    return true;
}

/**
 * Resolve a host name (or dotted-quad literal) to a network-order IPv4 address.
 *
 * Deliberately never calls getaddrinfo() directly: this runs while the network
 * is already failing, and an lwIP DNS query on a dead link retries for tens of
 * seconds, which would blow the documented bound on a blocking caller. A
 * numeric literal is parsed in place; anything else goes through the app-level
 * cached resolver (60 s TTL with stale-cache fallback), which answers instantly
 * for any host the poll path has already looked up.
 *
 * Returns false when the name cannot be turned into an address; the caller then
 * reports the broker as unresolvable instead of probing it.
 */
static bool net_diag_resolve_ipv4(const char *host, uint32_t *addr_out) {
    if (!host || !host[0] || !addr_out) {
        return false;
    }

    ip4_addr_t parsed;

    /* Numeric IPv4 literal — no resolver involved. */
    if (ip4addr_aton(host, &parsed)) {
        if (parsed.addr == 0) {
            return false;
        }
        *addr_out = parsed.addr;
        return true;
    }

    char ip_str[46];
    if (!nina_client_resolve_host(host, ip_str, sizeof(ip_str))) {
        return false;
    }
    if (!ip4addr_aton(ip_str, &parsed) || parsed.addr == 0) {
        return false;
    }

    *addr_out = parsed.addr;
    return true;
}

/**
 * Render the current wall-clock time, or "time-unsynced" when NTP has not
 * caught up yet. buf must be at least 20 bytes.
 */
static void net_diag_format_time(char *buf, size_t buf_len) {
    time_t now = time(NULL);
    struct tm tm_now;

    if (now >= NET_DIAG_TIME_SYNC_EPOCH && localtime_r(&now, &tm_now) != NULL) {
        if (strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &tm_now) > 0) {
            return;
        }
    }
    snprintf(buf, buf_len, "time-unsynced");
}

// =============================================================================
// Public API
// =============================================================================

void net_diag_log_outage(const char *failed_host) {
    const char *host_name = (failed_host && failed_host[0]) ? failed_host : "(unknown host)";

    /* Misuse guard: at most one triage run every NET_DIAG_MIN_INTERVAL_MS, and
     * never two at once even if a run outlives that window. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool suppress = false;
    taskENTER_CRITICAL(&s_guard);
    if (s_busy) {
        suppress = true;
    } else if (s_has_run && (now_ms - s_last_run_ms) < NET_DIAG_MIN_INTERVAL_MS) {
        suppress = true;
    } else {
        s_has_run = true;
        s_last_run_ms = now_ms;
        s_busy = true;
    }
    taskEXIT_CRITICAL(&s_guard);
    if (suppress) {
        return;
    }

    char timebuf[32];
    net_diag_format_time(timebuf, sizeof(timebuf));

    /* Step 1 — the device's own STA address and default gateway. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));

    if (!sta || esp_netif_get_ip_info(sta, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0 || ip_info.gw.addr == 0) {
        ESP_LOGW(TAG, "%s | " NET_DIAG_HOST_FMT " not answering. This device has no WiFi "
                      "address or gateway -> device network connection is down",
                 timebuf, host_name);
        net_diag_clear_busy();
        return;
    }

    /* Step 2 — ping the gateway. */
    bool gw_ok = net_diag_ping_ipv4(ip_info.gw.addr);

    /* Step 3 — ping the MQTT broker, when one is configured and parseable. */
    bool broker_checked = false;
    bool broker_ok = false;
    bool broker_unresolved = false;
    const app_config_t *cfg = app_config_get();

    if (cfg && cfg->mqtt_enabled) {
        char broker_host[128];
        uint32_t broker_addr = 0;
        if (net_diag_parse_broker_host(cfg->mqtt_broker_url, broker_host, sizeof(broker_host))) {
            if (net_diag_resolve_ipv4(broker_host, &broker_addr)) {
                broker_checked = true;
                broker_ok = net_diag_ping_ipv4(broker_addr);
            } else {
                /* No address, and we will not pay a blocking DNS retry to get
                 * one — say so in the verdict instead of probing. */
                broker_unresolved = true;
                ESP_LOGD(TAG, "broker host '%s' has no cached/literal address", broker_host);
            }
        } else {
            ESP_LOGD(TAG, "broker URL not parseable, skipping broker probe");
        }
    }

    /* Step 4 — one summary line. */
    const char *gw_txt = gw_ok ? "OK" : "FAIL";
    const char *broker_txt;
    if (broker_unresolved) {
        broker_txt = "unresolvable";
    } else if (!broker_checked) {
        broker_txt = "not checked";
    } else if (broker_ok) {
        broker_txt = "OK";
    } else {
        broker_txt = "FAIL";
    }

    /* Longest verdict is ~153 bytes with a full-length host field. */
    char verdict[192];
    if (!gw_ok && (!broker_checked || !broker_ok)) {
        snprintf(verdict, sizeof(verdict),
                 "device network connection is down");
    } else if (gw_ok && broker_checked && !broker_ok) {
        snprintf(verdict, sizeof(verdict),
                 "local network is up but the broker is down too -> outage is wider than "
                 NET_DIAG_HOST_FMT, host_name);
    } else if (!gw_ok && broker_checked && broker_ok) {
        snprintf(verdict, sizeof(verdict),
                 "gateway ignores ping but the broker replies -> problem is "
                 NET_DIAG_HOST_FMT " or path to it", host_name);
    } else {
        snprintf(verdict, sizeof(verdict),
                 "problem is " NET_DIAG_HOST_FMT " or path to it", host_name);
    }

    ESP_LOGW(TAG, "%s | " NET_DIAG_HOST_FMT " not answering. Gateway: %s, broker: %s -> %s",
             timebuf, host_name, gw_txt, broker_txt, verdict);

    net_diag_clear_busy();
}
