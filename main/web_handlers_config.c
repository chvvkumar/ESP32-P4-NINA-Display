#include "web_server_internal.h"
#include "mqtt_ha.h"
#include "http_fetch.h"                 /* http_fetch_text — /api/config/pull proxy */
#include <string.h>
#include <strings.h>                    /* strncasecmp */
#include <time.h>
#include "esp_heap_caps.h"
#include "build_version.h"
#include "esp_app_desc.h" /* esp_app_get_elf_sha256 — per-image ETag source */
#include "esp_mac.h"
#include "esp_timer.h"
#include "ui/nina_setup_screen.h"
#include "ui/nina_setup_hint.h"         /* nina_setup_hint_destroy — retire hint on first save */
#include "ui/nina_dashboard.h"          /* nina_dashboard_get_total_page_count() */
#include "ui/nina_dashboard_internal.h" /* PAGE_IDX_SUMMARY, SETTINGS_PAGE_IDX, SYSINFO_PAGE_IDX page-index macros */
#include "ui/nina_nav_arbiter.h"        /* nav_arbiter_submit_user — live Home Page USER claim */
#include "ui/page_registry.h"           /* page_ref_navigate, PAGE_REF_ID_MAX, PAGE_REF_SETTINGS */
#include "settings_table.h"             /* settings_json_serialize/parse — table-driven config JSON */

extern const uint8_t config_html_start[] asm("_binary_config_ui_html_start");
extern const uint8_t config_html_end[]   asm("_binary_config_ui_html_end");
extern const uint8_t home_html_start[] asm("_binary_home_ui_html_start");
extern const uint8_t home_html_end[]   asm("_binary_home_ui_html_end");
extern const uint8_t setup_html_start[] asm("_binary_setup_ui_html_start");
extern const uint8_t setup_html_end[]   asm("_binary_setup_ui_html_end");
extern const uint8_t favicon_png_start[] asm("_binary_favicon_png_start");
extern const uint8_t favicon_png_end[]   asm("_binary_favicon_png_end");

/* P6b tab fragments -- see s_ui_fragments[] below for the lookup table these feed. */
extern const uint8_t fragment_logs_html_start[] asm("_binary_fragment_logs_html_start");
extern const uint8_t fragment_logs_html_end[]   asm("_binary_fragment_logs_html_end");
extern const uint8_t fragment_backup_html_start[] asm("_binary_fragment_backup_html_start");
extern const uint8_t fragment_backup_html_end[]   asm("_binary_fragment_backup_html_end");
extern const uint8_t fragment_api_html_start[] asm("_binary_fragment_api_html_start");
extern const uint8_t fragment_api_html_end[]   asm("_binary_fragment_api_html_end");

/* P6c tab fragments. The four image tabs (image-goes, image-moon,
 * image-solar, image-custom) have hyphenated tab names; the embedded asset
 * filenames/symbols use underscores (fragment_image_goes.html and so on),
 * so the name->symbol mapping only lines up in the s_ui_fragments[] rows
 * below, not by naming convention alone. */
extern const uint8_t fragment_allsky_html_start[] asm("_binary_fragment_allsky_html_start");
extern const uint8_t fragment_allsky_html_end[]   asm("_binary_fragment_allsky_html_end");
extern const uint8_t fragment_json_html_start[] asm("_binary_fragment_json_html_start");
extern const uint8_t fragment_json_html_end[]   asm("_binary_fragment_json_html_end");
extern const uint8_t fragment_ha_html_start[] asm("_binary_fragment_ha_html_start");
extern const uint8_t fragment_ha_html_end[]   asm("_binary_fragment_ha_html_end");
extern const uint8_t fragment_clock_html_start[] asm("_binary_fragment_clock_html_start");
extern const uint8_t fragment_clock_html_end[]   asm("_binary_fragment_clock_html_end");
extern const uint8_t fragment_spotify_html_start[] asm("_binary_fragment_spotify_html_start");
extern const uint8_t fragment_spotify_html_end[]   asm("_binary_fragment_spotify_html_end");
extern const uint8_t fragment_image_goes_html_start[]   asm("_binary_fragment_image_goes_html_start");
extern const uint8_t fragment_image_goes_html_end[]     asm("_binary_fragment_image_goes_html_end");
extern const uint8_t fragment_image_moon_html_start[]   asm("_binary_fragment_image_moon_html_start");
extern const uint8_t fragment_image_moon_html_end[]     asm("_binary_fragment_image_moon_html_end");
extern const uint8_t fragment_image_solar_html_start[]  asm("_binary_fragment_image_solar_html_start");
extern const uint8_t fragment_image_solar_html_end[]    asm("_binary_fragment_image_solar_html_end");
extern const uint8_t fragment_image_custom_html_start[] asm("_binary_fragment_image_custom_html_start");
extern const uint8_t fragment_image_custom_html_end[]   asm("_binary_fragment_image_custom_html_end");
extern const uint8_t fragment_image_radar_html_start[]  asm("_binary_fragment_image_radar_html_start");
extern const uint8_t fragment_image_radar_html_end[]    asm("_binary_fragment_image_radar_html_end");
extern const uint8_t fragment_octoprint_html_start[] asm("_binary_fragment_octoprint_html_start");
extern const uint8_t fragment_octoprint_html_end[]   asm("_binary_fragment_octoprint_html_end");

/* P6d tab fragments -- final wave. Extracts the remaining four tabs (nodes,
 * display, behavior, system), completing the migration: all 11 tabs now ship
 * as lazily-fetched fragments and config_ui.html holds only the tab shell,
 * loader machinery, and the shared JS (see config_ui.html's Tab Fragment
 * Loader section for LAZY_TABS/loadedTabs). */
extern const uint8_t fragment_nodes_html_start[] asm("_binary_fragment_nodes_html_start");
extern const uint8_t fragment_nodes_html_end[]   asm("_binary_fragment_nodes_html_end");
extern const uint8_t fragment_display_html_start[] asm("_binary_fragment_display_html_start");
extern const uint8_t fragment_display_html_end[]   asm("_binary_fragment_display_html_end");
extern const uint8_t fragment_behavior_html_start[] asm("_binary_fragment_behavior_html_start");
extern const uint8_t fragment_behavior_html_end[]   asm("_binary_fragment_behavior_html_end");
extern const uint8_t fragment_system_html_start[] asm("_binary_fragment_system_html_start");
extern const uint8_t fragment_system_html_end[]   asm("_binary_fragment_system_html_end");

/* Voice Clips tab (custom clip overrides). Like the image tabs, the tab name is
 * hyphenated ("voice-clips") while the asset symbol uses underscores. */
extern const uint8_t fragment_voice_clips_html_start[] asm("_binary_fragment_voice_clips_html_start");
extern const uint8_t fragment_voice_clips_html_end[]   asm("_binary_fragment_voice_clips_html_end");

/* Build-time gzip copies of the shell and every fragment (see WEB_GZ_ASSETS in
 * main/CMakeLists.txt). Symbol names follow the embed convention: the .gz file
 * basename with every non-identifier character mapped to '_'. */
extern const uint8_t config_html_gz_start[] asm("_binary_config_ui_html_gz_start");
extern const uint8_t config_html_gz_end[]   asm("_binary_config_ui_html_gz_end");
extern const uint8_t home_html_gz_start[] asm("_binary_home_ui_html_gz_start");
extern const uint8_t home_html_gz_end[]   asm("_binary_home_ui_html_gz_end");
extern const uint8_t fragment_logs_html_gz_start[] asm("_binary_fragment_logs_html_gz_start");
extern const uint8_t fragment_logs_html_gz_end[]   asm("_binary_fragment_logs_html_gz_end");
extern const uint8_t fragment_backup_html_gz_start[] asm("_binary_fragment_backup_html_gz_start");
extern const uint8_t fragment_backup_html_gz_end[]   asm("_binary_fragment_backup_html_gz_end");
extern const uint8_t fragment_api_html_gz_start[] asm("_binary_fragment_api_html_gz_start");
extern const uint8_t fragment_api_html_gz_end[]   asm("_binary_fragment_api_html_gz_end");
extern const uint8_t fragment_allsky_html_gz_start[] asm("_binary_fragment_allsky_html_gz_start");
extern const uint8_t fragment_allsky_html_gz_end[]   asm("_binary_fragment_allsky_html_gz_end");
extern const uint8_t fragment_json_html_gz_start[] asm("_binary_fragment_json_html_gz_start");
extern const uint8_t fragment_json_html_gz_end[]   asm("_binary_fragment_json_html_gz_end");
extern const uint8_t fragment_ha_html_gz_start[] asm("_binary_fragment_ha_html_gz_start");
extern const uint8_t fragment_ha_html_gz_end[]   asm("_binary_fragment_ha_html_gz_end");
extern const uint8_t fragment_clock_html_gz_start[] asm("_binary_fragment_clock_html_gz_start");
extern const uint8_t fragment_clock_html_gz_end[]   asm("_binary_fragment_clock_html_gz_end");
extern const uint8_t fragment_spotify_html_gz_start[] asm("_binary_fragment_spotify_html_gz_start");
extern const uint8_t fragment_spotify_html_gz_end[]   asm("_binary_fragment_spotify_html_gz_end");
extern const uint8_t fragment_image_goes_html_gz_start[]   asm("_binary_fragment_image_goes_html_gz_start");
extern const uint8_t fragment_image_goes_html_gz_end[]     asm("_binary_fragment_image_goes_html_gz_end");
extern const uint8_t fragment_image_moon_html_gz_start[]   asm("_binary_fragment_image_moon_html_gz_start");
extern const uint8_t fragment_image_moon_html_gz_end[]     asm("_binary_fragment_image_moon_html_gz_end");
extern const uint8_t fragment_image_solar_html_gz_start[]  asm("_binary_fragment_image_solar_html_gz_start");
extern const uint8_t fragment_image_solar_html_gz_end[]    asm("_binary_fragment_image_solar_html_gz_end");
extern const uint8_t fragment_image_custom_html_gz_start[] asm("_binary_fragment_image_custom_html_gz_start");
extern const uint8_t fragment_image_custom_html_gz_end[]   asm("_binary_fragment_image_custom_html_gz_end");
extern const uint8_t fragment_image_radar_html_gz_start[]  asm("_binary_fragment_image_radar_html_gz_start");
extern const uint8_t fragment_image_radar_html_gz_end[]    asm("_binary_fragment_image_radar_html_gz_end");
extern const uint8_t fragment_octoprint_html_gz_start[] asm("_binary_fragment_octoprint_html_gz_start");
extern const uint8_t fragment_octoprint_html_gz_end[]   asm("_binary_fragment_octoprint_html_gz_end");
extern const uint8_t fragment_nodes_html_gz_start[] asm("_binary_fragment_nodes_html_gz_start");
extern const uint8_t fragment_nodes_html_gz_end[]   asm("_binary_fragment_nodes_html_gz_end");
extern const uint8_t fragment_display_html_gz_start[] asm("_binary_fragment_display_html_gz_start");
extern const uint8_t fragment_display_html_gz_end[]   asm("_binary_fragment_display_html_gz_end");
extern const uint8_t fragment_behavior_html_gz_start[] asm("_binary_fragment_behavior_html_gz_start");
extern const uint8_t fragment_behavior_html_gz_end[]   asm("_binary_fragment_behavior_html_gz_end");
extern const uint8_t fragment_voice_clips_html_gz_start[] asm("_binary_fragment_voice_clips_html_gz_start");
extern const uint8_t fragment_voice_clips_html_gz_end[]   asm("_binary_fragment_voice_clips_html_gz_end");
extern const uint8_t fragment_system_html_gz_start[] asm("_binary_fragment_system_html_gz_start");
extern const uint8_t fragment_system_html_gz_end[]   asm("_binary_fragment_system_html_gz_end");

/* Every embedded asset changes exactly when the firmware image does, so one
 * ETag covers the shell and all fragments.
 *
 * The tag is derived from the running app's ELF SHA-256, not from
 * BUILD_GIT_SHA: the maintainer's normal workflow rebuilds and reflashes from
 * a dirty working tree, where the git SHA is unchanged across builds. That
 * made a modified UI serve a 304 and leave the browser on the stale shell.
 * The ELF hash changes with the image itself, so a reflash always invalidates.
 *
 * Built once on first use into a static buffer. esp_http_server dispatches
 * handlers from a single task, so the lazy init cannot race; the buffer is
 * static because httpd_resp_set_hdr() stores the pointer rather than copying,
 * so the value has to outlive the response.
 */
static const char *asset_etag(void)
{
    static char s_etag[24]; /* '"' + up to 16 hex chars + '"' + NUL */

    if (s_etag[0] == '\0') {
        /* esp_app_get_elf_sha256() writes a null-terminated hex string
         * truncated to the buffer size, and returns the bytes written. */
        char sha[17] = {0};
        esp_app_get_elf_sha256(sha, sizeof(sha));
        snprintf(s_etag, sizeof(s_etag), "\"%s\"", sha);
    }
    return s_etag;
}

/*
 * Serve one embedded UI asset with revalidation and optional gzip.
 *
 * Cache-Control is "no-cache" (revalidate every time, never serve stale UI
 * after an OTA) paired with an ETag, so the steady-state cost of a page load
 * is a 304 with an empty body instead of a fresh transfer.
 *
 * gz_start/gz_end may be NULL to force the plain copy.
 */
static esp_err_t send_embedded_asset(httpd_req_t *req,
                                     const uint8_t *plain_start, const uint8_t *plain_end,
                                     const uint8_t *gz_start, const uint8_t *gz_end,
                                     const char *content_type)
{
    const char *etag = asset_etag();

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");

    /* If-None-Match may carry a list and/or a W/ weak prefix; substring match
     * on the quoted tag accepts both without parsing the list. */
    char inm[80];
    esp_err_t inm_err = httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm));
    if ((inm_err == ESP_OK || inm_err == ESP_ERR_HTTPD_RESULT_TRUNC) &&
        strstr(inm, etag) != NULL) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    if (gz_start != NULL && gz_end > gz_start) {
        /* On ESP_ERR_HTTPD_RESULT_TRUNC the buffer still holds a
         * null-terminated prefix (strlcpy), so searching it is safe. Clients
         * that bury "gzip" past 96 chars simply get the plain copy. */
        char accept_enc[96];
        esp_err_t enc_err = httpd_req_get_hdr_value_str(req, "Accept-Encoding",
                                                        accept_enc, sizeof(accept_enc));
        if ((enc_err == ESP_OK || enc_err == ESP_ERR_HTTPD_RESULT_TRUNC) &&
            strstr(accept_enc, "gzip") != NULL) {
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            return httpd_resp_send(req, (const char *)gz_start, gz_end - gz_start);
        }
    }

    return httpd_resp_send(req, (const char *)plain_start, plain_end - plain_start);
}

