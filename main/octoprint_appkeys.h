#pragma once

/**
 * @file octoprint_appkeys.h
 * @brief OctoPrint Application Keys authorization flow -- one-click API key
 *        acquisition for the 3D Printer page.
 *
 * Replaces "go to OctoPrint, open Settings, Application Keys, generate a key,
 * copy it, come back, paste it" with a button. The device asks OctoPrint for a
 * key; the user approves the request in OctoPrint's own UI; the granted key
 * lands straight in config.
 *
 * Protocol (OctoPrint bundled "appkeys" plugin, verified against 1.11.8):
 *   GET  <base>/plugin/appkeys/probe                 204 = workflow supported
 *                                                    anything else = unsupported
 *   POST <base>/plugin/appkeys/request  {"app":...}  201 + {app_token, auth_dialog}
 *   GET  <base>/plugin/appkeys/request/<app_token>   202 pending (keep polling)
 *                                                    200 + {api_key} granted
 *                                                    404 denied, or expired
 *
 * NOTE ON THE READ-ONLY CONTRACT: octoprint_client.h states the printer client
 * issues GET only, and that still holds. The two POSTs here are authorization
 * requests against the appkeys plugin, not printer or job commands -- they
 * cannot start, pause, or cancel anything. That is the only reason they live in
 * this separate module rather than in octoprint_client.c.
 *
 * Threading: one flow at a time, run on a dedicated worker task. State is
 * published under a spinlock and readable from any task (the web handlers read
 * it from an httpd worker).
 */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/** Buffer sizes for octoprint_appkeys_snapshot() out-params. */
#define OCTO_APPKEY_DIALOG_LEN  256
#define OCTO_APPKEY_DETAIL_LEN  96

/**
 * Flow state. Everything from GRANTED down is terminal: the worker has stopped
 * and a new flow may be started.
 */
typedef enum {
    OCTO_APPKEY_IDLE = 0,     /**< no flow has run since boot, or none is live  */
    OCTO_APPKEY_PROBING,      /**< checking whether the server supports the flow */
    OCTO_APPKEY_WAITING,      /**< request placed; waiting for the user to approve */
    OCTO_APPKEY_GRANTED,      /**< key received and saved to config              */
    OCTO_APPKEY_DENIED,       /**< the user declined, or the request expired     */
    OCTO_APPKEY_UNSUPPORTED,  /**< probe says this server has no appkeys workflow */
    OCTO_APPKEY_TIMEOUT,      /**< nobody decided within the cap                 */
    OCTO_APPKEY_ERROR,        /**< misconfiguration or transport failure         */
} octoprint_appkey_state_t;

/**
 * Start a flow.
 *
 * Returns immediately -- the probe/request/poll work runs on the worker task,
 * so the caller (an httpd handler) never blocks on the network. Poll
 * octoprint_appkeys_snapshot() for progress.
 *
 * @return ESP_OK on start (including the "address not configured" case, which
 *         is reported through the ERROR state so the UI shows one message
 *         style); ESP_ERR_INVALID_STATE when a flow is already running;
 *         ESP_ERR_NO_MEM when the worker task could not be created.
 */
esp_err_t octoprint_appkeys_start(void);

/**
 * Read the current state. Any out-param may be NULL.
 *
 * @param auth_dialog  URL of OctoPrint's approval page; "" until the request
 *                     has been placed, and always "" on OctoPrint older than
 *                     1.8.0 (the field did not exist). Buffer should be
 *                     OCTO_APPKEY_DIALOG_LEN bytes.
 * @param detail       Short end-user explanation of the current state, "" when
 *                     there is nothing to add. Buffer should be
 *                     OCTO_APPKEY_DETAIL_LEN bytes. Never contains the key.
 */
void octoprint_appkeys_snapshot(octoprint_appkey_state_t *state,
                                char *auth_dialog, size_t dialog_len,
                                char *detail, size_t detail_len);

/** Stable lowercase wire name for @p state ("idle", "probing", "waiting", ...). */
const char *octoprint_appkeys_state_name(octoprint_appkey_state_t state);
