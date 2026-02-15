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
"<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>NINA Display</title>"
"<style>"
":root{--bg-color:#111111;--tile-color:#1A1A1A;--accent-color:#2979ff;--text-primary:#ffffff;--text-secondary:#aaaaaa;--border-color:#333333;--danger-color:#cf6679;}"
"body{font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background-color:var(--bg-color);color:var(--text-primary);margin:0;padding:20px;}"
"input{background:#000;border:1px solid #333;color:#fff;padding:10px;border-radius:4px;width:100%;box-sizing:border-box;font-size:0.9rem;}"
"input:focus{outline:none;border-color:var(--accent-color);}"
"h3{text-transform:uppercase;opacity:0.9;font-size:0.9rem;margin-top:0;margin-bottom:15px;letter-spacing:1px;color:var(--text-primary);}"
".container{max-width:800px;margin:0 auto;}"
".bento-grid{display:grid;gap:16px;grid-template-columns:1fr;}"
"@media(min-width:768px){"
".bento-grid{grid-template-columns:1fr 1fr;grid-template-areas:\"net api\" \"filter filter\" \"rms hfr\" \"actions actions\";}"
".area-net{grid-area:net;}.area-api{grid-area:api;}.area-filter{grid-area:filter;}.area-rms{grid-area:rms;}.area-hfr{grid-area:hfr;}.area-actions{grid-area:actions;}}"
".tile{background-color:var(--tile-color);padding:20px;border-radius:8px;border:1px solid var(--border-color);}"
".group{margin-bottom:15px;}"
"label{display:block;margin-bottom:5px;color:var(--text-secondary);font-size:0.8rem;text-transform:uppercase;letter-spacing:0.5px;}"
".color-grid{display:flex;flex-wrap:wrap;gap:15px;}"
".filter-item{display:flex;flex-direction:column;align-items:center;gap:8px;}"
".color-dot{width:40px;height:40px;border-radius:50%;border:2px solid var(--border-color);cursor:pointer;padding:0;background:none;transition:transform 0.2s,border-color 0.2s;}"
".color-dot:hover{transform:scale(1.1);border-color:var(--text-primary);}"
".btn{width:100%;padding:14px;border:none;border-radius:4px;font-weight:bold;cursor:pointer;text-transform:uppercase;font-size:0.9rem;letter-spacing:1px;transition:opacity 0.2s;}"
".btn-primary{background-color:var(--accent-color);color:#fff;}"
".btn-danger{background-color:transparent;border:1px solid var(--danger-color);color:var(--danger-color);margin-top:10px;}"
".btn:hover{opacity:0.8;}"
".flex-row{display:flex;gap:10px;align-items:center;}"
".color-rect{width:40px;height:38px;padding:0;border:1px solid #333;background:none;border-radius:4px;cursor:pointer;}"
"@media(min-width:768px){.area-actions{display:flex;gap:16px;}.btn-danger{margin-top:0;}}"
"</style></head>"
"<body><div class=\"container\"><div class=\"bento-grid\">"
"<div class=\"tile area-net\"><h3>Network</h3>"
"<div class=\"group\"><label>SSID</label><input type=\"text\" id=\"ssid\"></div>"
"<div class=\"group\"><label>Password</label><input type=\"password\" id=\"pass\"></div>"
"<div class=\"group\"><label>NTP Server</label><input type=\"text\" id=\"ntp\"></div></div>"
"<div class=\"tile area-api\"><h3>NINA API</h3>"
"<div class=\"group\"><label>Host 1 (IP or Hostname)</label><input type=\"text\" id=\"url1\" placeholder=\"e.g., astromele2.lan or 192.168.1.100\"></div>"
"<div class=\"group\"><label>Host 2 (IP or Hostname)</label><input type=\"text\" id=\"url2\" placeholder=\"e.g., astromele3.lan or 192.168.1.101\"></div></div>"
"<div class=\"tile area-filter\"><h3>Filter Colors</h3><div id=\"filterColors\" class=\"color-grid\"></div></div>"
"<div class=\"tile area-rms\"><h3>RMS THRESHOLDS</h3>"
"<div class=\"group\"><label>Good RMS Max Value</label><div class=\"flex-row\"><input type=\"text\" id=\"rms_good_max\"><input type=\"color\" id=\"rms_good_color\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>OK RMS Max Value</label><div class=\"flex-row\"><input type=\"text\" id=\"rms_ok_max\"><input type=\"color\" id=\"rms_ok_color\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Bad RMS Max Color</label><input type=\"color\" id=\"rms_bad_color\" class=\"color-rect\" style=\"width:100%\"></div></div>"
"<div class=\"tile area-hfr\"><h3>HFR THRESHOLDS</h3>"
"<div class=\"group\"><label>Good HFR Max Value</label><div class=\"flex-row\"><input type=\"text\" id=\"hfr_good_max\"><input type=\"color\" id=\"hfr_good_color\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>OK HFR Max Value</label><div class=\"flex-row\"><input type=\"text\" id=\"hfr_ok_max\"><input type=\"color\" id=\"hfr_ok_color\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Bad HFR Color</label><input type=\"color\" id=\"hfr_bad_color\" class=\"color-rect\" style=\"width:100%\"></div></div>"
"<div class=\"tile area-actions\">"
"<button class=\"btn btn-primary\" onclick=\"save()\">Save & Reboot</button>"
"<button class=\"btn btn-danger\" onclick=\"factoryReset()\">Factory Reset</button></div>"
"</div></div>"
"<script>"
"let filterColorsObj={};"
"function renderFilterColors(){"
"const c=document.getElementById('filterColors');c.innerHTML='';"
"const fs=Object.keys(filterColorsObj).sort();"
"if(fs.length===0){c.innerHTML='<p style=\"color:var(--text-secondary);font-style:italic\">No filters loaded.</p>';return;}"
"fs.forEach(f=>{"
"const w=document.createElement('div');w.className='filter-item';"
"w.innerHTML=`<label style='font-size:0.7rem;margin:0;color:var(--text-secondary)'>${f}</label><input type='color' class='color-dot' value='${filterColorsObj[f]}' onchange='updateFilterColor(\"${f}\",this.value)'>`;"
"c.appendChild(w);});"
"}"
"function updateFilterColor(n,v){filterColorsObj[n]=v;}"
"function save(){"
"const b=document.querySelector('.btn-primary');const ot=b.innerText;b.innerText='SAVING...';b.disabled=true;"
"const rms={good_max:parseFloat(document.getElementById('rms_good_max').value)||0.5,ok_max:parseFloat(document.getElementById('rms_ok_max').value)||1.0,good_color:document.getElementById('rms_good_color').value,ok_color:document.getElementById('rms_ok_color').value,bad_color:document.getElementById('rms_bad_color').value};"
"const hfr={good_max:parseFloat(document.getElementById('hfr_good_max').value)||2.0,ok_max:parseFloat(document.getElementById('hfr_ok_max').value)||3.5,good_color:document.getElementById('hfr_good_color').value,ok_color:document.getElementById('hfr_ok_color').value,bad_color:document.getElementById('hfr_bad_color').value};"
"const h1=document.getElementById('url1').value.trim();"
"const h2=document.getElementById('url2').value.trim();"
"const u1=h1?'http://'+h1+':1888/v2/api/':'';"
"const u2=h2?'http://'+h2+':1888/v2/api/':'';"
"const d={ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value,url1:u1,url2:u2,ntp:document.getElementById('ntp').value,filter_colors:JSON.stringify(filterColorsObj),rms_thresholds:JSON.stringify(rms),hfr_thresholds:JSON.stringify(hfr)};"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})"
".then(r=>{if(r.ok){alert('Saved! Rebooting...');fetch('/api/reboot',{method:'POST'});}else{alert('Error saving');b.innerText=ot;b.disabled=false;}}).catch(e=>{alert('Connection failed');b.innerText=ot;b.disabled=false;});"
"}"
"function factoryReset(){"
"if(confirm('WARNING: Factory Reset?\\n\\nAll settings will be lost.')){if(confirm('Are you sure?')){fetch('/api/factory-reset',{method:'POST'}).then(r=>{if(r.ok)alert('Resetting...');});}}}"
"fetch('/api/config').then(r=>r.json()).then(d=>{"
"document.getElementById('ssid').value=d.ssid||'';"
"document.getElementById('pass').value=d.pass||'';"
"document.getElementById('ntp').value=d.ntp||'';"
"const extractHost=u=>{if(!u)return '';const m=u.match(/^https?:\\/\\/([^:\\/]+)/);return m?m[1]:u;};"
"document.getElementById('url1').value=extractHost(d.url1);"
"document.getElementById('url2').value=extractHost(d.url2);"
"try{filterColorsObj=JSON.parse(d.filter_colors||'{}');}catch(e){}"
"renderFilterColors();"
"try{const r=JSON.parse(d.rms_thresholds||'{}');"
"document.getElementById('rms_good_max').value=r.good_max||0.5;"
"document.getElementById('rms_ok_max').value=r.ok_max||1.0;"
"document.getElementById('rms_good_color').value=r.good_color||'#10b981';"
"document.getElementById('rms_ok_color').value=r.ok_color||'#eab308';"
"document.getElementById('rms_bad_color').value=r.bad_color||'#ef4444';"
"}catch(e){}"
"try{const h=JSON.parse(d.hfr_thresholds||'{}');"
"document.getElementById('hfr_good_max').value=h.good_max||2.0;"
"document.getElementById('hfr_ok_max').value=h.ok_max||3.5;"
"document.getElementById('hfr_good_color').value=h.good_color||'#10b981';"
"document.getElementById('hfr_ok_color').value=h.ok_color||'#eab308';"
"document.getElementById('hfr_bad_color').value=h.bad_color||'#ef4444';"
"}catch(e){}"
"});"
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
