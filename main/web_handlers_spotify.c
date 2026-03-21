#include "web_server_internal.h"
#include "spotify_auth.h"
#include "spotify_client.h"
#include <string.h>

/**
 * @brief GET /api/spotify/config — return Spotify-related config fields.
 */
esp_err_t spotify_config_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "spotify_enabled", cfg->spotify_enabled);
    cJSON_AddStringToObject(root, "spotify_client_id", cfg->spotify_client_id);
    cJSON_AddNumberToObject(root, "spotify_poll_interval_ms", cfg->spotify_poll_interval_ms);
    cJSON_AddBoolToObject(root, "spotify_show_progress_bar", cfg->spotify_show_progress_bar);
    cJSON_AddBoolToObject(root, "spotify_minimal_mode", cfg->spotify_minimal_mode);
    cJSON_AddBoolToObject(root, "spotify_scroll_text", cfg->spotify_scroll_text);
    cJSON_AddNumberToObject(root, "spotify_overlay_timeout_s", cfg->spotify_overlay_timeout_s);
    cJSON_AddBoolToObject(root, "spotify_overlay_visible", cfg->spotify_overlay_visible);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief POST /api/spotify/config — update Spotify-related config fields and save to NVS.
 */
esp_err_t spotify_config_post_handler(httpd_req_t *req)
{
    char buf[CONFIG_MAX_PAYLOAD];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return send_400(req, "Empty request body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_400(req, "Invalid JSON");
    }

    /* Validate string lengths */
    if (!validate_string_len(root, "spotify_client_id", sizeof(((app_config_t *)0)->spotify_client_id))) {
        cJSON_Delete(root);
        return send_400(req, "spotify_client_id too long");
    }

    app_config_t *cfg = app_config_get();

    JSON_TO_BOOL(root, "spotify_enabled", cfg->spotify_enabled);
    JSON_TO_STRING(root, "spotify_client_id", cfg->spotify_client_id);
    JSON_TO_INT(root, "spotify_poll_interval_ms", cfg->spotify_poll_interval_ms);
    JSON_TO_BOOL(root, "spotify_show_progress_bar", cfg->spotify_show_progress_bar);
    JSON_TO_BOOL(root, "spotify_minimal_mode", cfg->spotify_minimal_mode);
    JSON_TO_BOOL(root, "spotify_scroll_text", cfg->spotify_scroll_text);
    JSON_TO_INT(root, "spotify_overlay_timeout_s", cfg->spotify_overlay_timeout_s);
    JSON_TO_BOOL(root, "spotify_overlay_visible", cfg->spotify_overlay_visible);

    cJSON_Delete(root);

    app_config_save(cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/spotify/callback — serves an inline HTML page that extracts
 *        the auth code from URL params, reads code_verifier from sessionStorage,
 *        and POSTs both to /api/spotify/token-exchange.
 */
esp_err_t spotify_callback_get_handler(httpd_req_t *req)
{
    static const char callback_html[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Spotify Login</title>"
        "<style>"
        "body{font-family:sans-serif;display:flex;justify-content:center;align-items:center;"
        "min-height:100vh;margin:0;background:#1a1a2e;color:#e0e0e0;}"
        ".card{background:#16213e;padding:2em;border-radius:12px;text-align:center;max-width:400px;}"
        ".ok{color:#1db954;} .err{color:#e74c3c;}"
        "</style></head><body>"
        "<div class=\"card\" id=\"msg\">Completing login...</div>"
        "<script>"
        "(async()=>{"
        "const el=document.getElementById('msg');"
        "try{"
        "const p=new URLSearchParams(window.location.search);"
        "const code=p.get('code');"
        "const err=p.get('error');"
        "if(err){el.innerHTML='<span class=\"err\">Login denied: '+err+'</span>';return;}"
        "if(!code){el.innerHTML='<span class=\"err\">No authorization code received.</span>';return;}"
        "const cv=sessionStorage.getItem('spotify_code_verifier');"
        "if(!cv){el.innerHTML='<span class=\"err\">Missing code verifier. Please retry login from the config page.</span>';return;}"
        "const ru=sessionStorage.getItem('spotify_redirect_uri')||window.location.origin+'/api/spotify/callback';"
        "const r=await fetch('/api/spotify/token-exchange',{"
        "method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({code:code,code_verifier:cv,redirect_uri:ru})"
        "});"
        "const j=await r.json();"
        "if(j.success){"
        "sessionStorage.removeItem('spotify_code_verifier');"
        "sessionStorage.removeItem('spotify_redirect_uri');"
        "el.innerHTML='<span class=\"ok\">Login successful!</span><br>Redirecting...';"
        "setTimeout(()=>window.location.href='/',2000);"
        "}else{"
        "el.innerHTML='<span class=\"err\">Login failed: '+(j.error||'unknown error')+'</span>';"
        "}"
        "}catch(e){el.innerHTML='<span class=\"err\">Error: '+e.message+'</span>';}"
        "})();"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, callback_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief POST /api/spotify/token-exchange — receives {code, code_verifier, redirect_uri},
 *        exchanges for tokens via spotify_auth_exchange_code().
 */
esp_err_t spotify_token_exchange_post_handler(httpd_req_t *req)
{
    char buf[CONFIG_MAX_PAYLOAD];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return send_400(req, "Empty request body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_400(req, "Invalid JSON");
    }

    cJSON *code_item = cJSON_GetObjectItem(root, "code");
    cJSON *verifier_item = cJSON_GetObjectItem(root, "code_verifier");
    cJSON *redirect_item = cJSON_GetObjectItem(root, "redirect_uri");

    if (!cJSON_IsString(code_item) || !cJSON_IsString(verifier_item) || !cJSON_IsString(redirect_item)) {
        cJSON_Delete(root);
        return send_400(req, "Missing required fields: code, code_verifier, redirect_uri");
    }

    esp_err_t err = spotify_auth_exchange_code(
        code_item->valuestring,
        verifier_item->valuestring,
        redirect_item->valuestring
    );
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"success\":false,\"error\":\"Token exchange failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/**
 * @brief POST /api/spotify/logout — clears stored tokens.
 */
esp_err_t spotify_logout_post_handler(httpd_req_t *req)
{
    spotify_auth_logout();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/spotify/status — returns auth state and current playback info.
 */
esp_err_t spotify_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Auth state */
    spotify_auth_state_t state = spotify_auth_get_state();
    const char *state_str = "none";
    if (state == SPOTIFY_AUTH_AUTHORIZED) state_str = "authorized";
    else if (state == SPOTIFY_AUTH_ERROR) state_str = "error";
    cJSON_AddStringToObject(root, "auth_state", state_str);

    /* Playback info */
    spotify_playback_t pb;
    spotify_client_get_cached_playback(&pb);
    cJSON_AddBoolToObject(root, "is_playing", pb.is_playing);
    cJSON_AddStringToObject(root, "track", pb.track_title);
    cJSON_AddStringToObject(root, "artist", pb.artist_name);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief POST /api/spotify/control — accepts {action: "play"/"pause"/"next"/"previous"},
 *        calls the corresponding spotify_client function.
 */
esp_err_t spotify_control_post_handler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return send_400(req, "Empty request body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_400(req, "Invalid JSON");
    }

    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_item)) {
        cJSON_Delete(root);
        return send_400(req, "Missing required field: action");
    }

    const char *action = action_item->valuestring;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (strcmp(action, "play") == 0) {
        err = spotify_client_play();
    } else if (strcmp(action, "pause") == 0) {
        err = spotify_client_pause();
    } else if (strcmp(action, "next") == 0) {
        err = spotify_client_next();
    } else if (strcmp(action, "previous") == 0) {
        err = spotify_client_previous();
    } else {
        cJSON_Delete(root);
        return send_400(req, "Invalid action. Use: play, pause, next, previous");
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"success\":false,\"error\":\"Spotify API call failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}
