#pragma once

/**
 * @file nina_setup_hint.h
 * @brief First-boot setup hint overlay.
 *
 * After a fresh flash the device joins WiFi but the user has no way to learn
 * the address of the web config UI. This overlay covers the screen with the
 * STA IP as a browser-ready URL, the device hostname, and two buttons
 * ("Dismiss" for this boot, "Don't show again" to persist the choice).
 *
 * It is shown only while the device still looks factory-fresh: a STA IP
 * exists, the hostname is unset or still the factory "NINA-DISPLAY", and the
 * user has not chosen "Don't show again".
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the overlay when all show conditions hold, or refresh the URL label
 * (and re-raise the overlay) when it is already visible. Safe to call
 * repeatedly, before the display exists, and from any task.
 *
 * Takes the display lock internally, so the caller must NOT hold it.
 */
void nina_setup_hint_show_if_needed(void);

/**
 * Tear the overlay down and release the arbiter modal freeze, and suppress it
 * for the rest of this boot. A no-op when the overlay is not showing.
 *
 * Takes the display lock internally, so the caller must NOT hold it. Safe to
 * call from any task (the web config-save handler runs it on the httpd task).
 */
void nina_setup_hint_destroy(void);

#ifdef __cplusplus
}
#endif