// Handler for the config UI (served at /config; "/" is the Home page below)
esp_err_t root_get_handler(httpd_req_t *req)
{
    if (is_setup_mode()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, (const char *)setup_html_start,
                        setup_html_end - setup_html_start);
        return ESP_OK;
    }
    REQUIRE_AUTH(req);
    return send_embedded_asset(req, config_html_start, config_html_end,
                               config_html_gz_start, config_html_gz_end,
                               "text/html");
}

// Handler for the Home page at "/". Mirrors root_get_handler's flow exactly:
// the first-run setup screen wins while no WiFi is configured, and
// unauthenticated browsers are redirected into the login flow.
esp_err_t home_get_handler(httpd_req_t *req)
{
    if (is_setup_mode()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, (const char *)setup_html_start,
                        setup_html_end - setup_html_start);
        return ESP_OK;
    }
    REQUIRE_AUTH(req);
    return send_embedded_asset(req, home_html_start, home_html_end,
                               home_html_gz_start, home_html_gz_end, "text/html");
}

/**
 * @brief One row of the tab-fragment lookup table: a `tab` query value maps
 * to an embedded HTML asset's start/end pointers (same EMBED_TXTFILES
 * pattern as config_html_start/end above).
 */
typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    const uint8_t *gz_start;
    const uint8_t *gz_end;
} ui_fragment_entry_t;

/*
 * Fragment table for GET /ui/fragment?tab=<name> -- serves one config_ui.html
 * tab's markup as a standalone HTML asset, fetched lazily by the browser on
 * tab activation (see ensureTabLoaded() in config_ui.html). P6a landed only
 * the loader machinery (table empty, every lookup 404s). P6b extracted logs,
 * backup, and api. P6c extracted allsky, clock, spotify, and the image tabs.
 * P6d (final wave) extracts the remaining four -- nodes, display, behavior,
 * system -- so every tab now ships as its own fragment_NAME.html and none
 * remain inline in config_ui.html. The four image rows are the
 * name/symbol mismatch: the tab names have a hyphen, but embedded-asset
 * symbols cannot contain one, so the files are fragment_image_goes.html
 * and siblings, and only this table maps the hyphenated tab names to the
 * underscored symbols.
 */
static const ui_fragment_entry_t s_ui_fragments[] = {
    { "__none__", NULL, NULL, NULL, NULL },  /* placeholder so the array type-checks; never matches a real tab name */
    { "logs",   fragment_logs_html_start,   fragment_logs_html_end,
                fragment_logs_html_gz_start,   fragment_logs_html_gz_end },
    { "backup", fragment_backup_html_start, fragment_backup_html_end,
                fragment_backup_html_gz_start, fragment_backup_html_gz_end },
    { "api",    fragment_api_html_start,    fragment_api_html_end,
                fragment_api_html_gz_start,    fragment_api_html_gz_end },
    { "allsky",        fragment_allsky_html_start,        fragment_allsky_html_end,
                       fragment_allsky_html_gz_start,        fragment_allsky_html_gz_end },
    { "json",          fragment_json_html_start,          fragment_json_html_end,
                       fragment_json_html_gz_start,          fragment_json_html_gz_end },
    { "ha",            fragment_ha_html_start,            fragment_ha_html_end,
                       fragment_ha_html_gz_start,            fragment_ha_html_gz_end },
    { "clock",         fragment_clock_html_start,         fragment_clock_html_end,
                       fragment_clock_html_gz_start,         fragment_clock_html_gz_end },
    { "spotify",       fragment_spotify_html_start,       fragment_spotify_html_end,
                       fragment_spotify_html_gz_start,       fragment_spotify_html_gz_end },
    { "image-goes",    fragment_image_goes_html_start,    fragment_image_goes_html_end,
                       fragment_image_goes_html_gz_start,    fragment_image_goes_html_gz_end },
    { "image-moon",    fragment_image_moon_html_start,    fragment_image_moon_html_end,
                       fragment_image_moon_html_gz_start,    fragment_image_moon_html_gz_end },
    { "image-solar",   fragment_image_solar_html_start,   fragment_image_solar_html_end,
                       fragment_image_solar_html_gz_start,   fragment_image_solar_html_gz_end },
    { "image-custom",  fragment_image_custom_html_start,  fragment_image_custom_html_end,
                       fragment_image_custom_html_gz_start,  fragment_image_custom_html_gz_end },
    { "image-radar",   fragment_image_radar_html_start,   fragment_image_radar_html_end,
                       fragment_image_radar_html_gz_start,   fragment_image_radar_html_gz_end },
    { "octoprint",     fragment_octoprint_html_start,     fragment_octoprint_html_end,
                       fragment_octoprint_html_gz_start,     fragment_octoprint_html_gz_end },
    { "nodes",         fragment_nodes_html_start,         fragment_nodes_html_end,
                       fragment_nodes_html_gz_start,         fragment_nodes_html_gz_end },
    { "display",       fragment_display_html_start,       fragment_display_html_end,
                       fragment_display_html_gz_start,       fragment_display_html_gz_end },
    { "behavior",      fragment_behavior_html_start,      fragment_behavior_html_end,
                       fragment_behavior_html_gz_start,      fragment_behavior_html_gz_end },
    { "system",        fragment_system_html_start,        fragment_system_html_end,
                       fragment_system_html_gz_start,        fragment_system_html_gz_end },
    { "voice-clips",   fragment_voice_clips_html_start,   fragment_voice_clips_html_end,
                       fragment_voice_clips_html_gz_start,   fragment_voice_clips_html_gz_end },
};

// Handler for GET /ui/fragment?tab=<name> -- serves one lazily-loaded config_ui.html tab fragment.
esp_err_t ui_fragment_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char qbuf[64] = {0};
    char tab[32] = {0};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "tab", tab, sizeof(tab));
    }

    for (size_t i = 0; i < sizeof(s_ui_fragments) / sizeof(s_ui_fragments[0]); i++) {
        if (s_ui_fragments[i].start != NULL && strcmp(s_ui_fragments[i].name, tab) == 0) {
            return send_embedded_asset(req,
                                       s_ui_fragments[i].start, s_ui_fragments[i].end,
                                       s_ui_fragments[i].gz_start, s_ui_fragments[i].gz_end,
                                       "text/html");
        }
    }

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "unknown fragment", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for favicon
esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    httpd_resp_send(req, (const char *)favicon_png_start,
                    favicon_png_end - favicon_png_start);
    return ESP_OK;
}

/*
 * Serialize all app_config_t fields to a cJSON object.
 * Includes config_version but NOT ssid (WiFi stack) or _dirty (runtime state).
 * Returns a new cJSON object on success, NULL on allocation failure.
 * Caller must cJSON_Delete() the result.
 */
