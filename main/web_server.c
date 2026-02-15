#include "web_server.h"
#include "app_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui/nina_dashboard.h"
#include "ui/themes.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/others/snapshot/lv_snapshot.h"

static const char *TAG = "web_server";

static const char *HTML_CONTENT =
"<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>NINA Display</title>"
"<style>"
":root{--bg-color:#111111;--tile-color:#1A1A1A;--accent-color:#2979ff;--text-primary:#ffffff;--text-secondary:#aaaaaa;--border-color:#333333;--danger-color:#cf6679;}"
"body{font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background-color:var(--bg-color);color:var(--text-primary);margin:0;padding:20px;}"
"input,select{background:#000;border:1px solid #333;color:#fff;padding:10px;border-radius:4px;width:100%;box-sizing:border-box;font-size:0.9rem;}"
"input:focus,select:focus{outline:none;border-color:var(--accent-color);}"
"h3{text-transform:uppercase;opacity:0.9;font-size:0.9rem;margin-top:0;margin-bottom:15px;letter-spacing:1px;color:var(--text-primary);}"
".container{max-width:800px;margin:0 auto;}"
".bento-grid{display:grid;gap:16px;grid-template-columns:1fr;}"
"@media(min-width:768px){"
".bento-grid{grid-template-columns:1fr 1fr;grid-template-areas:\"net api\" \"appearance appearance\" \"filter filter\" \"rms hfr\" \"actions actions\";}"
".area-net{grid-area:net;}.area-api{grid-area:api;}.area-appearance{grid-area:appearance;}.area-filter{grid-area:filter;}.area-rms{grid-area:rms;}.area-hfr{grid-area:hfr;}.area-actions{grid-area:actions;}}"
".tile{background-color:var(--tile-color);padding:20px;border-radius:8px;border:1px solid var(--border-color);}"
".group{margin-bottom:15px;}"
"label{display:block;margin-bottom:5px;color:var(--text-secondary);font-size:0.8rem;text-transform:uppercase;letter-spacing:0.5px;}"
".color-grid{display:flex;flex-wrap:wrap;gap:15px;}"
".filter-item{display:flex;flex-direction:column;align-items:center;gap:6px;min-width:55px;}"
".filter-bright{width:40px;height:4px;-webkit-appearance:none;appearance:none;background:#333;border-radius:2px;outline:none;padding:0;margin:0;}"
".filter-bright::-webkit-slider-thumb{-webkit-appearance:none;width:10px;height:10px;border-radius:50%;background:var(--accent-color);cursor:pointer;}"
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
"<div class=\"group\"><label>Host 2 (IP or Hostname)</label><input type=\"text\" id=\"url2\" placeholder=\"e.g., astromele3.lan or 192.168.1.101\"></div>"
"<div class=\"group\"><label>Host 3 (IP or Hostname)</label><input type=\"text\" id=\"url3\" placeholder=\"e.g., astromele4.lan or 192.168.1.102\"></div></div>"
"<div class=\"tile area-appearance\"><h3>Appearance</h3>"
"<div class=\"group\"><label>Dashboard Theme</label>"
"<select id=\"theme_select\" onchange=\"setTheme(this.value)\">"
"<option value=\"0\">Bento Default</option>"
"<option value=\"1\">OLED Black</option>"
"<option value=\"2\">Deep Space</option>"
"<option value=\"3\">Red Night</option>"
"<option value=\"4\">Cyberpunk</option>"
"<option value=\"5\">Midnight Green</option>"
"<option value=\"6\">Solarized Dark</option>"
"<option value=\"7\">Monochrome</option>"
"<option value=\"8\">Crimson</option>"
"<option value=\"9\">Slate</option>"
"<option value=\"10\">All Black</option>"
"</select></div>"
"<div class=\"group\"><label>Display Brightness</label>"
"<div class=\"flex-row\"><input type=\"range\" id=\"brightness\" min=\"0\" max=\"100\" value=\"50\" oninput=\"setBrightness(this.value)\" style=\"flex:1\"><span id=\"bright_val\" style=\"min-width:36px;text-align:right\">50%</span></div>"
"</div></div>"
"<div class=\"tile area-filter\"><h3>Filter Colors</h3>"
"<div class=\"group\"><label>Global Color Brightness</label>"
"<div class=\"flex-row\"><input type=\"range\" id=\"color_brightness\" min=\"0\" max=\"100\" value=\"100\" oninput=\"setColorBrightness(this.value)\" style=\"flex:1\"><span id=\"cbright_val\" style=\"min-width:36px;text-align:right\">100%</span></div></div>"
"<div id=\"filterColors\" class=\"color-grid\"></div></div>"
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
"let filterColorsObj={};let filterBrightnessObj={};"
"function renderFilterColors(){"
"const c=document.getElementById('filterColors');c.innerHTML='';"
"const fs=Object.keys(filterColorsObj).sort();"
"if(fs.length===0){c.innerHTML='<p style=\"color:var(--text-secondary);font-style:italic\">No filters loaded.</p>';return;}"
"fs.forEach(f=>{"
"const b=filterBrightnessObj[f]!=null?filterBrightnessObj[f]:100;"
"const w=document.createElement('div');w.className='filter-item';"
"w.innerHTML=`<label style='font-size:0.7rem;margin:0;color:var(--text-secondary)'>${f}</label>"
"<input type='color' class='color-dot' value='${filterColorsObj[f]}' onchange='updateFilterColor(\"${f}\",this.value)'>"
"<input type='range' class='filter-bright' min='0' max='100' value='${b}' title='${b}%' oninput='updateFilterBrightness(\"${f}\",this.value)'>`;"
"c.appendChild(w);});"
"}"
"function updateFilterColor(n,v){filterColorsObj[n]=v;}"
"function updateFilterBrightness(n,v){filterBrightnessObj[n]=parseInt(v);}"
"function setTheme(v){fetch('/api/theme',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({theme_index:parseInt(v)})});}"
"let brightTimer=null;function setBrightness(v){document.getElementById('bright_val').innerText=v+'%';clearTimeout(brightTimer);brightTimer=setTimeout(()=>{fetch('/api/brightness',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brightness:parseInt(v)})});},150);}"
"let cbrightTimer=null;function setColorBrightness(v){document.getElementById('cbright_val').innerText=v+'%';clearTimeout(cbrightTimer);cbrightTimer=setTimeout(()=>{fetch('/api/color-brightness',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({color_brightness:parseInt(v)})});},150);}"
"function checkOnline(){"
"return fetch('/api/config',{method:'GET',cache:'no-cache'}).then(r=>r.ok).catch(()=>false);"
"}"
"function waitForReboot(){"
"const b=document.querySelector('.btn-primary');"
"let attempts=0;const maxAttempts=60;"
"const check=()=>{"
"attempts++;checkOnline().then(online=>{"
"if(online){location.reload();}else if(attempts<maxAttempts){setTimeout(check,1000);}else{b.innerText='REBOOT TIMEOUT';b.disabled=false;}"
"});};"
"setTimeout(check,3000);"
"}"
"function save(){"
"const b=document.querySelector('.btn-primary');const ot=b.innerText;b.innerText='SAVING...';b.disabled=true;"
"const rms={good_max:parseFloat(document.getElementById('rms_good_max').value)||0.5,ok_max:parseFloat(document.getElementById('rms_ok_max').value)||1.0,good_color:document.getElementById('rms_good_color').value,ok_color:document.getElementById('rms_ok_color').value,bad_color:document.getElementById('rms_bad_color').value};"
"const hfr={good_max:parseFloat(document.getElementById('hfr_good_max').value)||2.0,ok_max:parseFloat(document.getElementById('hfr_ok_max').value)||3.5,good_color:document.getElementById('hfr_good_color').value,ok_color:document.getElementById('hfr_ok_color').value,bad_color:document.getElementById('hfr_bad_color').value};"
"const h1=document.getElementById('url1').value.trim();"
"const h2=document.getElementById('url2').value.trim();"
"const h3=document.getElementById('url3').value.trim();"
"const u1=h1?'http://'+h1+':1888/v2/api/':'';"
"const u2=h2?'http://'+h2+':1888/v2/api/':'';"
"const u3=h3?'http://'+h3+':1888/v2/api/':'';"
"const d={ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value,url1:u1,url2:u2,url3:u3,ntp:document.getElementById('ntp').value,theme_index:parseInt(document.getElementById('theme_select').value),brightness:parseInt(document.getElementById('brightness').value),color_brightness:parseInt(document.getElementById('color_brightness').value),filter_colors:JSON.stringify(filterColorsObj),filter_brightness:JSON.stringify(filterBrightnessObj),rms_thresholds:JSON.stringify(rms),hfr_thresholds:JSON.stringify(hfr)};"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})"
".then(r=>{if(r.ok){b.innerText='REBOOTING...';fetch('/api/reboot',{method:'POST'}).catch(()=>{});waitForReboot();}else{alert('Error saving');b.innerText=ot;b.disabled=false;}}).catch(e=>{alert('Connection failed');b.innerText=ot;b.disabled=false;});"
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
"document.getElementById('url3').value=extractHost(d.url3||'');"
"document.getElementById('theme_select').value=d.theme_index||0;"
"var br=d.brightness!=null?d.brightness:50;document.getElementById('brightness').value=br;document.getElementById('bright_val').innerText=br+'%';"
"var cb=d.color_brightness!=null?d.color_brightness:100;document.getElementById('color_brightness').value=cb;document.getElementById('cbright_val').innerText=cb+'%';"
"try{filterColorsObj=JSON.parse(d.filter_colors||'{}');}catch(e){}"
"try{filterBrightnessObj=JSON.parse(d.filter_brightness||'{}');}catch(e){}"
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
    cJSON_AddStringToObject(root, "url3", cfg->api_url_3);
    cJSON_AddStringToObject(root, "ntp", cfg->ntp_server);
    cJSON_AddStringToObject(root, "filter_colors", cfg->filter_colors);
    cJSON_AddStringToObject(root, "rms_thresholds", cfg->rms_thresholds);
    cJSON_AddStringToObject(root, "hfr_thresholds", cfg->hfr_thresholds);
    cJSON_AddStringToObject(root, "filter_brightness", cfg->filter_brightness);
    cJSON_AddNumberToObject(root, "theme_index", cfg->theme_index);
    cJSON_AddNumberToObject(root, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(root, "color_brightness", cfg->color_brightness);

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
    char buf[2560];
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

    cJSON *url3 = cJSON_GetObjectItem(root, "url3");
    if (cJSON_IsString(url3)) {
        strncpy(cfg.api_url_3, url3->valuestring, sizeof(cfg.api_url_3) - 1);
        cfg.api_url_3[sizeof(cfg.api_url_3) - 1] = '\0';
    }

    cJSON *ntp = cJSON_GetObjectItem(root, "ntp");
    if (cJSON_IsString(ntp)) {
        strncpy(cfg.ntp_server, ntp->valuestring, sizeof(cfg.ntp_server) - 1);
        cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = '\0';
    }

    cJSON *theme_index = cJSON_GetObjectItem(root, "theme_index");
    if (cJSON_IsNumber(theme_index)) {
        cfg.theme_index = theme_index->valueint;
    }

    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        int val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg.brightness = val;
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

    cJSON *filter_brightness = cJSON_GetObjectItem(root, "filter_brightness");
    if (cJSON_IsString(filter_brightness)) {
        strncpy(cfg.filter_brightness, filter_brightness->valuestring, sizeof(cfg.filter_brightness) - 1);
        cfg.filter_brightness[sizeof(cfg.filter_brightness) - 1] = '\0';
    }

    cJSON *color_brightness = cJSON_GetObjectItem(root, "color_brightness");
    if (cJSON_IsNumber(color_brightness)) {
        int val = color_brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg.color_brightness = val;
    }

    app_config_save(&cfg);
    cJSON_Delete(root);

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live brightness adjustment (no reboot needed)
static esp_err_t brightness_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
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

    cJSON *val = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(val)) {
        int brightness = val->valueint;
        if (brightness < 0) brightness = 0;
        if (brightness > 100) brightness = 100;
        bsp_display_brightness_set(brightness);
        ESP_LOGI(TAG, "Brightness set to %d%%", brightness);
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live color brightness adjustment (no reboot needed)
static esp_err_t color_brightness_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
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

    cJSON *val = cJSON_GetObjectItem(root, "color_brightness");
    if (cJSON_IsNumber(val)) {
        int cb = val->valueint;
        if (cb < 0) cb = 0;
        if (cb > 100) cb = 100;
        app_config_get()->color_brightness = cb;
        
        // Re-apply theme to update static text brightness
        bsp_display_lock(0);
        nina_dashboard_apply_theme(app_config_get()->theme_index);
        bsp_display_unlock();
        
        ESP_LOGI(TAG, "Color brightness set to %d%%", cb);
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live theme switching (no reboot needed)
static esp_err_t theme_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
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

    cJSON *val = cJSON_GetObjectItem(root, "theme_index");
    if (cJSON_IsNumber(val)) {
        int idx = val->valueint;
        if (idx < 0) idx = 0;
        if (idx >= themes_get_count()) idx = themes_get_count() - 1;
        app_config_get()->theme_index = idx;
        bsp_display_lock(0);
        nina_dashboard_apply_theme(idx);
        bsp_display_unlock();
        ESP_LOGI(TAG, "Theme set to %d", idx);
    }

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

// BMP file header for 720x720 RGB565 image (bottom-up, no compression)
#define SCREENSHOT_W    BSP_LCD_H_RES
#define SCREENSHOT_H    BSP_LCD_V_RES
#define BMP_HEADER_SIZE 66  // 14 (file header) + 40 (DIB header) + 12 (RGB565 masks)

static void build_bmp_header(uint8_t *hdr, uint32_t width, uint32_t height)
{
    uint32_t row_size = width * 2;  // RGB565: 2 bytes per pixel
    uint32_t img_size = row_size * height;
    uint32_t file_size = BMP_HEADER_SIZE + img_size;

    memset(hdr, 0, BMP_HEADER_SIZE);

    // BMP file header (14 bytes)
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &file_size, 4);
    // reserved (4 bytes) = 0
    uint32_t offset = BMP_HEADER_SIZE;
    memcpy(&hdr[10], &offset, 4);

    // DIB header (BITMAPINFOHEADER = 40 bytes)
    uint32_t dib_size = 40;
    memcpy(&hdr[14], &dib_size, 4);
    int32_t w = (int32_t)width;
    int32_t h = -(int32_t)height;  // negative = top-down (no row reversal needed)
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    uint16_t planes = 1;
    memcpy(&hdr[26], &planes, 2);
    uint16_t bpp = 16;
    memcpy(&hdr[28], &bpp, 2);
    uint32_t compression = 3;  // BI_BITFIELDS
    memcpy(&hdr[30], &compression, 4);
    memcpy(&hdr[34], &img_size, 4);
    // pixels per meter (can be 0)
    // colors used, important (0)

    // RGB565 bitmasks (12 bytes after DIB header)
    uint32_t mask_r = 0xF800;
    uint32_t mask_g = 0x07E0;
    uint32_t mask_b = 0x001F;
    memcpy(&hdr[54], &mask_r, 4);
    memcpy(&hdr[58], &mask_g, 4);
    memcpy(&hdr[62], &mask_b, 4);
}

// Handler for screenshot capture - serves a BMP image viewable in any browser
static esp_err_t screenshot_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Screenshot requested");

    // Take LVGL snapshot while holding the display lock
    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "Failed to acquire display lock for screenshot");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "No active screen");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    lv_draw_buf_t *snapshot = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();

    if (!snapshot || !snapshot->data) {
        ESP_LOGE(TAG, "Snapshot capture failed");
        if (snapshot) lv_draw_buf_destroy(snapshot);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint32_t width = snapshot->header.w;
    uint32_t height = snapshot->header.h;
    uint32_t stride = snapshot->header.stride;
    uint32_t row_size = width * 2;  // RGB565 packed row (no padding)

    ESP_LOGI(TAG, "Snapshot captured: %lux%lu, stride=%lu", width, height, stride);

    // Build BMP header
    uint8_t bmp_hdr[BMP_HEADER_SIZE];
    build_bmp_header(bmp_hdr, width, height);

    // Send as BMP image
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.bmp\"");

    // Send BMP header
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)bmp_hdr, BMP_HEADER_SIZE);
    if (err != ESP_OK) {
        lv_draw_buf_destroy(snapshot);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_FAIL;
    }

    // Send pixel rows - handle stride != row_size (LVGL may add padding per row)
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = snapshot->data + (y * stride);
        err = httpd_resp_send_chunk(req, (const char *)row, row_size);
        if (err != ESP_OK) {
            break;
        }
    }

    // End chunked response
    httpd_resp_send_chunk(req, NULL, 0);
    lv_draw_buf_destroy(snapshot);

    ESP_LOGI(TAG, "Screenshot sent successfully");
    return ESP_OK;
}

void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
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

        httpd_uri_t uri_post_brightness = {
            .uri       = "/api/brightness",
            .method    = HTTP_POST,
            .handler   = brightness_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_brightness);

        httpd_uri_t uri_post_color_brightness = {
            .uri       = "/api/color-brightness",
            .method    = HTTP_POST,
            .handler   = color_brightness_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_color_brightness);

        httpd_uri_t uri_post_theme = {
            .uri       = "/api/theme",
            .method    = HTTP_POST,
            .handler   = theme_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_theme);

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

        httpd_uri_t uri_get_screenshot = {
            .uri       = "/api/screenshot",
            .method    = HTTP_GET,
            .handler   = screenshot_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_get_screenshot);

        ESP_LOGI(TAG, "Web server started");
    } else {
        ESP_LOGE(TAG, "Error starting server!");
    }
}