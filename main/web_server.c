#include "web_server.h"
#include "app_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";

static const char *HTML_CONTENT =
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><title>Device Config</title>"
"<style>"
":root{--bg-color:#121212;--surface-color:#1e1e1e;--primary-color:#bb86fc;--primary-variant:#3700b3;--secondary-color:#03dac6;--error-color:#cf6679;--text-primary:#ffffff;--text-secondary:#b0bec5;--border-color:#333;}"
"body{font-family:'Roboto',Arial,sans-serif;background-color:var(--bg-color);color:var(--text-primary);margin:0;padding:20px;display:flex;justify-content:center;}"
".container{width:100%;max-width:600px;}"
"h2{color:var(--text-primary);text-align:center;margin-bottom:24px;}"
"h3{color:var(--secondary-color);margin-top:0;font-size:1.1rem;border-bottom:1px solid var(--border-color);padding-bottom:10px;}"
".card{background-color:var(--surface-color);border-radius:8px;padding:16px;margin-bottom:16px;box-shadow:0 4px 6px rgba(0,0,0,0.3);}"
".group{margin-bottom:16px;}"
"label{display:block;margin-bottom:8px;color:var(--text-secondary);font-size:0.9rem;}"
"input[type='text'],input[type='password']{width:100%;padding:12px;background-color:#2c2c2c;border:1px solid var(--border-color);border-radius:4px;color:var(--text-primary);box-sizing:border-box;font-size:1rem;transition:border-color 0.3s;}"
"input[type='text']:focus,input[type='password']:focus{outline:none;border-color:var(--primary-color);}"
".btn{width:100%;padding:12px;border:none;border-radius:4px;font-size:1rem;font-weight:bold;cursor:pointer;text-transform:uppercase;letter-spacing:1px;transition:opacity 0.3s;}"
".btn-primary{background-color:var(--primary-color);color:#000;}"
".btn-danger{background-color:var(--error-color);color:#000;margin-top:16px;}"
".btn:hover{opacity:0.9;}"
".filter-row{display:flex;align-items:center;justify-content:space-between;padding:8px 0;border-bottom:1px solid #333;}"
".filter-row:last-child{border-bottom:none;}"
".filter-name{color:var(--text-primary);}"
".color-input{width:60px;height:40px;border:none;border-radius:4px;cursor:pointer;background:none;padding:0;}"
"p{color:var(--text-secondary);}"
"</style>"
"</head><body><div class='container'><h2>Device Configuration</h2>"
"<div class='card'><h3>Network Settings</h3>"
"<div class='group'><label>WiFi SSID</label><input type='text' id='ssid' placeholder='Enter SSID'></div>"
"<div class='group'><label>WiFi Password</label><input type='password' id='pass' placeholder='Enter Password'></div>"
"<div class='group'><label>NTP Server</label><input type='text' id='ntp' placeholder='pool.ntp.org'></div></div>"
"<div class='card'><h3>NINA API</h3>"
"<div class='group'><label>NINA API URL 1</label><input type='text' id='url1' placeholder='http://...'></div>"
"<div class='group'><label>NINA API URL 2</label><input type='text' id='url2' placeholder='http://...'></div></div>"
"<div class='card'><h3>Filter Colors</h3>"
"<p style='font-size:0.9rem;margin-bottom:16px'>Configure colors for each filter. The exposure arc will be colored based on the active filter.</p>"
"<div id='filterColors'></div></div>"
"<div class='card'><h3>Guiding RMS Thresholds</h3>"
"<p style='font-size:0.9rem;margin-bottom:16px'>Set value thresholds and colors for Good, OK, and Bad guiding RMS levels (arcseconds).</p>"
"<div class='group'><label>Good (max value)</label><div style='display:flex;gap:8px;align-items:center'>"
"<input type='text' id='rms_good_max' style='flex:1' placeholder='0.5'>"
"<input type='color' class='color-input' id='rms_good_color' value='#10b981'></div></div>"
"<div class='group'><label>OK (max value)</label><div style='display:flex;gap:8px;align-items:center'>"
"<input type='text' id='rms_ok_max' style='flex:1' placeholder='1.0'>"
"<input type='color' class='color-input' id='rms_ok_color' value='#eab308'></div></div>"
"<div class='group'><label>Bad (above OK threshold)</label><div style='display:flex;gap:8px;align-items:center'>"
"<span style='flex:1;color:var(--text-secondary);font-size:0.9rem'>Values above OK threshold</span>"
"<input type='color' class='color-input' id='rms_bad_color' value='#ef4444'></div></div></div>"
"<div class='card'><h3>HFR Thresholds</h3>"
"<p style='font-size:0.9rem;margin-bottom:16px'>Set value thresholds and colors for Good, OK, and Bad HFR levels.</p>"
"<div class='group'><label>Good (max value)</label><div style='display:flex;gap:8px;align-items:center'>"
"<input type='text' id='hfr_good_max' style='flex:1' placeholder='2.0'>"
"<input type='color' class='color-input' id='hfr_good_color' value='#10b981'></div></div>"
"<div class='group'><label>OK (max value)</label><div style='display:flex;gap:8px;align-items:center'>"
"<input type='text' id='hfr_ok_max' style='flex:1' placeholder='3.5'>"
"<input type='color' class='color-input' id='hfr_ok_color' value='#eab308'></div></div>"
"<div class='group'><label>Bad (above OK threshold)</label><div style='display:flex;gap:8px;align-items:center'>"
"<span style='flex:1;color:var(--text-secondary);font-size:0.9rem'>Values above OK threshold</span>"
"<input type='color' class='color-input' id='hfr_bad_color' value='#ef4444'></div></div></div>"
"<button class='btn btn-primary' onclick='save()'>Save & Reboot</button>"
"<button class='btn btn-danger' onclick='factoryReset()'>Factory Reset</button></div>"
"<script>"
"let filterColorsObj={};"
"fetch('/api/config').then(r=>r.json()).then(d=>{"
"document.getElementById('ssid').value=d.ssid||'';"
"document.getElementById('pass').value=d.pass||'';"
"document.getElementById('url1').value=d.url1||'';"
"document.getElementById('url2').value=d.url2||'';"
"document.getElementById('ntp').value=d.ntp||'';"
"try{filterColorsObj=JSON.parse(d.filter_colors||'{}');}catch(e){filterColorsObj={};}"
"renderFilterColors();"
"try{var rms=JSON.parse(d.rms_thresholds||'{}');"
"document.getElementById('rms_good_max').value=rms.good_max||0.5;"
"document.getElementById('rms_ok_max').value=rms.ok_max||1.0;"
"document.getElementById('rms_good_color').value=rms.good_color||'#10b981';"
"document.getElementById('rms_ok_color').value=rms.ok_color||'#eab308';"
"document.getElementById('rms_bad_color').value=rms.bad_color||'#ef4444';"
"}catch(e){}"
"try{var hfr=JSON.parse(d.hfr_thresholds||'{}');"
"document.getElementById('hfr_good_max').value=hfr.good_max||2.0;"
"document.getElementById('hfr_ok_max').value=hfr.ok_max||3.5;"
"document.getElementById('hfr_good_color').value=hfr.good_color||'#10b981';"
"document.getElementById('hfr_ok_color').value=hfr.ok_color||'#eab308';"
"document.getElementById('hfr_bad_color').value=hfr.bad_color||'#ef4444';"
"}catch(e){}"
"});"
"function renderFilterColors(){"
"const container=document.getElementById('filterColors');"
"container.innerHTML='';"
"const filters=Object.keys(filterColorsObj).sort();"
"if(filters.length===0){container.innerHTML='<p style=\"text-align:center;font-style:italic\">No filters configured yet.</p>';return;}"
"filters.forEach(f=>{"
"const row=document.createElement('div');"
"row.className='filter-row';"
"row.innerHTML=`<span class='filter-name'>${f}</span><input type='color' class='color-input' value='${filterColorsObj[f]}' onchange='updateFilterColor(\"${f}\",this.value)'>`;"
"container.appendChild(row);"
"});"
"}"
"function updateFilterColor(name,color){filterColorsObj[name]=color;}"
"function save(){"
"const btn=document.querySelector('.btn-primary');const originalText=btn.innerText;btn.innerText='Saving...';btn.disabled=true;"
"const rmsT=JSON.stringify({good_max:parseFloat(document.getElementById('rms_good_max').value)||0.5,ok_max:parseFloat(document.getElementById('rms_ok_max').value)||1.0,good_color:document.getElementById('rms_good_color').value,ok_color:document.getElementById('rms_ok_color').value,bad_color:document.getElementById('rms_bad_color').value});"
"const hfrT=JSON.stringify({good_max:parseFloat(document.getElementById('hfr_good_max').value)||2.0,ok_max:parseFloat(document.getElementById('hfr_ok_max').value)||3.5,good_color:document.getElementById('hfr_good_color').value,ok_color:document.getElementById('hfr_ok_color').value,bad_color:document.getElementById('hfr_bad_color').value});"
"const data={ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value,url1:document.getElementById('url1').value,url2:document.getElementById('url2').value,ntp:document.getElementById('ntp').value,filter_colors:JSON.stringify(filterColorsObj),rms_thresholds:rmsT,hfr_thresholds:hfrT};"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})"
".then(r=>{if(r.ok){alert('Saved! Rebooting...');fetch('/api/reboot',{method:'POST'});}else{alert('Error saving');btn.innerText=originalText;btn.disabled=false;}}).catch(()=>{alert('Connection failed');btn.innerText=originalText;btn.disabled=false;});"
"}"
"function factoryReset(){"
"if(confirm('WARNING: This will erase ALL settings and reboot the device.\\n\\nAre you sure you want to continue?')){"
"if(confirm('This action cannot be undone!\\n\\nClick OK to proceed with factory reset.')){"
"fetch('/api/factory-reset',{method:'POST'})"
".then(r=>{if(r.ok){alert('Factory reset complete! Device will reboot now.');}else{alert('Error performing factory reset');}});"
"}}}"
"</script></body></html>";