static cJSON *serialize_config_to_json(const app_config_t *cfg)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "config_version", APP_CONFIG_VERSION);

    /* Every "simple" field (plain default + optional range check) is driven
     * from the single SETTINGS_TABLE X-macro in settings_table.h/.c. This
     * covers the large majority of scalar fields below; anything NOT covered
     * (arrays, JSON-blob strings, secrets, cross-field page targets) keeps
     * its hand-written call below, in original relative order. */
    settings_json_serialize(cfg, obj);

    cJSON_AddStringToObject(obj, "url1", cfg->api_url[0]);
    cJSON_AddStringToObject(obj, "url2", cfg->api_url[1]);
    cJSON_AddStringToObject(obj, "url3", cfg->api_url[2]);
    cJSON_AddStringToObject(obj, "filter_colors_1", cfg->filter_colors[0]);
    cJSON_AddStringToObject(obj, "filter_colors_2", cfg->filter_colors[1]);
    cJSON_AddStringToObject(obj, "filter_colors_3", cfg->filter_colors[2]);
    cJSON_AddStringToObject(obj, "rms_thresholds_1", cfg->rms_thresholds[0]);
    cJSON_AddStringToObject(obj, "rms_thresholds_2", cfg->rms_thresholds[1]);
    cJSON_AddStringToObject(obj, "rms_thresholds_3", cfg->rms_thresholds[2]);
    cJSON_AddStringToObject(obj, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
    cJSON_AddStringToObject(obj, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
    cJSON_AddStringToObject(obj, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
    cJSON_AddStringToObject(obj, "mqtt_password", cfg->mqtt_password);
    cJSON_AddNumberToObject(obj, "active_page_override", cfg->active_page_override);
    /* Retired: auto_rotate_pages/_hi and auto_rotate_order[] are migration-only
     * fields now; auto_rotate_order2 below is the sole slideshow list. */
    {
        /* New flat slideshow list: 16 ARP_IDX_* slots, 0xFF terminates/skips. */
        cJSON *order2_arr = cJSON_CreateArray();
        for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
            uint8_t v = cfg->auto_rotate_order2[i];
            if (v == 0xFF) {
                continue;   /* unused slot */
            }
            cJSON_AddItemToArray(order2_arr, cJSON_CreateNumber(v));
        }
        cJSON_AddItemToObject(obj, "auto_rotate_order2", order2_arr);
    }
    cJSON_AddNumberToObject(obj, "update_rate_s", cfg->update_rate_s);
    cJSON_AddNumberToObject(obj, "graph_update_interval_s", cfg->graph_update_interval_s);
    cJSON_AddBoolToObject(obj, "instance_enabled_1", cfg->instance_enabled[0]);
    cJSON_AddBoolToObject(obj, "instance_enabled_2", cfg->instance_enabled[1]);
    cJSON_AddBoolToObject(obj, "instance_enabled_3", cfg->instance_enabled[2]);
    cJSON_AddStringToObject(obj, "allsky_field_config", cfg->allsky_field_config);
    cJSON_AddStringToObject(obj, "allsky_thresholds", cfg->allsky_thresholds);
    cJSON_AddStringToObject(obj, "spotify_client_id", cfg->spotify_client_id);
    cJSON_AddNumberToObject(obj, "moon_drag_light_mode", cfg->moon_drag_light_mode);
    cJSON_AddNumberToObject(obj, "toast_notify_mask", cfg->toast_notify_mask);
    cJSON_AddNumberToObject(obj, "voice_notify_mask", cfg->voice_notify_mask);
    cJSON_AddBoolToObject(obj, "toast_instance_muted_1", cfg->toast_instance_muted[0]);
    cJSON_AddBoolToObject(obj, "toast_instance_muted_2", cfg->toast_instance_muted[1]);
    cJSON_AddBoolToObject(obj, "toast_instance_muted_3", cfg->toast_instance_muted[2]);
    cJSON_AddBoolToObject(obj, "alert_voice_muted_1", cfg->alert_voice_muted[0]);
    cJSON_AddBoolToObject(obj, "alert_voice_muted_2", cfg->alert_voice_muted[1]);
    cJSON_AddBoolToObject(obj, "alert_voice_muted_3", cfg->alert_voice_muted[2]);

    // Idle override (target excluded from the table: cross-field page-registry semantics)
    cJSON_AddNumberToObject(obj, "idle_page_override_target", cfg->idle_page_override_target);

    // Home Page lock (always show the Home Page regardless of connection state)
    cJSON_AddBoolToObject(obj, "home_page_lock", cfg->home_page_lock);

    return obj;
}

// Handler for getting config
esp_err_t config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    app_config_t *cfg = app_config_get();
    cJSON *root = serialize_config_to_json(cfg);
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Redact secrets. Real values are never exposed via GET /api/config.
     * The UI round-trips the "********" sentinel via POST and the server
     * preserves the existing NVS value when it sees that sentinel. */
    #define REDACT_STRING_FIELD(key) do { \
        cJSON_DeleteItemFromObject(root, #key); \
        if (cfg->key[0] != '\0') cJSON_AddStringToObject(root, #key, "********"); \
        else                     cJSON_AddStringToObject(root, #key, ""); \
    } while (0)
    REDACT_STRING_FIELD(mqtt_password);
    REDACT_STRING_FIELD(spotify_client_id);
    /* octoprint_api_key reaches this object via settings_json_serialize() (it is
     * a SETTINGS_TABLE row, unlike ha_token which is never serialized here), so
     * it MUST be redacted explicitly or GET /api/config hands out the key. The
     * write direction needs no arm: it is is_sensitive in s_backup_fields, so
     * strip_masked_secrets() drops the sentinel and preserves the stored key. */
    REDACT_STRING_FIELD(octoprint_api_key);
    #undef REDACT_STRING_FIELD
    /* admin_password is never serialized by serialize_config_to_json() at all. */

    /* WiFi networks: serialize from app_config. SSIDs exposed; passwords
     * replaced with "********" (sentinel) when present so the UI can
     * round-trip without knowing the real value. */
    {
        cJSON *wifi_arr = cJSON_AddArrayToObject(root, "wifi_networks");
        for (int i = 0; i < 3; i++) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", cfg->wifi_networks[i].ssid);
            cJSON_AddStringToObject(net, "password",
                cfg->wifi_networks[i].password[0] != '\0' ? "********" : "");
            cJSON_AddItemToArray(wifi_arr, net);
        }
        /* Backward compat: expose primary SSID as top-level "ssid" */
        cJSON_AddStringToObject(root, "ssid", cfg->wifi_networks[0].ssid);
        cJSON_AddStringToObject(root, "wifi_password",
            cfg->wifi_networks[0].password[0] != '\0' ? "********" : "");
    }

    cJSON_AddBoolToObject(root, "_dirty", app_config_is_dirty());

    return send_json_response(req, root);
}

/* ---- Backup/Restore Field Registry ---- */

/* is_sensitive and mask_preview are related but NOT the same question:
 *   is_sensitive  -- "may this leave the device in a redacted export?"
 *                    Drives the config/sensitive split and the "********"
 *                    leave-alone sentinel on the write path.
 *   mask_preview  -- "is the value itself a credential?" Drives whether a
 *                    restore-preview diff row shows the value or a placeholder.
 * hostname is the case that separates them: sensitive enough to withhold from a
 * redacted export, but its real from/to must be visible in the preview -- it is
 * the one change that renames the device mid-session, and masking it would hide
 * exactly the diff a user needs to see. mask_preview implies is_sensitive; the
 * reverse does not hold. */
typedef struct {
    const char *json_key;      /* JSON key as used in GET /api/config */
    const char *label;         /* Human-readable label for diff display */
    const char *category;      /* Tab/group name for diff grouping */
    bool        is_sensitive;  /* true = only exported in "sensitive" section */
    bool        is_large;      /* true = truncate value in diff display */
    bool        mask_preview;  /* true = never put the value in a preview diff */
} backup_field_t;

static const backup_field_t s_backup_fields[] = {
    /* Display */
    {"theme_index",          "Theme",              "Display",      false, false},
    {"brightness",           "Brightness",         "Display",      false, false},
    {"color_brightness",     "Color Brightness",   "Display",      false, false},
    {"widget_style",         "Widget Style",       "Display",      false, false},
    {"screen_rotation",      "Screen Rotation",    "Display",      false, false},

    /* Behavior */
    {"auto_rotate_enabled",         "Auto-rotate",              "Behavior", false, false},
    {"auto_rotate_interval_s",      "Rotate Interval",          "Behavior", false, false},
    {"auto_rotate_effect",          "Rotate Effect",            "Behavior", false, false},
    {"auto_rotate_skip_disconnected","Skip Disconnected",       "Behavior", false, false},
    {"update_rate_s",               "Update Rate",              "Behavior", false, false},
    {"idle_poll_interval_s",        "Idle Poll Interval",       "Behavior", false, false},
    {"connection_timeout_s",        "Connection Timeout",       "Behavior", false, false},
    {"toast_duration_s",            "Toast Duration",           "Behavior", false, false},
    {"graph_update_interval_s",     "Graph Update Interval",    "Behavior", false, false},
    {"active_page_override",        "Home Page",                "Behavior", false, false},
    {"alert_flash_enabled",         "Alert Flash",              "Behavior", false, false},
    {"alert_voice_enabled",         "Voice Alerts",             "Behavior", false, false},
    {"alert_voice_volume",          "Voice Alert Volume",       "Behavior", false, false},
    {"alert_voice_types",           "Voice Alert Types",        "Behavior", false, false},
    {"alert_voice_repeat_min",      "Voice Alert Repeat",       "Behavior", false, false},
    {"alert_voice_brief",           "Brief Voice Alerts",       "Behavior", false, false},
    {"alert_voice_conn",            "Announce NINA Connect",    "Behavior", false, false},
    {"alert_voice_disc",            "Announce NINA Disconnect", "Behavior", false, false},
    {"toast_aggregation_window_s",  "Toast Aggregation Window","Behavior", false, false},
    {"toast_notify_mask",           "Notification Categories", "Behavior", false, false},
    {"voice_notify_mask",           "Voice Alert Categories",  "Behavior", false, false},
    {"boot_jingle_enabled",         "Startup Sound",           "Behavior", false, false},
    {"screen_sleep_enabled",        "Screen Sleep",             "Behavior", false, false},
    {"screen_sleep_timeout_s",      "Screen Sleep Timeout",     "Behavior", false, false},

    /* Nodes & Data */
    {"url1",               "NINA URL 1",         "Nodes & Data", false, false},
    {"url2",               "NINA URL 2",         "Nodes & Data", false, false},
    {"url3",               "NINA URL 3",         "Nodes & Data", false, false},
    {"instance_enabled_1", "Instance 1 Enabled",  "Nodes & Data", false, false},
    {"instance_enabled_2", "Instance 2 Enabled",  "Nodes & Data", false, false},
    {"instance_enabled_3", "Instance 3 Enabled",  "Nodes & Data", false, false},
    {"toast_instance_muted_1",      "Instance 1 Muted",       "Nodes & Data", false, false},
    {"toast_instance_muted_2",      "Instance 2 Muted",       "Nodes & Data", false, false},
    {"toast_instance_muted_3",      "Instance 3 Muted",       "Nodes & Data", false, false},
    {"alert_voice_muted_1",         "Instance 1 Voice Muted", "Nodes & Data", false, false},
    {"alert_voice_muted_2",         "Instance 2 Voice Muted", "Nodes & Data", false, false},
    {"alert_voice_muted_3",         "Instance 3 Voice Muted", "Nodes & Data", false, false},
    {"filter_colors_1",    "Filter Colors 1",     "Nodes & Data", false, true},
    {"filter_colors_2",    "Filter Colors 2",     "Nodes & Data", false, true},
    {"filter_colors_3",    "Filter Colors 3",     "Nodes & Data", false, true},
    {"rms_thresholds_1",   "RMS Thresholds 1",    "Nodes & Data", false, true},
    {"rms_thresholds_2",   "RMS Thresholds 2",    "Nodes & Data", false, true},
    {"rms_thresholds_3",   "RMS Thresholds 3",    "Nodes & Data", false, true},
    {"hfr_thresholds_1",   "HFR Thresholds 1",    "Nodes & Data", false, true},
    {"hfr_thresholds_2",   "HFR Thresholds 2",    "Nodes & Data", false, true},
    {"hfr_thresholds_3",   "HFR Thresholds 3",    "Nodes & Data", false, true},
    /* JSON Display + Home Assistant pages. None of these reach the backup by
     * default: the tile layouts live in dedicated NVS keys (v52 moved them out
     * of app_config_t to keep ~6 KB each off the main /api/config payload), and
     * the eight scalars are not SETTINGS_TABLE rows, so serialize_config_to_json()
     * emits none of them. backup_get_handler and the restore preview inject all
     * ten into their local JSON explicitly; the confirm path applies the scalars
     * onto new_cfg and the layouts through app_config_set_*_tiles(). Registered
     * here so the diff, category grouping, is_large truncation, sensitive split,
     * and unknown-field detection treat them like any other field.
     *
     * Sensitivity: the two credential-bearing headers are sensitive; a tile
     * layout, a URL, an interval, and an enable flag are not. */
    {"json_enabled",           "JSON Display Page",        "Nodes & Data", false, false},
    {"json_url",               "JSON Source URL",          "Nodes & Data", false, false},
    {"json_auth_header",       "JSON Auth Header",         "Nodes & Data", true,  false, true},
    {"json_update_interval_s", "JSON Poll Interval",       "Nodes & Data", false, false},
    {"json_tiles_config",      "JSON Display Tiles",       "Nodes & Data", false, true},
    {"ha_enabled",             "Home Assistant Page",      "Nodes & Data", false, false},
    {"ha_base_url",            "Home Assistant URL",       "Nodes & Data", false, false},
    {"ha_token",               "Home Assistant Token",     "Nodes & Data", true,  false, true},
    {"ha_update_interval_s",   "Home Assistant Interval",  "Nodes & Data", false, false},
    {"ha_tiles_config",        "Home Assistant Tiles",     "Nodes & Data", false, true},

    /* OctoPrint 3D Printer page. Unlike the JSON/HA scalars above, all eight
     * of these ARE SETTINGS_TABLE rows, so serialize_config_to_json() emits them
     * and parse_config_from_json() reads them with no hand-written arm. Registered
     * here for the diff, category grouping, sensitive split, and unknown-field
     * detection — and, for the API key, to drive strip_masked_secrets(). */
    {"octoprint_enabled",           "OctoPrint Page",         "OctoPrint", false, false},
    {"octoprint_url",               "OctoPrint URL",          "OctoPrint", false, false},
    {"octoprint_api_key",           "OctoPrint API Key",      "OctoPrint", true,  false, true},
    {"octoprint_update_interval_s", "OctoPrint Poll Interval","OctoPrint", false, false},
    {"octoprint_image_source",      "OctoPrint Image Source", "OctoPrint", false, false},
    {"octoprint_layout",            "OctoPrint Layout",       "OctoPrint", false, false},
    {"octoprint_snapshot_url",      "OctoPrint Snapshot URL", "OctoPrint", false, false},
    {"octoprint_overlay_visible",   "Show readings over picture", "OctoPrint", false, false},

    /* System */
    {"ntp",                  "NTP Server",          "System", false, false},
    {"timezone",             "Timezone",             "System", false, false},
    {"debug_mode",           "Debug Mode",           "System", false, false},
    {"demo_mode",            "Demo Mode",            "System", false, false},
    {"auto_update_check",    "Auto Update Check",    "System", false, false},
    {"update_channel",       "Update Channel",       "System", false, false},
    {"deep_sleep_enabled",   "Deep Sleep",           "System", false, false},
    {"deep_sleep_wake_timer_s","Deep Sleep Timer",   "System", false, false},
    {"deep_sleep_on_idle",   "Sleep on Idle",        "System", false, false},
    {"wifi_power_save",      "WiFi Power Save",      "System", false, false},
    {"wifi_max_tx_dbm",      "WiFi Transmit Power",  "System", false, false},
    {"crash_log_retention_days","Crash Log Retention","System", false, false},

    /* AllSky */
    {"allsky_enabled",            "AllSky Enabled",       "AllSky", false, false},
    {"allsky_hostname",           "AllSky Hostname",      "AllSky", false, false},
    {"allsky_update_interval_s",  "AllSky Update Interval","AllSky", false, false},
    {"allsky_dew_offset",         "AllSky Dew Offset",    "AllSky", false, false},
    {"allsky_field_config",       "AllSky Field Config",  "AllSky", false, true},
    {"allsky_thresholds",         "AllSky Thresholds",    "AllSky", false, true},

    /* Spotify */
    {"spotify_enabled",           "Spotify Enabled",      "Spotify", false, false},
    {"spotify_poll_interval_ms",  "Spotify Poll Interval","Spotify", false, false},
    {"spotify_show_progress_bar", "Progress Bar",         "Spotify", false, false},
    {"spotify_overlay_timeout_s", "Overlay Timeout",      "Spotify", false, false},
    {"spotify_minimal_mode",      "Minimal Mode",         "Spotify", false, false},
    {"spotify_scroll_text",       "Scroll Text",          "Spotify", false, false},
    {"spotify_overlay_visible",   "Show Overlay",         "Spotify", false, false},

    /* Image pages: one category per page, matching the config tab names */
    /* Image pages (v61 split): one enable/overlay/crop/interval per page */
    {"goes_enabled",               "GOES Page Enabled",    "GOES", false, false},
    {"goes_show_overlay",          "GOES Show Overlay",    "GOES", false, false},
    {"goes_crop",                  "GOES Crop/Fill",       "GOES", false, false},
    {"moon_enabled",               "Moon Page Enabled",    "Moon", false, false},
    {"moon_show_overlay",          "Moon Show Overlay",    "Moon", false, false},
    {"moon_update_interval_s",     "Moon Update Interval", "Moon", false, false},
    {"solar_enabled",              "Solar Page Enabled",   "Solar", false, false},
    {"solar_show_overlay",         "Solar Show Overlay",   "Solar", false, false},
    {"solar_crop",                 "Solar Crop/Fill",      "Solar", false, false},
    {"solar_update_interval_s",    "Solar Update Interval","Solar", false, false},
    {"custom_enabled",             "Custom Page Enabled",  "Custom URL", false, false},
    {"custom_show_overlay",        "Custom Show Overlay",  "Custom URL", false, false},
    {"custom_crop",                "Custom Crop/Fill",     "Custom URL", false, false},
    /* Weather Radar page (v63, plus radar_dark_mode at v64 and radar_map_style at v65).
     * All eight are SETTINGS_TABLE rows, so
     * serialize_config_to_json() emits them and parse_config_from_json() reads
     * them with no hand-written arm; registered here so the diff, category
     * grouping, sensitive split and unknown-field detection treat them like any
     * other field (without this, every restore reports them as coming from a
     * newer firmware). */
    {"radar_enabled",              "Radar Page Enabled",   "Radar", false, false},
    {"radar_show_overlay",         "Radar Show Overlay",   "Radar", false, false},
    {"radar_crop",                 "Radar Crop",           "Radar", false, false},
    {"radar_token",                "Radar Area",           "Radar", false, false},
    {"radar_update_interval_s",    "Radar Update Interval","Radar", false, false},
    {"radar_frames",               "Radar Animation Length","Radar", false, false},
    {"radar_dark_mode",            "Radar Map Appearance", "Radar", false, false},
    {"radar_map_style",            "Radar Map Style",      "Radar", false, false},
    {"goes_region",                "GOES Region",          "GOES", false, false},
    {"goes_update_interval_s",     "GOES Update Interval", "GOES", false, false},
    {"custom_image_url",           "Custom Image URL",     "Custom URL", false, false},
    {"custom_orientation",         "Custom Orientation",   "Custom URL", false, false},
    {"custom_update_interval_s",   "Custom Update Interval","Custom URL", false, false},
    {"moon_bg_style",              "Moon Background Style", "Moon", false, false},
    {"moon_lat",                   "Moon Latitude",        "Moon", false, false},
    {"moon_lon",                   "Moon Longitude",       "Moon", false, false},
    {"solar_band",                 "Solar Band",           "Solar", false, false},
    {"moon_drag_light_mode",       "Moon Drag Lighting",   "Moon", false, false},
    {"goes_vflip",                 "GOES Flip Vertical",   "GOES", false, false},
    {"goes_hflip",                 "GOES Flip Horizontal", "GOES", false, false},
    {"solar_vflip",                "Solar Flip Vertical",  "Solar", false, false},
    {"solar_hflip",                "Solar Flip Horizontal","Solar", false, false},
    {"custom_vflip",               "Custom Flip Vertical", "Custom URL", false, false},
    {"custom_hflip",               "Custom Flip Horizontal","Custom URL", false, false},
    {"moon_flip_u",                "Moon Flip U",          "Moon", false, false},
    {"moon_flip_v",                "Moon Flip V",          "Moon", false, false},
    {"moon_roll_offset",           "Moon Roll Offset",     "Moon", false, false},
    {"moon_yaw_offset",            "Moon Yaw Offset",      "Moon", false, false},
    {"moon_pitch_offset",          "Moon Pitch Offset",    "Moon", false, false},
    {"moon_north_up",              "Moon North-Up",        "Moon", false, false},
    {"moon_spin_mode",             "Moon Spin Mode",       "Moon", false, false},
    {"moon_spin_return_s",         "Moon Spin Return (s)", "Moon", false, false},

    /* MQTT (non-sensitive) */
    {"mqtt_enabled",       "MQTT Enabled",       "MQTT", false, false},
    {"mqtt_port",          "MQTT Port",          "MQTT", false, false},
    {"mqtt_topic_prefix",  "MQTT Topic Prefix",  "MQTT", false, false},

    /* Weather */
    {"weather_provider",          "Weather Provider",       "Weather", false, false},
    {"weather_location_name",     "Weather Location",       "Weather", false, false},
    {"weather_lat",               "Weather Latitude",       "Weather", false, false},
    {"weather_lon",               "Weather Longitude",      "Weather", false, false},
    {"weather_poll_interval_s",   "Weather Poll Interval",  "Weather", false, false},
    {"weather_units",             "Weather Units",          "Weather", false, false},
    {"weather_time_format",       "Time Format",            "Weather", false, false},

    /* Idle Override */
    {"idle_page_override_enabled","Idle Override Enabled",  "Behavior", false, false},
    {"idle_page_override_target", "Idle Override Target",   "Behavior", false, false},
    {"idle_indicator_enabled",   "Idle Indicator Enabled", "Behavior", false, false},
    {"nav_grace_s",              "Manual Nav Grace (s)",   "Behavior", false, false},
    {"home_page_lock",           "Always show Home Page",  "Behavior", false, false},
    {"auth_enabled",             "Authentication Enabled", "System",   false, false},

    /* Sensitive. mask_preview (6th column) is set on everything whose VALUE is a
     * credential -- including mqtt_broker_url, which may embed user:pass@host.
     * hostname is deliberately left unmasked: still withheld from a redacted
     * export, but its real from/to shows in the preview, because a hostname
     * change renames the device mid-session and is the diff a user most needs
     * to see. */
    {"weather_api_key",    "Weather API Key",    "Weather", true, false, true},
    {"mqtt_username",      "MQTT Username",      "MQTT",    true, false, true},
    {"mqtt_password",      "MQTT Password",      "MQTT",    true, false, true},
    {"mqtt_broker_url",    "MQTT Broker URL",    "MQTT",    true, false, true},
    {"spotify_client_id",  "Spotify Client ID",  "Spotify", true, false, true},
    {"hostname",           "Hostname",           "System",  true, false, false},

    {NULL, NULL, NULL, false, false, false}  /* sentinel */
};

/* Returns true if two cJSON values are equal */
static bool cjson_values_equal(const cJSON *a, const cJSON *b)
{
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;

    if (cJSON_IsString(a)) return strcmp(a->valuestring, b->valuestring) == 0;
    if (cJSON_IsNumber(a)) return a->valuedouble == b->valuedouble;
    if (cJSON_IsBool(a))   return cJSON_IsTrue(a) == cJSON_IsTrue(b);
    return false;  /* objects/arrays not compared field-by-field */
}

/* ---- Restore-preview validation tables ---- */

/* String fields with a maximum length. Mirrors validate_config_fields()
 * (web_handlers_config.c) exactly; validate_string_len() flags when
 * strlen >= max, so the limit is "max - 1" usable characters. */
typedef struct { const char *json_key; size_t max_len; } restore_strmax_t;
static const restore_strmax_t s_restore_strmax[] = {
    {"hostname",            32},
    {"url1",                128},
    {"url2",                128},
    {"url3",                128},
    {"ntp",                 64},
    {"timezone",            64},
    {"mqtt_broker_url",     128},
    {"mqtt_username",       64},
    {"mqtt_password",       64},
    {"mqtt_topic_prefix",   64},
    {"filter_colors_1",     512},
    {"filter_colors_2",     512},
    {"filter_colors_3",     512},
    {"rms_thresholds_1",    256},
    {"rms_thresholds_2",    256},
    {"rms_thresholds_3",    256},
    {"hfr_thresholds_1",    256},
    {"hfr_thresholds_2",    256},
    {"hfr_thresholds_3",    256},
    {"allsky_hostname",     128},
    {"allsky_field_config", 1536},
    {"allsky_thresholds",   1024},
    {"custom_image_url",    256},
    {"json_tiles_config",   JSON_TILES_CONFIG_MAX},
    {"ha_tiles_config",     HA_TILES_CONFIG_MAX},
    {"json_url",            256},
    {"json_auth_header",    256},
    {"ha_base_url",         256},
    {"ha_token",            256},
    {"octoprint_url",           128},
    {"octoprint_api_key",       64},
    {"octoprint_snapshot_url",  128},
    /* goes_region max sourced from the struct field size at runtime below */
    {NULL, 0}
};

/* Fields whose value must satisfy validate_url_format() (empty allowed = "not
 * configured"). ONE list drives both the restore-preview check
 * (check_restore_field step 4) and the write-path check (validate_config_fields),
 * so a field can never preview clean and then 400 on confirm. Adding a URL field
 * means adding one row here, not editing two strcmp chains. */
static const char *const s_url_fields[] = {
    "url1", "url2", "url3", "mqtt_broker_url", "json_url", "ha_base_url",
    "octoprint_url", "octoprint_snapshot_url", NULL
};

static bool is_url_field(const char *json_key) {
    for (const char *const *k = s_url_fields; *k; k++) {
        if (strcmp(*k, json_key) == 0) return true;
    }
    return false;
}

/* Numeric fields with a confirmable [min,max] clamp. Ranges sourced directly
 * from app_config.c validate_config() (line refs in comments). is_float marks
 * fields clamped as floats so the message formats sensibly. Out-of-range
 * values are silently clamped on restore, so these are WARNINGS only. */
typedef struct { const char *json_key; double min; double max; bool is_float; } restore_numrange_t;
static const restore_numrange_t s_restore_numrange[] = {
    {"color_brightness",        0,    100,   false},  /* app_config.c:2468 */
    {"brightness",              0,    100,   false},  /* app_config.c:2476 */
    {"auto_rotate_interval_s",  1,    3600,  false},  /* app_config.c:2498 (0 invalid) */
    {"auto_rotate_effect",      0,    3,     false},  /* app_config.c:2502 */
    {"update_rate_s",           1,    10,    false},  /* app_config.c:2516 */
    {"graph_update_interval_s", 2,    30,    false},  /* app_config.c:2520 */
    {"connection_timeout_s",    2,    30,    false},  /* app_config.c:2524 */
    {"toast_duration_s",        3,    30,    false},  /* app_config.c:2528 */
    {"screen_sleep_timeout_s",  10,   3600,  false},  /* app_config.c:2532 */
    {"idle_poll_interval_s",    5,    120,   false},  /* app_config.c:2536 */
    {"screen_rotation",         0,    3,     false},  /* app_config.c:2544 */
    {"allsky_update_interval_s",1,    300,   false},  /* app_config.c:2548 */
    {"json_update_interval_s",  5,    300,   false},  /* app_config.c validate_config (out of range -> 30) */
    {"ha_update_interval_s",    5,    300,   false},  /* app_config.c validate_config (out of range -> 30) */
    {"octoprint_update_interval_s", 2, 300,  false},  /* settings_table.h INT row (clamped to bound) */
    {"octoprint_image_source",  0,    1,     false},  /* settings_table.h INT_RESET row (out of range -> 0) */
    {"octoprint_layout",        0,    6,     false},  /* settings_table.h INT_RESET row (out of range -> 0) */
    {"allsky_dew_offset",       -50,  50,    true},   /* app_config.c:2552 */
    {"goes_update_interval_s",  300,  7200,  false},  /* app_config.c:2556 */
    {"solar_update_interval_s", 300,  7200,  false},  /* settings_table.h INT_RESET row (out of range -> 600) */
    {"moon_update_interval_s",  10,   3600,  false},  /* settings_table.h INT_RESET row (out of range -> 60) */
    {"custom_orientation",      0,    3,     false},  /* app_config.c:2590 */
    {"custom_update_interval_s",10,   7200,  false},  /* app_config.c:2594 */
    {"moon_bg_style",           0,    3,     false},  /* app_config.c:2569 */
    {"solar_band",              0,    17,    false},  /* app_config.c:2573 */
    {"moon_drag_light_mode",    0,    2,     false},  /* app_config.c:2577 */
    {"moon_roll_offset",        -180, 180,   true},   /* app_config.c:2589 */
    {"moon_yaw_offset",         -180, 180,   true},   /* app_config.c:2594 */
    {"moon_pitch_offset",       -90,  90,    true},   /* app_config.c:2599 */
    {"moon_spin_return_s",      3,    60,    false},  /* app_config.c:2608 */
    {"nav_grace_s",             10,   300,   false},  /* app_config.c:2624 */
    {"spotify_poll_interval_ms",1000, 30000, false},  /* app_config.c:2626 */
    {NULL, 0, 0, false}
};

/* Append a {severity, message} object to the validation_notes array.
 * Sets *blocked = true when severity is "error". */
static void add_validation_note(cJSON *notes, bool *blocked,
                                const char *severity, const char *message)
{
    cJSON *note = cJSON_CreateObject();
    if (!note) return;
    cJSON_AddStringToObject(note, "severity", severity);
    cJSON_AddStringToObject(note, "message", message);
    cJSON_AddItemToArray(notes, note);
    if (strcmp(severity, "error") == 0 && blocked) *blocked = true;
}

/* Run the per-field restore validation checks for one backup field.
 * backup_value is guaranteed non-NULL; current_value may be NULL. */
static void check_restore_field(const backup_field_t *f,
                                const cJSON *backup_value,
                                const cJSON *current_value,
                                cJSON *notes, bool *blocked)
{
    char msg[256];

    /* 1. Malformed JSON in large (JSON-encoded) string fields -> ERROR */
    if (f->is_large && cJSON_IsString(backup_value)) {
        cJSON *t = cJSON_Parse(backup_value->valuestring);
        if (t == NULL) {
            snprintf(msg, sizeof(msg),
                     "%s: contains invalid JSON and cannot be restored.", f->label);
            add_validation_note(notes, blocked, "error", msg);
        }
        cJSON_Delete(t);
    }

    /* 2. Type mismatch vs current firmware -> ERROR */
    if (current_value) {
        int bk = cJSON_IsString(backup_value) ? 1 :
                 cJSON_IsNumber(backup_value) ? 2 :
                 cJSON_IsBool(backup_value)   ? 3 : 0;
        int ck = cJSON_IsString(current_value) ? 1 :
                 cJSON_IsNumber(current_value) ? 2 :
                 cJSON_IsBool(current_value)   ? 3 : 0;
        if (bk != 0 && ck != 0 && bk != ck) {
            snprintf(msg, sizeof(msg),
                     "%s: value type does not match this firmware and cannot be restored.",
                     f->label);
            add_validation_note(notes, blocked, "error", msg);
        }
    }

    /* 3. String too long -> ERROR (mirror validate_config_fields boundary) */
    if (cJSON_IsString(backup_value)) {
        size_t max_len = 0;
        bool have_max = false;
        if (strcmp(f->json_key, "goes_region") == 0) {
            max_len = sizeof(((app_config_t *)0)->goes_region);
            have_max = true;
        } else {
            for (const restore_strmax_t *s = s_restore_strmax; s->json_key; s++) {
                if (strcmp(s->json_key, f->json_key) == 0) {
                    max_len = s->max_len;
                    have_max = true;
                    break;
                }
            }
        }
        if (have_max && strlen(backup_value->valuestring) >= max_len) {
            snprintf(msg, sizeof(msg),
                     "%s: value is too long (maximum %d characters).",
                     f->label, (int)(max_len - 1));
            add_validation_note(notes, blocked, "error", msg);
        }
    }

    /* 4. Invalid URL -> ERROR. Same s_url_fields list validate_config_fields
     *    enforces on confirm, so preview and confirm cannot disagree. */
    if (cJSON_IsString(backup_value) && backup_value->valuestring[0] != '\0') {
        if (is_url_field(f->json_key) &&
            !validate_url_format(backup_value->valuestring)) {
            snprintf(msg, sizeof(msg), "%s: is not a valid URL.", f->label);
            add_validation_note(notes, blocked, "error", msg);
        }
    }

    /* 5. Numeric out of range -> WARNING (only when the value actually changed) */
    if (cJSON_IsNumber(backup_value) &&
        !cjson_values_equal(backup_value, current_value)) {
        for (const restore_numrange_t *n = s_restore_numrange; n->json_key; n++) {
            if (strcmp(n->json_key, f->json_key) != 0) continue;
            double v = backup_value->valuedouble;
            if (v < n->min || v > n->max) {
                if (n->is_float) {
                    snprintf(msg, sizeof(msg),
                             "%s: value %g is outside the supported range %g to %g "
                             "and will be adjusted on restore.",
                             f->label, v, n->min, n->max);
                } else {
                    snprintf(msg, sizeof(msg),
                             "%s: value %lld is outside the supported range %lld to %lld "
                             "and will be adjusted on restore.",
                             f->label, (long long)v, (long long)n->min, (long long)n->max);
                }
                add_validation_note(notes, blocked, "warning", msg);
            }
            break;
        }
    }

    /* 6. Hostname format -> WARNING */
    if (strcmp(f->json_key, "hostname") == 0 &&
        cJSON_IsString(backup_value) && backup_value->valuestring[0] != '\0') {
        bool bad = false;
        for (const char *c = backup_value->valuestring; *c; c++) {
            char ch = *c;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '-')) {
                bad = true;
                break;
            }
        }
        if (bad) {
            add_validation_note(notes, blocked, "warning",
                "Hostname contains characters that may not work as a network name; "
                "use only letters, numbers, and hyphens.");
        }
    }
}

