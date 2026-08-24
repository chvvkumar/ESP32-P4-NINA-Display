#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_JOIN_IDLE = 0,
    WIFI_JOIN_SCANNING,
    WIFI_JOIN_SCAN_DONE,
    WIFI_JOIN_SCAN_FAILED,
    WIFI_JOIN_CONNECTING,
    WIFI_JOIN_REJOINING,
    WIFI_JOIN_SUCCESS,
    WIFI_JOIN_FAIL_AUTH,      /* wrong password class */
    WIFI_JOIN_FAIL_NO_AP,     /* SSID not found */
    WIFI_JOIN_FAIL_TIMEOUT,   /* 20 s elapsed */
} wifi_join_state_t;

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secured;
} wifi_join_ap_t;

void wifi_join_init(void);                 /* mutex creation; called from app_main after wifi_init */
bool wifi_join_start_scan(void);           /* false if a scan/join is already running */
int  wifi_join_get_scan_results(wifi_join_ap_t *out, int max);  /* copy under mutex, returns count */
bool wifi_join_start_connect(const char *ssid, const char *password); /* password "" = open network */
void wifi_join_cancel(void);               /* abort connect; triggers rejoin of previous network */
wifi_join_state_t wifi_join_get_state(void);
int8_t wifi_join_success_rssi(void);       /* valid in WIFI_JOIN_SUCCESS */
bool wifi_join_active(void);               /* true in SCANNING/CONNECTING/REJOINING */
void wifi_join_ack_result(void);           /* UI consumed SUCCESS/FAIL_*: state -> IDLE */

/* Called ONLY from main.c's WiFi event handler: */
void wifi_join_note_disconnect(uint8_t reason);
void wifi_join_note_got_ip(void);

/* UI notification: invoked by the worker as
 *   lvgl_port_lock(0); lv_async_call(cb_trampoline, NULL); lvgl_port_unlock();
 * The registered cb runs later on the LVGL task and re-reads state via getters. */
void wifi_join_set_notify_cb(void (*cb)(void));