// Handler for root URL
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_CONTENT, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for getting config
static esp_err_t config_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "pass", cfg->wifi_pass);
    cJSON_AddStringToObject(root, "url1", cfg->api_url_1);
    cJSON_AddStringToObject(root, "url2", cfg->api_url_2);
    cJSON_AddStringToObject(root, "ntp", cfg->ntp_server);
    cJSON_AddStringToObject(root, "filter_colors", cfg->filter_colors);
    cJSON_AddStringToObject(root, "rms_thresholds", cfg->rms_thresholds);
    cJSON_AddStringToObject(root, "hfr_thresholds", cfg->hfr_thresholds);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    
    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Handler for saving config
static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[2048];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    app_config_t cfg;
    // Load current to preserve anything not sent
    memcpy(&cfg, app_config_get(), sizeof(app_config_t));

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    if (cJSON_IsString(ssid)) {
        strncpy(cfg.wifi_ssid, ssid->valuestring, sizeof(cfg.wifi_ssid) - 1);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
    }

    cJSON *pass = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(pass)) {
        strncpy(cfg.wifi_pass, pass->valuestring, sizeof(cfg.wifi_pass) - 1);
        cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';
    }

    cJSON *url1 = cJSON_GetObjectItem(root, "url1");
    if (cJSON_IsString(url1)) {
        strncpy(cfg.api_url_1, url1->valuestring, sizeof(cfg.api_url_1) - 1);
        cfg.api_url_1[sizeof(cfg.api_url_1) - 1] = '\0';
    }

    cJSON *url2 = cJSON_GetObjectItem(root, "url2");
    if (cJSON_IsString(url2)) {
        strncpy(cfg.api_url_2, url2->valuestring, sizeof(cfg.api_url_2) - 1);
        cfg.api_url_2[sizeof(cfg.api_url_2) - 1] = '\0';
    }

    cJSON *ntp = cJSON_GetObjectItem(root, "ntp");
    if (cJSON_IsString(ntp)) {
        strncpy(cfg.ntp_server, ntp->valuestring, sizeof(cfg.ntp_server) - 1);
        cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = '\0';
    }

    cJSON *filter_colors = cJSON_GetObjectItem(root, "filter_colors");
    if (cJSON_IsString(filter_colors)) {
        strncpy(cfg.filter_colors, filter_colors->valuestring, sizeof(cfg.filter_colors) - 1);
        cfg.filter_colors[sizeof(cfg.filter_colors) - 1] = '\0';
    }

    cJSON *rms_thresholds = cJSON_GetObjectItem(root, "rms_thresholds");
    if (cJSON_IsString(rms_thresholds)) {
        strncpy(cfg.rms_thresholds, rms_thresholds->valuestring, sizeof(cfg.rms_thresholds) - 1);
        cfg.rms_thresholds[sizeof(cfg.rms_thresholds) - 1] = '\0';
    }

    cJSON *hfr_thresholds = cJSON_GetObjectItem(root, "hfr_thresholds");
    if (cJSON_IsString(hfr_thresholds)) {
        strncpy(cfg.hfr_thresholds, hfr_thresholds->valuestring, sizeof(cfg.hfr_thresholds) - 1);
        cfg.hfr_thresholds[sizeof(cfg.hfr_thresholds) - 1] = '\0';
    }

    app_config_save(&cfg);
    cJSON_Delete(root);

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for reboot
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "Rebooting...", HTTPD_RESP_USE_STRLEN);
    // Delay slightly to let the response go out
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

// Handler for factory reset
static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested via web interface");
    httpd_resp_send(req, "Factory reset initiated...", HTTPD_RESP_USE_STRLEN);

    // Delay slightly to let the response go out
    vTaskDelay(pdMS_TO_TICKS(100));

    // Perform factory reset
    app_config_factory_reset();

    // Reboot the device
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; 
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_get_config = {
            .uri       = "/api/config",
            .method    = HTTP_GET,
            .handler   = config_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_get_config);

        httpd_uri_t uri_post_config = {
            .uri       = "/api/config",
            .method    = HTTP_POST,
            .handler   = config_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_config);

        httpd_uri_t uri_post_reboot = {
            .uri       = "/api/reboot",
            .method    = HTTP_POST,
            .handler   = reboot_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_reboot);

        httpd_uri_t uri_post_factory_reset = {
            .uri       = "/api/factory-reset",
            .method    = HTTP_POST,
            .handler   = factory_reset_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_factory_reset);

        ESP_LOGI(TAG, "Web server started");
    } else {
        ESP_LOGE(TAG, "Error starting server!");
    }
}