/* Display stand-in for a secret in a preview diff row. Reveals only whether a
 * value is present, never the value itself. "********" is the same sentinel the
 * restore confirm path already treats as "leave the live credential alone", so
 * a preview row that is echoed back verbatim is inert. */
static const char *sensitive_placeholder(const cJSON *v)
{
    if (v == NULL || cJSON_IsNull(v)) return "(not set)";
    if (cJSON_IsString(v) && v->valuestring[0] == '\0') return "(not set)";
    return "********";
}

/*
 * Build restore preview response.
 * backup_root: the parsed backup JSON (contains "meta", "config", optionally "sensitive")
 * current_json: the current config serialized as JSON (same format as GET /api/config)
 * Returns a cJSON object with the full preview response, or NULL on error.
 * Caller must cJSON_Delete() the result.
 */
static cJSON *build_restore_preview(const cJSON *backup_root, const cJSON *current_json)
{
    const cJSON *meta = cJSON_GetObjectItem(backup_root, "meta");
    const cJSON *backup_config = cJSON_GetObjectItem(backup_root, "config");
    const cJSON *backup_sensitive = cJSON_GetObjectItem(backup_root, "sensitive");

    int backup_ver = 0;
    if (meta) {
        cJSON *ver = cJSON_GetObjectItem(meta, "config_version");
        if (cJSON_IsNumber(ver)) backup_ver = ver->valueint;
    }
    int current_ver = APP_CONFIG_VERSION;

    /* Determine version match type */
    const char *version_match;
    if (backup_ver == current_ver)               version_match = "exact";
    else if (backup_ver > current_ver)           version_match = "newer";
    else if (current_ver - backup_ver > 10)      version_match = "much_older";
    else                                         version_match = "older";

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "preview");
    cJSON_AddStringToObject(resp, "version_match", version_match);
    cJSON_AddNumberToObject(resp, "backup_version", backup_ver);
    cJSON_AddNumberToObject(resp, "current_version", current_ver);

    /* Copy metadata fields */
    if (meta) {
        cJSON *fw = cJSON_GetObjectItem(meta, "firmware_version");
        if (cJSON_IsString(fw)) cJSON_AddStringToObject(resp, "backup_firmware", fw->valuestring);
        cJSON *hn = cJSON_GetObjectItem(meta, "hostname");
        if (cJSON_IsString(hn)) cJSON_AddStringToObject(resp, "backup_hostname", hn->valuestring);
        cJSON *mac = cJSON_GetObjectItem(meta, "mac_address");
        if (cJSON_IsString(mac)) cJSON_AddStringToObject(resp, "backup_mac", mac->valuestring);
        cJSON *dt = cJSON_GetObjectItem(meta, "export_date");
        if (cJSON_IsString(dt)) cJSON_AddStringToObject(resp, "export_date", dt->valuestring);
    }

    /* Build warnings */
    cJSON *warnings = cJSON_CreateArray();
    if (strcmp(version_match, "exact") == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Config version matches (v%d). All settings will be restored.", current_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    } else if (strcmp(version_match, "older") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "Backup is from an older config version (v%d -> v%d). "
            "Settings added since v%d will keep their current values.",
            backup_ver, current_ver, backup_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    } else if (strcmp(version_match, "much_older") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "Backup is from a much older version (v%d -> v%d). "
            "Many settings may have been added since your backup version and will keep current values.",
            backup_ver, current_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    } else if (strcmp(version_match, "newer") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "Backup is from a newer firmware (v%d -> v%d). "
            "Some settings may not be recognized by this firmware and will be skipped.",
            backup_ver, current_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    }

    /* Check dirty state */
    if (app_config_is_dirty()) {
        cJSON_AddItemToArray(warnings, cJSON_CreateString(
            "You have unsaved changes that will be overwritten by this restore."));
    }
    cJSON_AddItemToObject(resp, "warnings", warnings);

    /* Walk field registry to compute diff */
    cJSON *changes = cJSON_CreateObject();       /* category -> array of change objects */
    cJSON *missing_fields = cJSON_CreateArray();
    cJSON *unknown_fields = cJSON_CreateArray();
    cJSON *sensitive_excluded = cJSON_CreateArray();
    cJSON *validation_notes = cJSON_CreateArray();
    cJSON *no_changes_arr = cJSON_CreateArray();
    int total_changes = 0;
    bool sensitive_included = (backup_sensitive != NULL);
    bool restore_blocked = false;

    /* Track which categories have changes */
    const char *categories[] = {"Display", "Behavior", "Nodes & Data", "System", "AllSky", "Spotify", "MQTT", "OctoPrint", "GOES", "Moon", "Solar", "Custom URL", "Radar"};
    int cat_counts[13] = {0};
    int num_categories = 13;

    for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
        /* Determine which backup section this field comes from */
        const cJSON *backup_value = NULL;
        if (f->is_sensitive) {
            if (backup_sensitive) {
                backup_value = cJSON_GetObjectItem(backup_sensitive, f->json_key);
            }
            if (!backup_value) {
                /* Sensitive field not in backup */
                cJSON_AddItemToArray(sensitive_excluded, cJSON_CreateString(f->json_key));
                continue;
            }
        } else {
            backup_value = cJSON_GetObjectItem(backup_config, f->json_key);
        }

        if (!backup_value) {
            /* Field missing from backup (older version) */
            cJSON_AddItemToArray(missing_fields, cJSON_CreateString(f->json_key));
            continue;
        }

        /* Compare with current value */
        const cJSON *current_value = cJSON_GetObjectItem(current_json, f->json_key);

        /* Pre-restore validation (runs whether or not the value changed; the
         * numeric range check itself only fires on changed values). */
        check_restore_field(f, backup_value, current_value, validation_notes, &restore_blocked);

        if (cjson_values_equal(backup_value, current_value)) {
            continue;  /* No change */
        }

        /* Record the change */
        total_changes++;

        /* Find or create category array */
        cJSON *cat_arr = cJSON_GetObjectItem(changes, f->category);
        if (!cat_arr) {
            cat_arr = cJSON_CreateArray();
            cJSON_AddItemToObject(changes, f->category, cat_arr);
        }

        cJSON *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change, "field", f->json_key);
        cJSON_AddStringToObject(change, "label", f->label);

        /* Add from/to values.
         *
         * A credential never goes on the wire, not even in a preview the browser
         * only renders masked: the row's existence already says "this changes",
         * and the placeholders say whether a value is being set or cleared. This
         * is the single choke point every diff row passes through, so a secret
         * added later is protected by setting mask_preview in the registry -- not
         * by remembering to mask at a call site. The other preview arrays carry key
         * names only (missing_fields, unknown_fields, sensitive_excluded) and the
         * validation notes are built from f->label, so this is the only path that
         * ever had a value to leak.
         *
         * Large JSON-blob fields are summarized by length rather than inlined. */
        if (f->mask_preview) {
            cJSON_AddStringToObject(change, "from", sensitive_placeholder(current_value));
            cJSON_AddStringToObject(change, "to",   sensitive_placeholder(backup_value));
        } else if (f->is_large && cJSON_IsString(backup_value)) {
            char trunc[80];
            snprintf(trunc, sizeof(trunc), "Modified (%d chars)", (int)strlen(backup_value->valuestring));
            cJSON_AddStringToObject(change, "to", trunc);
            if (current_value && cJSON_IsString(current_value)) {
                snprintf(trunc, sizeof(trunc), "Current (%d chars)", (int)strlen(current_value->valuestring));
                cJSON_AddStringToObject(change, "from", trunc);
            } else {
                cJSON_AddStringToObject(change, "from", "(empty)");
            }
        } else {
            cJSON_AddItemToObject(change, "from", current_value ? cJSON_Duplicate(current_value, true) : cJSON_CreateNull());
            cJSON_AddItemToObject(change, "to", cJSON_Duplicate(backup_value, true));
        }

        cJSON_AddItemToArray(cat_arr, change);

        /* Track category counts */
        for (int c = 0; c < num_categories; c++) {
            if (strcmp(f->category, categories[c]) == 0) { cat_counts[c]++; break; }
        }
    }

    /* Check for unknown fields in backup (from newer firmware) */
    if (backup_config) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, backup_config) {
            bool found = false;
            for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
                if (!f->is_sensitive && strcmp(f->json_key, item->string) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                cJSON_AddItemToArray(unknown_fields, cJSON_CreateString(item->string));
            }
        }
    }

    /* Build no_changes list */
    for (int c = 0; c < num_categories; c++) {
        if (cat_counts[c] == 0) {
            cJSON_AddItemToArray(no_changes_arr, cJSON_CreateString(categories[c]));
        }
    }

    cJSON_AddItemToObject(resp, "missing_fields", missing_fields);
    cJSON_AddItemToObject(resp, "unknown_fields", unknown_fields);
    cJSON_AddItemToObject(resp, "validation_notes", validation_notes);
    cJSON_AddBoolToObject(resp, "restore_blocked", restore_blocked);
    cJSON_AddBoolToObject(resp, "sensitive_included", sensitive_included);
    cJSON_AddItemToObject(resp, "sensitive_excluded", sensitive_excluded);
    cJSON_AddItemToObject(resp, "changes", changes);
    cJSON_AddItemToObject(resp, "no_changes", no_changes_arr);
    cJSON_AddNumberToObject(resp, "total_changes", total_changes);

    return resp;
}

