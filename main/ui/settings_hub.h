#pragma once
#include "lvgl.h"

/* Dashboard contract — same shape the tabview had (recon section 1).
 * Lazy modal lifecycle stays owned by nina_dashboard.c:
 * created on show, destroyed on hide, modal_open/close NOT called here. */
lv_obj_t *settings_hub_create(lv_obj_t *parent);
void settings_hub_destroy(void);
void settings_hub_refresh(void);       /* safe no-op when hub not open; called from main.c GOT_IP */
void settings_hub_apply_theme(void);   /* rebuilds current screen in new theme colors */

/* Internal navigation seam shared with settings_wifi.c.
 * Every screen builder creates its widgets into the given root (the hub's
 * full-size container) with the LVGL lock already held by the caller. */
typedef enum {
    HUB_SCREEN_HUB = 0,
    HUB_SCREEN_THEME,
    HUB_SCREEN_BRIGHTNESS,
    HUB_SCREEN_WIFI_HOME,
    HUB_SCREEN_WIFI_SCAN,
    HUB_SCREEN_WIFI_PASSWORD,
    HUB_SCREEN_WIFI_CONNECT,
    HUB_SCREEN_PAGES,
    HUB_SCREEN_MORE,
} hub_screen_t;

void settings_hub_goto(hub_screen_t screen);      /* destroy current, build target */
hub_screen_t settings_hub_current(void);
lv_obj_t *settings_hub_make_header(lv_obj_t *parent, const char *title);
                                                   /* 72 px header row with 96x72 BACK,
                                                      back target = hub (or exit, on hub) */

/* Implemented in settings_wifi.c, dispatched from settings_hub_goto(): */
void settings_wifi_build(lv_obj_t *root, hub_screen_t which);
/* Selected SSID/secured flag handoff between scan -> password -> connect: */
void settings_wifi_set_candidate(const char *ssid, bool secured);
