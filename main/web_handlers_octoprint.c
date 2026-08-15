/**
 * @file web_handlers_octoprint.c
 * @brief HTTP handlers for the OctoPrint Application Keys flow.
 *
 * Two routes, both ROUTE_AUTH_REQUIRED (see web_server.c) and each also
 * REQUIRE_AUTH() first-line (defense-in-depth):
 *   POST /api/octoprint/appkey-request  -- start a flow, answer with the state
 *                                          it started in (never blocks on the
 *                                          network; the worker does that)
 *   GET  /api/octoprint/appkey-status   -- poll the flow's state
 *
 * The rest of the OctoPrint page's settings ride on the main /api/config
 * payload (see the octoprint_* rows in web_handlers_config.c), so there is no
 * octoprint-config endpoint pair here the way there is for JSON and HA.
 *
 * The granted key is NEVER returned by either route. It goes straight into
 * config, from where GET /api/config hands out the same "********" sentinel it
 * uses for every other stored secret.
 */

#include "web_server_internal.h"
#include "octoprint_appkeys.h"

/**
 * Build the response body shared by both routes: the state name plus the two
 * strings the UI needs to act on it.
 */
static esp_err_t send_appkey_state(httpd_req_t *req)
{
    octoprint_appkey_state_t state = OCTO_APPKEY_IDLE;
    char dialog[OCTO_APPKEY_DIALOG_LEN];
    char detail[OCTO_APPKEY_DETAIL_LEN];
    octoprint_appkeys_snapshot(&state, dialog, sizeof(dialog), detail, sizeof(detail));

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "status",      octoprint_appkeys_state_name(state));
    cJSON_AddStringToObject(root, "auth_dialog", dialog);
    cJSON_AddStringToObject(root, "detail",      detail);
    return send_json_response(req, root);
}

/**
 * @brief POST /api/octoprint/appkey-request -- ask OctoPrint for an API key.
 *
 * Returns as soon as the worker has been handed the job, so the single httpd
 * task is never parked on a five-minute approval wait. That means auth_dialog is
 * normally still empty in this response; the UI picks it up from the first
 * appkey-status poll a second or two later.
 *
 * A second request while one is running answers 409 with the live state rather
 * than starting a competing flow.
 */
esp_err_t octoprint_appkey_request_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    esp_err_t err = octoprint_appkeys_start();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    return send_appkey_state(req);
}

/**
 * @brief GET /api/octoprint/appkey-status -- current state of the flow.
 *
 * Terminal states persist until the next request is started, so a UI that
 * reloads mid-flow still sees how the last one ended.
 */
esp_err_t octoprint_appkey_status_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return send_appkey_state(req);
}