/* ---- Shared config parsing helpers ---- */

// Validate all config string field lengths and URL formats.
// Returns true if valid; sends 400 and returns false if invalid.
static bool validate_config_fields(cJSON *root, httpd_req_t *req)
{
    if (!validate_string_len(root, "hostname", 32) ||
        !validate_string_len(root, "url1", 128) ||
        !validate_string_len(root, "url2", 128) ||
        !validate_string_len(root, "url3", 128) ||
        !validate_string_len(root, "ntp", 64) ||
        !validate_string_len(root, "timezone", 64) ||
        !validate_string_len(root, "mqtt_broker_url", 128) ||
        !validate_string_len(root, "mqtt_username", 64) ||
        !validate_string_len(root, "mqtt_password", 64) ||
        !validate_string_len(root, "mqtt_topic_prefix", 64) ||
        !validate_string_len(root, "filter_colors_1", 512) ||
        !validate_string_len(root, "filter_colors_2", 512) ||
        !validate_string_len(root, "filter_colors_3", 512) ||
        !validate_string_len(root, "rms_thresholds_1", 256) ||
        !validate_string_len(root, "rms_thresholds_2", 256) ||
        !validate_string_len(root, "rms_thresholds_3", 256) ||
        !validate_string_len(root, "hfr_thresholds_1", 256) ||
        !validate_string_len(root, "hfr_thresholds_2", 256) ||
        !validate_string_len(root, "hfr_thresholds_3", 256) ||
        !validate_string_len(root, "allsky_hostname", 128) ||
        !validate_string_len(root, "allsky_field_config", 1536) ||
        !validate_string_len(root, "allsky_thresholds", 1024) ||
        !validate_string_len(root, "goes_region", sizeof(((app_config_t *)0)->goes_region)) ||
        !validate_string_len(root, "custom_image_url", sizeof(((app_config_t *)0)->custom_image_url)) ||
        /* JSON Display / Home Assistant page fields (restore path only; absent
         * from the config POST body, which routes them through their own
         * endpoints). Bounds mirror json_config_post_handler /
         * ha_config_post_handler exactly. */
        !validate_string_len(root, "json_url", sizeof(((app_config_t *)0)->json_url)) ||
        !validate_string_len(root, "json_auth_header", sizeof(((app_config_t *)0)->json_auth_header)) ||
        !validate_string_len(root, "json_tiles_config", JSON_TILES_CONFIG_MAX) ||
        !validate_string_len(root, "ha_base_url", sizeof(((app_config_t *)0)->ha_base_url)) ||
        !validate_string_len(root, "ha_token", sizeof(((app_config_t *)0)->ha_token)) ||
        !validate_string_len(root, "ha_tiles_config", HA_TILES_CONFIG_MAX) ||
        /* OctoPrint page fields. Unlike the JSON/HA block above these DO arrive
         * in the main config POST body (they are SETTINGS_TABLE rows), so this
         * length gate runs on the normal save path as well as on restore. */
        !validate_string_len(root, "octoprint_url", sizeof(((app_config_t *)0)->octoprint_url)) ||
        !validate_string_len(root, "octoprint_api_key", sizeof(((app_config_t *)0)->octoprint_api_key)) ||
        !validate_string_len(root, "octoprint_snapshot_url", sizeof(((app_config_t *)0)->octoprint_snapshot_url))) {
        send_400(req, "String field exceeds maximum length");
        return false;
    }

    /* Driven by s_url_fields, the same list check_restore_field step 4 uses for
     * the preview, so a malformed URL is reported at preview time instead of
     * surfacing here as a field-less 400 on confirm. validate_url_format() lets
     * an empty string through, so clearing any of these still works. */
    for (const char *const *k = s_url_fields; *k; k++) {
        cJSON *u = cJSON_GetObjectItem(root, *k);
        if (cJSON_IsString(u) && !validate_url_format(u->valuestring)) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Invalid URL format in '%s'", *k);
            send_400(req, msg);
            return false;
        }
    }
    return true;
}

/* Delete every sensitive key whose incoming value is the redaction sentinel.
 *
 * GET /api/config and the restore preview both hand out "********" in place of a
 * real secret, so a round-tripped payload carries the sentinel rather than the
 * value. Because parse_config_from_json() starts from a copy of the live config
 * and every field parse is key-present-gated, deleting the key IS "preserve the
 * existing value" -- one registry-driven pass replaces a per-secret `if (strcmp
 * != "********")` arm, and covers a hand-edited backup with literal asterisks in
 * a field nobody remembered to guard.
 *
 * Driven by is_sensitive (all 8), not mask_preview (7): hostname shows its real
 * value in a preview but must still honour the sentinel on the write path. */
static void strip_masked_secrets(cJSON *root)
{
    for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
        if (!f->is_sensitive) continue;
        cJSON *v = cJSON_GetObjectItem(root, f->json_key);
        if (cJSON_IsString(v) && strcmp(v->valuestring, "********") == 0) {
            cJSON_DeleteItemFromObject(root, f->json_key);
        }
    }
}

// Parse JSON into a new app_config_t (heap-allocated).
// Starts from a copy of the current config so missing fields are preserved.
// Returns NULL on allocation failure.
// NOTE: mutates `root` -- strip_masked_secrets() removes redacted secrets so
// they cannot be written. Callers delete `root` shortly after, and the restore
// path relies on the same removal.
static app_config_t *parse_config_from_json(cJSON *root)
{
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        ESP_LOGE(TAG, "parse_config_from_json: PSRAM alloc failed");
        return NULL;
    }
    memcpy(cfg, app_config_get(), sizeof(app_config_t));

    strip_masked_secrets(root);

    /* Every "simple" field (plain default + optional range check) is driven
     * from the single SETTINGS_TABLE X-macro in settings_table.h/.c. This
     * covers the large majority of scalar fields below; anything NOT covered
     * (arrays, JSON-blob strings, secrets with sentinel handling, cross-field
     * page targets) keeps its hand-written parse below, in original relative
     * order. */
    settings_json_parse(root, cfg);

    /* radar_token is a SETTINGS_TABLE STR row, so the main Save writes it here
     * as well as the radar tab's own POST. It is pasted straight into the NWS
     * image URL, so this path needs the same charset gate as the other two
     * (validate_config() on load, radar_token_is_valid() in the image handler):
     * fold lowercase up, and treat anything outside [A-Z0-9] as junk -> empty,
     * which means "resolve the nearest site at fetch time". */
    for (int i = 0; cfg->radar_token[i] != '\0'; i++) {
        char ch = cfg->radar_token[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
            cfg->radar_token[i] = ch;
        }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            cfg->radar_token[0] = '\0';
            break;
        }
    }

    JSON_TO_STRING(root, "url1",           cfg->api_url[0]);
    JSON_TO_STRING(root, "url2",           cfg->api_url[1]);
    JSON_TO_STRING(root, "url3",           cfg->api_url[2]);
    JSON_TO_STRING(root, "filter_colors_1", cfg->filter_colors[0]);
    JSON_TO_STRING(root, "filter_colors_2", cfg->filter_colors[1]);
    JSON_TO_STRING(root, "filter_colors_3", cfg->filter_colors[2]);
    JSON_TO_STRING(root, "rms_thresholds_1", cfg->rms_thresholds[0]);
    JSON_TO_STRING(root, "rms_thresholds_2", cfg->rms_thresholds[1]);
    JSON_TO_STRING(root, "rms_thresholds_3", cfg->rms_thresholds[2]);
    JSON_TO_STRING(root, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
    JSON_TO_STRING(root, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
    JSON_TO_STRING(root, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
    /* mqtt_password: a sentinel value was already removed by
     * strip_masked_secrets(), so key-present means "a real new value". */
    JSON_TO_STRING(root, "mqtt_password", cfg->mqtt_password);

    /* Home Page now stores a page_ref registry id (0..PAGE_REF_ID_MAX-1). The web
     * UI always sends a concrete id (never -1). Reject out-of-range ids and the
     * Settings page (never a valid Home Page target) by falling back to 0
     * (Summary). PAGE_REF_ID_MAX/PAGE_REF_SETTINGS come from ui/page_registry.h. */
    cJSON *apo_item = cJSON_GetObjectItem(root, "active_page_override");
    if (cJSON_IsNumber(apo_item)) {
        int v = apo_item->valueint;
        if (v < 0 || v >= PAGE_REF_ID_MAX || v == PAGE_REF_SETTINGS) {
            v = 0;   /* Summary */
        }
        cfg->active_page_override = (int8_t)v;
    }

    /* The flat slideshow list is the single ordered auto_rotate_order2[] list:
     * membership equals order. The legacy auto_rotate_pages/_hi bitmask and
     * auto_rotate_order[]/_ext list are retired (migration-only) and are not
     * accepted here. */
    cJSON *order2_arr = cJSON_GetObjectItem(root, "auto_rotate_order2");
    if (cJSON_IsArray(order2_arr)) {
        int count2 = cJSON_GetArraySize(order2_arr);
        if (count2 > ARP_ORDER_CAPACITY) count2 = ARP_ORDER_CAPACITY;
        for (int i = 0; i < count2; i++) {
            cJSON *item2 = cJSON_GetArrayItem(order2_arr, i);
            uint8_t v = 0xFF;
            if (cJSON_IsNumber(item2) &&
                item2->valueint >= 0 && ARP_STOP_IS_VALID(item2->valueint)) {
                v = (uint8_t)item2->valueint;
            }
            cfg->auto_rotate_order2[i] = v;
        }
        for (int i = count2; i < ARP_ORDER_CAPACITY; i++) {
            cfg->auto_rotate_order2[i] = 0xFF;
        }
    }

    cJSON *ur_item = cJSON_GetObjectItem(root, "update_rate_s");
    if (cJSON_IsNumber(ur_item)) {
        int v = ur_item->valueint;
        if (v < 1) v = 1;
        if (v > 10) v = 10;
        cfg->update_rate_s = (uint8_t)v;
    }

    cJSON *gui_item = cJSON_GetObjectItem(root, "graph_update_interval_s");
    if (cJSON_IsNumber(gui_item)) {
        int v = gui_item->valueint;
        if (v < 2) v = 2;
        if (v > 30) v = 30;
        cfg->graph_update_interval_s = (uint8_t)v;
    }

    JSON_TO_BOOL(root, "instance_enabled_1", cfg->instance_enabled[0]);
    JSON_TO_BOOL(root, "instance_enabled_2", cfg->instance_enabled[1]);
    JSON_TO_BOOL(root, "instance_enabled_3", cfg->instance_enabled[2]);

    JSON_TO_STRING(root, "allsky_field_config",  cfg->allsky_field_config);
    JSON_TO_STRING(root, "allsky_thresholds",    cfg->allsky_thresholds);

    /* spotify_client_id: sentinel already removed by strip_masked_secrets(). */
    JSON_TO_STRING(root, "spotify_client_id", cfg->spotify_client_id);
    /* admin_password is never accepted via /api/config — use /api/admin-password. */

    cJSON *jmoondrag = cJSON_GetObjectItem(root, "moon_drag_light_mode");
    if (cJSON_IsNumber(jmoondrag)) {
        int v = jmoondrag->valueint;
        cfg->moon_drag_light_mode = (v >= 0 && v <= 2) ? (uint8_t)v : 0;
    }

    cJSON *tnm_item = cJSON_GetObjectItem(root, "toast_notify_mask");
    if (cJSON_IsNumber(tnm_item)) {
        /* Bound to defined notification-category bits (highest used bit = 11).
         * Mask to 20 bits for headroom; rejects garbage high-bit values. */
        double d = tnm_item->valuedouble;
        if (d < 0) d = 0;
        cfg->toast_notify_mask = (uint32_t)d & 0xFFFFFu;
    }

    cJSON *vnm_item = cJSON_GetObjectItem(root, "voice_notify_mask");
    if (cJSON_IsNumber(vnm_item)) {
        /* Bound to defined notification-category bits (highest used bit = 11). */
        double d = vnm_item->valuedouble;
        if (d < 0) d = 0;
        cfg->voice_notify_mask = (uint32_t)d & 0xFFFu;
    }

    JSON_TO_BOOL(root, "toast_instance_muted_1", cfg->toast_instance_muted[0]);
    JSON_TO_BOOL(root, "toast_instance_muted_2", cfg->toast_instance_muted[1]);
    JSON_TO_BOOL(root, "toast_instance_muted_3", cfg->toast_instance_muted[2]);
    JSON_TO_BOOL(root, "alert_voice_muted_1", cfg->alert_voice_muted[0]);
    JSON_TO_BOOL(root, "alert_voice_muted_2", cfg->alert_voice_muted[1]);
    JSON_TO_BOOL(root, "alert_voice_muted_3", cfg->alert_voice_muted[2]);

    /* idle_page_override_target now stores a page_ref registry id
     * (0..PAGE_REF_ID_MAX-1). Out-of-range falls back to 0 (Summary).
     * PAGE_REF_ID_MAX comes from ui/page_registry.h. */
    cJSON *ipt_item = cJSON_GetObjectItem(root, "idle_page_override_target");
    if (cJSON_IsNumber(ipt_item)) {
        int v = ipt_item->valueint;
        if (v < 0 || v >= PAGE_REF_ID_MAX) v = 0;
        cfg->idle_page_override_target = (int8_t)v;
    }

    // Home Page lock (always show the Home Page). app_config_save() normalizes
    // exclusivity: the lock clears auto-rotate and idle override when set.
    JSON_TO_BOOL(root, "home_page_lock", cfg->home_page_lock);

    return cfg;
}

// Receive and parse JSON body from a POST request.
// Returns parsed cJSON root on success, NULL on failure (error response already sent).
// Non-static: shared with other web_handlers_*.c via web_server_internal.h.
cJSON *receive_json_body(httpd_req_t *req, int max_size)
{
    int remaining = req->content_len;
    if (remaining >= max_size) {
        send_400(req, "Payload too large");
        return NULL;
    }

    char *buf = heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Config handler: malloc failed for payload buffer");
        httpd_resp_send_500(req);
        return NULL;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, buf + received, remaining - received);
        if (ret <= 0) {
            memset(buf, 0, (size_t)received);   /* see scrub note below */
            free(buf);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Config handler: recv timeout (got %d/%d bytes)", received, remaining);
                httpd_resp_send_408(req);
            } else {
                ESP_LOGW(TAG, "Config handler: recv error %d (got %d/%d bytes)", ret, received, remaining);
            }
            return NULL;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    /* Scrub before releasing: config POST bodies routinely carry the admin
     * password, WiFi passphrases, an HA token, or a source device's password
     * (/api/config/pull). free() does not clear, and this PSRAM block is handed
     * straight to the next allocation, so a later handler could read a stale
     * secret out of its own fresh buffer. One memset here covers every caller. */
    memset(buf, 0, (size_t)received);
    free(buf);
    if (!root) {
        send_400(req, "Invalid JSON");
        return NULL;
    }
    return root;
}

// Handler for saving config (persists to NVS)
esp_err_t config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = receive_json_body(req, CONFIG_MAX_PAYLOAD);
    if (!root) return ESP_OK;

    if (!validate_config_fields(root, req)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    app_config_t *cfg = parse_config_from_json(root);
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    /* WiFi networks: accept array or legacy single ssid/pass */
    cJSON *wifi_arr = cJSON_GetObjectItem(root, "wifi_networks");
    if (cJSON_IsArray(wifi_arr)) {
        int count = cJSON_GetArraySize(wifi_arr);
        if (count > 3) count = 3;
        for (int i = 0; i < count; i++) {
            cJSON *net = cJSON_GetArrayItem(wifi_arr, i);
            if (!cJSON_IsObject(net)) continue;

            cJSON *ssid_item = cJSON_GetObjectItem(net, "ssid");
            cJSON *pass_item = cJSON_GetObjectItem(net, "pass");

            if (cJSON_IsString(ssid_item)) {
                if (strlen(ssid_item->valuestring) >= 32) {
                    free(cfg);
                    cJSON_Delete(root);
                    return send_400(req, "SSID too long (max 31 chars)");
                }
                if (cJSON_IsString(pass_item) && strlen(pass_item->valuestring) >= 64) {
                    free(cfg);
                    cJSON_Delete(root);
                    return send_400(req, "WiFi password too long (max 63 chars)");
                }

                if (ssid_item->valuestring[0] == '\0') {
                    memset(&cfg->wifi_networks[i], 0, sizeof(wifi_network_t));
                } else {
                    bool ssid_changed = strcmp(cfg->wifi_networks[i].ssid,
                                               ssid_item->valuestring) != 0;
                    strncpy(cfg->wifi_networks[i].ssid, ssid_item->valuestring,
                            sizeof(cfg->wifi_networks[i].ssid) - 1);
                    cfg->wifi_networks[i].ssid[sizeof(cfg->wifi_networks[i].ssid) - 1] = '\0';

                    if (cJSON_IsString(pass_item) && pass_item->valuestring[0] != '\0' &&
                        strcmp(pass_item->valuestring, "********") != 0) {
                        memset(cfg->wifi_networks[i].password, 0,
                               sizeof(cfg->wifi_networks[i].password));
                        strncpy(cfg->wifi_networks[i].password, pass_item->valuestring,
                                sizeof(cfg->wifi_networks[i].password) - 1);
                    } else if (cJSON_IsString(pass_item) &&
                               strcmp(pass_item->valuestring, "********") == 0) {
                        /* Sentinel: preserve existing password. */
                    } else if (ssid_changed) {
                        memset(cfg->wifi_networks[i].password, 0,
                               sizeof(cfg->wifi_networks[i].password));
                    }
                }
            }
        }
        ESP_LOGI(TAG, "WiFi networks updated via wifi_networks array");
    } else {
        /* Legacy single ssid/pass → maps to wifi_networks[0] */
        cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
        cJSON *pass_item = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(ssid_item) && ssid_item->valuestring[0] != '\0') {
            if (strlen(ssid_item->valuestring) >= 32) {
                free(cfg);
                cJSON_Delete(root);
                return send_400(req, "SSID too long (max 31 chars)");
            }
            if (cJSON_IsString(pass_item) && strlen(pass_item->valuestring) >= 64) {
                free(cfg);
                cJSON_Delete(root);
                return send_400(req, "WiFi password too long (max 63 chars)");
            }
            strncpy(cfg->wifi_networks[0].ssid, ssid_item->valuestring,
                    sizeof(cfg->wifi_networks[0].ssid) - 1);
            cfg->wifi_networks[0].ssid[sizeof(cfg->wifi_networks[0].ssid) - 1] = '\0';
            if (cJSON_IsString(pass_item) && pass_item->valuestring[0] != '\0' &&
                strcmp(pass_item->valuestring, "********") != 0) {
                memset(cfg->wifi_networks[0].password, 0,
                       sizeof(cfg->wifi_networks[0].password));
                strncpy(cfg->wifi_networks[0].password, pass_item->valuestring,
                        sizeof(cfg->wifi_networks[0].password) - 1);
            }
            ESP_LOGI(TAG, "WiFi credentials updated via legacy ssid/pass");
        }
    }

    /* Explicit false is a deliberate API/test request to re-arm the hint (the web UI never sends this key); absent key or true keeps the retire-on-first-save behavior. */
    cJSON *sh_item = cJSON_GetObjectItem(root, "setup_hint_dismissed");
    bool setup_hint_explicit_clear = cJSON_IsBool(sh_item) && !cJSON_IsTrue(sh_item);
    if (setup_hint_explicit_clear) {
        cfg->setup_hint_dismissed = false;
    }

    cJSON_Delete(root);

    app_config_t *old_cfg = config_snapshot_for_request(req);
    if (!old_cfg) {
        ESP_LOGE(TAG, "config_post: PSRAM alloc failed for old_cfg");
        free(cfg);
        return ESP_OK;   /* 500 already sent */
    }

    /* A settings save proves the user found the web config UI, which is the
     * only thing the first-boot hint overlay exists to teach. Retire it here so
     * it persists inside this same NVS write (no second save). */
    bool retire_setup_hint = !cfg->setup_hint_dismissed && !setup_hint_explicit_clear;
    if (retire_setup_hint) {
        cfg->setup_hint_dismissed = true;
    }

    app_config_save(cfg);   /* applies app_config_normalize_nav_exclusivity() internally */
    config_trigger_side_effects(old_cfg, cfg);

    if (retire_setup_hint) {
        nina_setup_hint_destroy();   /* no-op when the overlay is not showing */
        ESP_LOGI(TAG, "Setup hint retired by web config save");
    }

    /* Home Page live (Q7): if active_page_override changed, navigate there now.
     * The field is a page_ref registry id; page_ref_navigate() resolves the id,
     * sets the image-source override for image-source ids (which the old direct
     * page-index claim did not), and issues the USER-claim navigation. */
    if (cfg->active_page_override != old_cfg->active_page_override) {
        page_ref_navigate((page_ref_t)cfg->active_page_override);
    }

    free(old_cfg);
    free(cfg);

    ESP_LOGI(TAG, "Config saved to NVS");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live-applying config (in-memory only, no NVS)
esp_err_t config_apply_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = receive_json_body(req, CONFIG_MAX_PAYLOAD);
    if (!root) return ESP_OK;

    if (!validate_config_fields(root, req)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    app_config_t *cfg = parse_config_from_json(root);
    cJSON_Delete(root);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    app_config_t *old_cfg = config_snapshot_for_request(req);
    if (!old_cfg) {
        ESP_LOGE(TAG, "config_apply: PSRAM alloc failed for old_cfg");
        free(cfg);
        return ESP_OK;   /* 500 already sent */
    }
    app_config_apply(cfg);
    config_trigger_side_effects(old_cfg, cfg);
    free(old_cfg);
    free(cfg);

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for reverting config to NVS-saved state
esp_err_t config_revert_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    app_config_t *old_cfg = config_snapshot_for_request(req);
    if (!old_cfg) {
        ESP_LOGE(TAG, "config_revert: PSRAM alloc failed for old_cfg");
        return ESP_OK;   /* 500 already sent */
    }

    esp_err_t err = app_config_revert();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config revert failed");
        free(old_cfg);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    config_trigger_side_effects(old_cfg, app_config_get());
    free(old_cfg);

    ESP_LOGI(TAG, "Config reverted from NVS");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t backup_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    /* Check include_sensitive query param */
    bool include_sensitive = false;
    {
        char qbuf[32] = {0};
        if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
            char val[4] = {0};
            if (httpd_query_key_value(qbuf, "include_sensitive", val, sizeof(val)) == ESP_OK) {
                include_sensitive = (val[0] == '1');
            }
        }
    }

    /* Safety rail: if authentication is disabled, secrets must NEVER be
     * returned in any API response — including this backup endpoint. A caller
     * that explicitly requests include_sensitive=1 on an open device still
     * gets a redacted backup. */
    app_config_t *cfg = app_config_get();
    if (!cfg->auth_enabled && include_sensitive) {
        ESP_LOGI(TAG, "backup: include_sensitive forced off (auth disabled)");
        include_sensitive = false;
    }

    /* Build root JSON */
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* ---- Meta section ---- */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "config_version", APP_CONFIG_VERSION);
    cJSON_AddStringToObject(meta, "firmware_version", BUILD_GIT_TAG);
    cJSON_AddStringToObject(meta, "git_sha", BUILD_GIT_SHA);
    cJSON_AddStringToObject(meta, "hostname", cfg->hostname);

    /* MAC address */
    uint8_t mac[6];
    char mac_str[18];   /* "AA:BB:CC:DD:EE:FF" + null */
    char mac_file[13];  /* "AABBCCDDEEFF" + null (for filename) */
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(mac_file, sizeof(mac_file), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strcpy(mac_str, "00:00:00:00:00:00");
        strcpy(mac_file, "000000000000");
    }
    cJSON_AddStringToObject(meta, "mac_address", mac_str);

    /* Export date (ISO-8601 UTC) */
    time_t now;
    time(&now);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    cJSON_AddStringToObject(meta, "export_date", date_str);

    cJSON_AddItemToObject(root, "meta", meta);

    /* ---- Build full config JSON, then split into config + sensitive sections ---- */
    cJSON *full_config = serialize_config_to_json(cfg);
    if (!full_config) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* The JSON Display / Home Assistant page config never reaches this object on
     * its own: the tile layouts live in their own NVS keys (and must stay off
     * this payload -- it also backs GET /api/config, deliberately slimmed by
     * moving those ~6 KB blobs out), and the eight scalars are not SETTINGS_TABLE
     * rows. Inject all ten here so the s_backup_fields split below routes them
     * for us -- json_auth_header and ha_token into sensitive_section, the rest
     * into config_section. Everything except those two carries no credential, so
     * it is exported regardless of include_sensitive. */
    cJSON_AddBoolToObject(full_config,   "json_enabled",           cfg->json_enabled);
    cJSON_AddStringToObject(full_config, "json_url",               cfg->json_url);
    cJSON_AddStringToObject(full_config, "json_auth_header",       cfg->json_auth_header);
    cJSON_AddNumberToObject(full_config, "json_update_interval_s", cfg->json_update_interval_s);
    cJSON_AddStringToObject(full_config, "json_tiles_config",      app_config_get_json_tiles());
    cJSON_AddBoolToObject(full_config,   "ha_enabled",             cfg->ha_enabled);
    cJSON_AddStringToObject(full_config, "ha_base_url",            cfg->ha_base_url);
    cJSON_AddStringToObject(full_config, "ha_token",               cfg->ha_token);
    cJSON_AddNumberToObject(full_config, "ha_update_interval_s",   cfg->ha_update_interval_s);
    cJSON_AddStringToObject(full_config, "ha_tiles_config",        app_config_get_ha_tiles());

    /* Split into config and sensitive sections */
    cJSON *config_section = cJSON_CreateObject();
    cJSON *sensitive_section = include_sensitive ? cJSON_CreateObject() : NULL;

    for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
        cJSON *val = cJSON_GetObjectItem(full_config, f->json_key);
        if (!val) continue;

        if (f->is_sensitive) {
            if (sensitive_section) {
                cJSON_AddItemToObject(sensitive_section, f->json_key, cJSON_Duplicate(val, true));
            }
        } else {
            cJSON_AddItemToObject(config_section, f->json_key, cJSON_Duplicate(val, true));
        }
    }

    /* Inject fields that serialize_config_to_json() does NOT emit (admin_password,
     * wifi_networks array, wifi_password legacy). Without these, a restore from
     * a "full" backup would wipe device credentials. Only included when
     * include_sensitive=1 (matches the existing sensitive-section gating). */
    if (sensitive_section) {
        cJSON_AddStringToObject(sensitive_section, "admin_password", cfg->admin_password);
        /* mqtt_password, spotify_client_id, weather_api_key already emitted by
         * serialize_config_to_json() and routed into sensitive_section by the
         * s_backup_fields registry above. */

        /* wifi_networks: full array with real ssid + password per entry */
        cJSON *wifi_arr = cJSON_CreateArray();
        for (int i = 0; i < 3; i++) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", cfg->wifi_networks[i].ssid);
            cJSON_AddStringToObject(net, "password", cfg->wifi_networks[i].password);
            /* Mirror as "pass" too — config_post handler reads "pass" key */
            cJSON_AddStringToObject(net, "pass", cfg->wifi_networks[i].password);
            cJSON_AddItemToArray(wifi_arr, net);
        }
        cJSON_AddItemToObject(sensitive_section, "wifi_networks", wifi_arr);

        /* Legacy top-level wifi_password = wifi_networks[0].password */
        cJSON_AddStringToObject(sensitive_section, "wifi_password",
                                cfg->wifi_networks[0].password);
    }

    cJSON_AddItemToObject(root, "config", config_section);
    if (sensitive_section) {
        cJSON_AddItemToObject(root, "sensitive", sensitive_section);
    }

    cJSON_Delete(full_config);

    /* Serialize and send */
    const char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* Build filename: {hostname}_{MAC}_v{version}_{date}.json */
    char filename[128];
    /* Sanitize hostname for filename (alphanumeric + hyphens only) */
    char safe_host[33];
    {
        const char *src = cfg->hostname;
        int j = 0;
        for (int i = 0; src[i] && j < 32; i++) {
            char c = src[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-') {
                safe_host[j++] = c;
            }
        }
        if (j == 0) { safe_host[j++] = 'D'; safe_host[j++] = 'E'; safe_host[j++] = 'V'; }
        safe_host[j] = '\0';
    }
    char date_short[16];
    strftime(date_short, sizeof(date_short), "%Y-%m-%d", &timeinfo);
    snprintf(filename, sizeof(filename),
             "attachment; filename=\"%s_%s_v%d_%s%s.json\"",
             safe_host, mac_file, APP_CONFIG_VERSION, date_short,
             include_sensitive ? "_secrets" : "");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", filename);
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free((void *)json_str);
    return ESP_OK;
}

esp_err_t restore_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = receive_json_body(req, CONFIG_MAX_RESTORE_PAYLOAD);
    if (!root) return ESP_OK;  /* error already sent */

    /* Validate structure */
    cJSON *backup = cJSON_GetObjectItem(root, "backup");
    cJSON *confirm_item = cJSON_GetObjectItem(root, "confirm");
    if (!backup || !cJSON_IsObject(backup)) {
        cJSON_Delete(root);
        return send_400(req, "Missing 'backup' object in request");
    }

    cJSON *meta = cJSON_GetObjectItem(backup, "meta");
    cJSON *backup_config = cJSON_GetObjectItem(backup, "config");
    if (!meta || !backup_config) {
        cJSON_Delete(root);
        return send_400(req, "Invalid backup file: missing 'meta' or 'config' section");
    }

    cJSON *ver = cJSON_GetObjectItem(meta, "config_version");
    if (!cJSON_IsNumber(ver)) {
        cJSON_Delete(root);
        return send_400(req, "Invalid backup file: no config_version in metadata");
    }

    bool do_confirm = cJSON_IsTrue(confirm_item);

    if (!do_confirm) {
        /* ---- Preview mode ---- */
        /* Serialize current config to JSON for comparison */
        cJSON *current_json = serialize_config_to_json(app_config_get());
        if (!current_json) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_OK;
        }
        /* Mirror the injection in backup_get_handler so the JSON Display / HA
         * fields have a current value to diff against; without it every restore
         * would report all ten as changing from (empty). */
        {
            const app_config_t *live = app_config_get();
            cJSON_AddBoolToObject(current_json,   "json_enabled",           live->json_enabled);
            cJSON_AddStringToObject(current_json, "json_url",               live->json_url);
            cJSON_AddStringToObject(current_json, "json_auth_header",       live->json_auth_header);
            cJSON_AddNumberToObject(current_json, "json_update_interval_s", live->json_update_interval_s);
            cJSON_AddStringToObject(current_json, "json_tiles_config",      app_config_get_json_tiles());
            cJSON_AddBoolToObject(current_json,   "ha_enabled",             live->ha_enabled);
            cJSON_AddStringToObject(current_json, "ha_base_url",            live->ha_base_url);
            cJSON_AddStringToObject(current_json, "ha_token",               live->ha_token);
            cJSON_AddNumberToObject(current_json, "ha_update_interval_s",   live->ha_update_interval_s);
            cJSON_AddStringToObject(current_json, "ha_tiles_config",        app_config_get_ha_tiles());
        }

        cJSON *preview = build_restore_preview(backup, current_json);
        cJSON_Delete(current_json);
        cJSON_Delete(root);

        if (!preview) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        return send_json_response(req, preview);

    } else {
        /* ---- Confirm mode: apply the backup ---- */
        /* Merge backup config + sensitive into a single JSON object */
        cJSON *merged = cJSON_Duplicate(backup_config, true);
        if (!merged) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        /* Overlay sensitive fields if present */
        cJSON *backup_sensitive = cJSON_GetObjectItem(backup, "sensitive");
        if (backup_sensitive) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, backup_sensitive) {
                /* Only overlay fields we recognize */
                bool known = false;
                for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
                    if (strcmp(f->json_key, item->string) == 0) { known = true; break; }
                }
                /* Also allow credential fields not in the registry (admin_password,
                 * wifi_networks array, wifi_password legacy compat). */
                if (!known && item->string &&
                    (strcmp(item->string, "admin_password") == 0 ||
                     strcmp(item->string, "wifi_networks") == 0 ||
                     strcmp(item->string, "wifi_password") == 0)) {
                    known = true;
                }
                if (known) {
                    cJSON_DeleteItemFromObject(merged, item->string);
                    cJSON_AddItemToObject(merged, item->string, cJSON_Duplicate(item, true));
                }
            }
        }

        /* Validate merged fields */
        if (!validate_config_fields(merged, req)) {
            cJSON_Delete(merged);
            cJSON_Delete(root);
            return ESP_OK;
        }

        /* Parse into config struct using existing parse_config_from_json */
        app_config_t *new_cfg = parse_config_from_json(merged);

        if (!new_cfg) {
            cJSON_Delete(merged);
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        /* Pre-v61 backup: only the retired image_display_* keys are present.
         * Derive the thirteen per-page image fields from them so the restored
         * device keeps its image pages (same derivation the NVS loader runs). */
        if (!cJSON_HasObjectItem(merged, "goes_enabled") &&
            cJSON_HasObjectItem(merged, "image_display_enabled")) {
            image_pages_derive_from_legacy(new_cfg);
        }

        /* parse_config_from_json() does not handle admin_password or the
         * wifi_networks[] array. Apply them here from the merged backup. */
        {
            cJSON *ap = cJSON_GetObjectItem(merged, "admin_password");
            if (cJSON_IsString(ap) && ap->valuestring[0] != '\0' &&
                strcmp(ap->valuestring, "********") != 0) {
                strncpy(new_cfg->admin_password, ap->valuestring,
                        sizeof(new_cfg->admin_password) - 1);
                new_cfg->admin_password[sizeof(new_cfg->admin_password) - 1] = '\0';
            }

            cJSON *warr = cJSON_GetObjectItem(merged, "wifi_networks");
            if (cJSON_IsArray(warr)) {
                int count = cJSON_GetArraySize(warr);
                if (count > 3) count = 3;
                for (int i = 0; i < count; i++) {
                    cJSON *net = cJSON_GetArrayItem(warr, i);
                    if (!cJSON_IsObject(net)) continue;
                    cJSON *sid = cJSON_GetObjectItem(net, "ssid");
                    /* Accept either "password" (backup format) or "pass" (legacy). */
                    cJSON *pwd = cJSON_GetObjectItem(net, "password");
                    if (!cJSON_IsString(pwd)) pwd = cJSON_GetObjectItem(net, "pass");

                    if (cJSON_IsString(sid)) {
                        if (sid->valuestring[0] == '\0') {
                            memset(&new_cfg->wifi_networks[i], 0,
                                   sizeof(wifi_network_t));
                        } else {
                            strncpy(new_cfg->wifi_networks[i].ssid,
                                    sid->valuestring,
                                    sizeof(new_cfg->wifi_networks[i].ssid) - 1);
                            new_cfg->wifi_networks[i].ssid[
                                sizeof(new_cfg->wifi_networks[i].ssid) - 1] = '\0';
                            if (cJSON_IsString(pwd) &&
                                strcmp(pwd->valuestring, "********") != 0) {
                                memset(new_cfg->wifi_networks[i].password, 0,
                                       sizeof(new_cfg->wifi_networks[i].password));
                                strncpy(new_cfg->wifi_networks[i].password,
                                        pwd->valuestring,
                                        sizeof(new_cfg->wifi_networks[i].password) - 1);
                            }
                        }
                    }
                }
            } else {
                /* Legacy top-level wifi_password (maps to wifi_networks[0]) */
                cJSON *wp = cJSON_GetObjectItem(merged, "wifi_password");
                if (cJSON_IsString(wp) && wp->valuestring[0] != '\0' &&
                    strcmp(wp->valuestring, "********") != 0) {
                    memset(new_cfg->wifi_networks[0].password, 0,
                           sizeof(new_cfg->wifi_networks[0].password));
                    strncpy(new_cfg->wifi_networks[0].password, wp->valuestring,
                            sizeof(new_cfg->wifi_networks[0].password) - 1);
                }
            }
        }

        /* JSON Display + Home Assistant page config. parse_config_from_json()
         * carries none of it: the eight scalars are not SETTINGS_TABLE rows, and
         * the tile layouts are not even in app_config_t. Apply the scalars onto
         * new_cfg (picked up by the app_config_save() below, whose
         * validate_config() clamps both intervals to 5..300 and NUL-terminates
         * the char arrays), and persist the layouts through their own setters,
         * which clamp to MAX-1 and refresh the getter cache.
         *
         * An absent key leaves the current value untouched, so a backup taken
         * before these fields existed neither blanks a URL nor drops a token.
         * strip_masked_secrets() (run inside parse_config_from_json above) has
         * already deleted any "********" value, so a redacted backup cannot
         * overwrite a live credential with literal asterisks.
         *
         * Change detection compares against the LIVE config, which is still
         * pre-restore at this point; the tiles comparison must happen BEFORE the
         * setter overwrites the cache. Each page's live-apply spine runs once,
         * after the save, only if something on that page actually moved. */
        bool json_page_changed = false;
        bool ha_page_changed = false;
        {
            const app_config_t *live = app_config_get();

            cJSON *it = cJSON_GetObjectItem(merged, "json_enabled");
            if (cJSON_IsBool(it)) new_cfg->json_enabled = cJSON_IsTrue(it);
            it = cJSON_GetObjectItem(merged, "json_url");
            if (cJSON_IsString(it)) strlcpy(new_cfg->json_url, it->valuestring, sizeof(new_cfg->json_url));
            it = cJSON_GetObjectItem(merged, "json_auth_header");
            /* A "********" value was already deleted by strip_masked_secrets()
             * inside parse_config_from_json above, so key-present means a real
             * new credential; a redacted backup simply leaves the live one. */
            if (cJSON_IsString(it)) {
                strlcpy(new_cfg->json_auth_header, it->valuestring, sizeof(new_cfg->json_auth_header));
            }
            it = cJSON_GetObjectItem(merged, "json_update_interval_s");
            if (cJSON_IsNumber(it)) new_cfg->json_update_interval_s = (uint16_t)it->valueint;

            it = cJSON_GetObjectItem(merged, "ha_enabled");
            if (cJSON_IsBool(it)) new_cfg->ha_enabled = cJSON_IsTrue(it);
            it = cJSON_GetObjectItem(merged, "ha_base_url");
            if (cJSON_IsString(it)) strlcpy(new_cfg->ha_base_url, it->valuestring, sizeof(new_cfg->ha_base_url));
            it = cJSON_GetObjectItem(merged, "ha_token");
            if (cJSON_IsString(it)) {   /* sentinel already stripped -- see above */
                strlcpy(new_cfg->ha_token, it->valuestring, sizeof(new_cfg->ha_token));
            }
            it = cJSON_GetObjectItem(merged, "ha_update_interval_s");
            if (cJSON_IsNumber(it)) new_cfg->ha_update_interval_s = (uint16_t)it->valueint;

            json_page_changed =
                (new_cfg->json_enabled != live->json_enabled) ||
                (new_cfg->json_update_interval_s != live->json_update_interval_s) ||
                (strcmp(new_cfg->json_url, live->json_url) != 0) ||
                (strcmp(new_cfg->json_auth_header, live->json_auth_header) != 0);
            ha_page_changed =
                (new_cfg->ha_enabled != live->ha_enabled) ||
                (new_cfg->ha_update_interval_s != live->ha_update_interval_s) ||
                (strcmp(new_cfg->ha_base_url, live->ha_base_url) != 0) ||
                (strcmp(new_cfg->ha_token, live->ha_token) != 0);

            cJSON *jt = cJSON_GetObjectItem(merged, "json_tiles_config");
            if (cJSON_IsString(jt) && jt->valuestring) {
                if (strcmp(jt->valuestring, app_config_get_json_tiles()) != 0) {
                    json_page_changed = true;
                }
                esp_err_t te = app_config_set_json_tiles(jt->valuestring);
                if (te != ESP_OK) ESP_LOGW(TAG, "restore: json tiles persist failed: %s", esp_err_to_name(te));
            }
            cJSON *ht = cJSON_GetObjectItem(merged, "ha_tiles_config");
            if (cJSON_IsString(ht) && ht->valuestring) {
                if (strcmp(ht->valuestring, app_config_get_ha_tiles()) != 0) {
                    ha_page_changed = true;
                }
                esp_err_t te = app_config_set_ha_tiles(ht->valuestring);
                if (te != ESP_OK) ESP_LOGW(TAG, "restore: ha tiles persist failed: %s", esp_err_to_name(te));
            }
        }

        cJSON_Delete(merged);
        cJSON_Delete(root);

        /* Save to NVS and trigger side effects */
        app_config_t *old_cfg = config_snapshot_for_request(req);
        if (!old_cfg) {
            free(new_cfg);
            return ESP_OK;   /* 500 already sent */
        }

        app_config_save(new_cfg);
        config_trigger_side_effects(old_cfg, new_cfg);
        free(old_cfg);
        free(new_cfg);

        /* Bring each tile page up to the restored config: rebuild the widget
         * tree, show/hide per the restored enable flag, re-resolve navigation,
         * and start the poll task if the restore just enabled the page. Run only
         * for a page whose scalars or layout actually moved -- a rebuild is not
         * free, and most restores touch neither. Read the enable flag back from
         * the saved config so it reflects validate_config()'s canonicalization. */
        if (json_page_changed) {
            json_page_apply_live(app_config_get()->json_enabled);
        }
        if (ha_page_changed) {
            ha_page_apply_live(app_config_get()->ha_enabled);
        }

        /* Send success response with change count */
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "applied");
        /* Note: total_changes is approximate — config may have changed between
         * preview and confirm. The UI should not assert on matching counts. */
        cJSON_AddNumberToObject(resp, "total_changes", 0);
        cJSON_AddItemToObject(resp, "validation_notes", cJSON_CreateArray());
        return send_json_response(req, resp);
    }
}

/* ---- Device-to-device config clone (POST /api/config/pull) ---- */

/* Upstream backup cap. A backup with both tiles blobs runs ~30 KB; 64 KB leaves
 * room for growth while bounding what one httpd worker will buffer in PSRAM. */
#define PULL_MAX_RESPONSE  65536
/* The source device may be on a slow power-save link (20-50 KB/s measured), so
 * allow a generous window for a ~30 KB body before giving up. */
#define PULL_TIMEOUT_MS    15000

/* Send {"error":"<code>"} (plus "status" when @p status is non-zero) as a 200.
 * The verdict lives in the body so the browser gets one uniform shape to switch
 * on; transport-vs-upstream detail never leaks into the HTTP status of OUR
 * response. Returns ESP_OK so handlers can `return pull_send_error(...)`. */
static esp_err_t pull_send_error(httpd_req_t *req, const char *code, int status)
{
    char body[96];
    if (status != 0) {
        snprintf(body, sizeof(body), "{\"error\":\"%s\",\"status\":%d}", code, status);
    } else {
        snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief POST /api/config/pull  -- fetch another device's backup through this
 *        device and forward it, so one panel can be cloned from another.
 *
 * Body: {"host":"<hostname or ip[:port]>","password":"<source admin password>",
 *        "sensitive":true|false}
 *
 * The device performs GET http://<host>/api/config/backup?include_sensitive=<0|1>
 * with an X-Auth-Password header (check_session() on the source accepts that
 * header as an alternative to a session cookie, and feeds it through the same
 * login lockout). Doing the fetch here rather than in the browser avoids a CORS
 * problem and means the source password never has to survive a cross-origin
 * request; it is used once, never logged, and never echoed.
 *
 * On success the upstream body is forwarded VERBATIM (it is already exactly the
 * backup JSON that POST /api/config/restore consumes). Failures come back as a
 * 200 carrying {"error":...} -- see pull_send_error.
 */
esp_err_t config_pull_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = receive_json_body(req, CONFIG_MAX_PAYLOAD);
    if (!root) return ESP_OK;  /* error already sent */

    cJSON *host_item = cJSON_GetObjectItem(root, "host");
    cJSON *pw_item   = cJSON_GetObjectItem(root, "password");
    bool include_sensitive = cJSON_IsTrue(cJSON_GetObjectItem(root, "sensitive"));

    if (!cJSON_IsString(host_item) || host_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_400(req, "Missing 'host'");
    }

    /* Host hygiene. Only a bare host[:port] is accepted; the scheme is supplied
     * by us, so a caller cannot redirect the fetch elsewhere by smuggling one
     * in. A single leading "http://" is tolerated (users paste URLs) and
     * stripped BEFORE the checks, so "https://x" and "host/path" both fail the
     * '/' test, and "user@host" fails the '@' test. Whitespace and control
     * characters are rejected outright -- they cannot appear in a hostname and
     * could otherwise split the request line. */
    char host[128];
    {
        const char *h = host_item->valuestring;
        if (strncasecmp(h, "http://", 7) == 0) h += 7;
        if (h[0] == '\0' || strlen(h) >= sizeof(host)) {
            cJSON_Delete(root);
            return pull_send_error(req, "bad_host", 0);
        }
        for (const char *p = h; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '/' || c == '@' || c == '?' || c == '#' || c == '\\' ||
                c <= ' ' || c == 0x7f) {
                cJSON_Delete(root);
                return pull_send_error(req, "bad_host", 0);
            }
        }
        strlcpy(host, h, sizeof(host));
    }

    /* Password: optional (the source may have auth disabled). Capped at the
     * admin_password capacity -- check_session() rejects anything longer, so a
     * longer value could never authenticate anyway. */
    char auth_hdr[64];
    auth_hdr[0] = '\0';
    if (cJSON_IsString(pw_item) && pw_item->valuestring[0] != '\0') {
        if (strlen(pw_item->valuestring) >= sizeof(((app_config_t *)0)->admin_password)) {
            cJSON_Delete(root);
            return pull_send_error(req, "auth", 0);
        }
        /* auth_hdr[64] covers "X-Auth-Password: " (17) + password (<=32). */
        snprintf(auth_hdr, sizeof(auth_hdr), "X-Auth-Password: %s", pw_item->valuestring);
    }

    /* Wipe the password out of the cJSON string before freeing it, so the only
     * remaining copy is auth_hdr (a local, zeroed right after the fetch). cJSON
     * frees without clearing, and the PSRAM it releases is reused by the next
     * allocation -- receive_json_body() scrubs the raw request buffer for the
     * same reason. */
    if (cJSON_IsString(pw_item) && pw_item->valuestring) {
        memset(pw_item->valuestring, 0, strlen(pw_item->valuestring));
    }
    cJSON_Delete(root);   /* password is now only in auth_hdr, a local */

    /* url[192] covers "http://" + host[<=127] + the fixed query string (45). */
    char url[192];
    snprintf(url, sizeof(url), "http://%s/api/config/backup?include_sensitive=%d",
             host, include_sensitive ? 1 : 0);

    int status = 0;
    http_fetch_opts_t opts = {
        .timeout_ms         = PULL_TIMEOUT_MS,
        .max_redirects      = 0,   /* a peer device never redirects; refuse to chase one */
        .max_attempts       = 1,
        .max_response_bytes = PULL_MAX_RESPONSE,
        .extra_header       = (auth_hdr[0] != '\0') ? auth_hdr : NULL,
        .status_out         = &status,
    };

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_fetch_text(url, &opts, &body, &body_len);

    /* Scrub the password from the stack copy as soon as the fetch is done. */
    memset(auth_hdr, 0, sizeof(auth_hdr));

    if (err != ESP_OK) {
        /* status 0 == no HTTP response at all (DNS/connect/timeout). Never log
         * the password or the header; host and status are safe. */
        ESP_LOGW(TAG, "config pull: %s failed (status=%d, %s)",
                 host, status, esp_err_to_name(err));
        if (status == 0) {
            return pull_send_error(req, "unreachable", 0);
        }
        if (status == 401 || status == 403) {
            return pull_send_error(req, "auth", status);
        }
        return pull_send_error(req, "bad_response", status);
    }

    /* A 200 is not proof we reached a NINA display: a captive portal or a login
     * page also answers 200. Require the body to parse and to carry the "meta"
     * object every backup has, so the browser is never handed something the
     * restore endpoint will reject. Parse-and-discard: the raw text is what gets
     * forwarded, since it is already the exact restore input. */
    cJSON *probe = cJSON_Parse(body);
    bool looks_like_backup = (probe != NULL && cJSON_IsObject(cJSON_GetObjectItem(probe, "meta")));
    if (probe) cJSON_Delete(probe);
    if (!looks_like_backup) {
        ESP_LOGW(TAG, "config pull: %s returned a non-backup body", host);
        heap_caps_free(body);
        return pull_send_error(req, "bad_response", status);
    }

    ESP_LOGI(TAG, "config pull: %s ok (%u bytes, secrets=%d)",
             host, (unsigned)body_len, include_sensitive ? 1 : 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, body_len);
    heap_caps_free(body);
    return ESP_OK;
}
