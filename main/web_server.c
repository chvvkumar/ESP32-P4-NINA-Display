#include "web_server.h"
#include "app_config.h"
#include "mqtt_ha.h"
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
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>NINA Display</title>"
"<style>"
":root{"
"--bg:#09090b;--card:#121217;--border:#27272a;--accent:#14b8a6;--accent-hover:#0d9488;--text:#fafafa;--text-dim:#a1a1aa;--danger:#ef4444;--warning:#f59e0b;"
"}"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background-color:var(--bg);color:var(--text);margin:0;padding:24px;line-height:1.5;}"
".container{max-width:1100px;margin:0 auto;}"
"header{margin-bottom:32px;border-left:4px solid var(--accent);padding-left:16px;}"
"header h1{margin:0;font-size:1.5rem;font-weight:700;letter-spacing:-0.025em;}"
"header p{margin:4px 0 0;color:var(--text-dim);font-size:0.9rem;}"
".bento-grid{display:grid;gap:20px;grid-template-columns:repeat(12, 1fr);}"
".tile{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:24px;display:flex;flex-direction:column;transition:border-color 0.2s;}"
".tile:hover{border-color:var(--accent);}"
".tile h3{margin:0 0 20px 0;font-size:0.85rem;font-weight:600;text-transform:uppercase;letter-spacing:0.05em;color:var(--accent);display:flex;align-items:center;gap:8px;}"
".group{margin-bottom:18px;position:relative;}"
"label{display:block;margin-bottom:6px;color:var(--text-dim);font-size:0.75rem;font-weight:500;text-transform:uppercase;}"
"input[type='text'],input[type='password'],select{background:var(--bg);border:1px solid var(--border);color:var(--text);padding:10px 14px;border-radius:8px;width:100%;font-size:0.9rem;transition:all 0.2s;}"
"input:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 2px rgba(20,184,166,0.2);}"
".color-grid{display:flex;gap:12px;width:100%;flex-wrap:wrap;margin-top:10px;}"
".filter-item{flex:1 0 90px;min-width:0;max-width:200px;background:rgba(255,255,255,0.03);padding:14px 10px;border-radius:12px;display:flex;flex-direction:column;align-items:center;gap:10px;border:1px solid transparent;transition:all 0.2s;}"
".filter-item:hover{background:rgba(255,255,255,0.06);border-color:var(--border);}"
".filter-item label{margin:0;font-size:0.7rem;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:100%;text-transform:none;}"
".color-dot{width:40px;height:40px;border-radius:10px;border:2px solid var(--border);cursor:pointer;padding:0;background:none;transition:transform 0.1s;}"
".color-dot:active{transform:scale(0.9);}"
".slider-wrapper{display:flex;align-items:center;gap:12px;}"
"input[type='range']{flex:1;height:6px;appearance:none;background:var(--border);border-radius:3px;outline:none;}"
"input[type='range']::-webkit-slider-thumb{appearance:none;width:18px;height:18px;border-radius:50%;background:var(--accent);cursor:pointer;border:3px solid var(--card);box-shadow:0 0 10px rgba(0,0,0,0.5);}"
".toggle{position:relative;display:inline-block;width:44px;height:24px;}"
".toggle input{opacity:0;width:0;height:0;}"
".toggle-slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:var(--border);transition:.2s;border-radius:24px;}"
".toggle-slider:before{content:'';position:absolute;height:18px;width:18px;left:3px;bottom:3px;background:var(--text-dim);transition:.2s;border-radius:50%;}"
".toggle input:checked+.toggle-slider{background:var(--accent);}"
".toggle input:checked+.toggle-slider:before{transform:translateX(20px);background:var(--bg);}"
".btn{border:none;border-radius:10px;padding:14px 20px;font-weight:600;cursor:pointer;font-size:0.9rem;transition:all 0.2s;display:flex;align-items:center;justify-content:center;gap:8px;}"
".btn-primary{background:var(--accent);color:var(--bg);width:100%;}"
".btn-primary:hover{background:var(--accent-hover);transform:translateY(-1px);}"
".btn-outline{background:transparent;border:1px solid var(--border);color:var(--text-dim);flex:1;}"
".btn-outline:hover{border-color:var(--warning);color:var(--warning);}"
".btn-danger{background:transparent;border:1px solid var(--border);color:var(--text-dim);flex:1;}"
".btn-danger:hover{border-color:var(--danger);color:var(--danger);}"
".color-rect{width:48px;height:38px;padding:0;border:1px solid var(--border);background:none;border-radius:8px;cursor:pointer;}"
".area-net{grid-column:span 4;}.area-api{grid-column:span 4;}.area-appearance{grid-column:span 4;}"
".area-thresholds{grid-column:span 12;}"
".filter-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:8px;}"
".filter-grid .filter-item{flex:none;min-width:0;padding:8px 4px;}"
".filter-grid .filter-item label{font-size:0.65rem;}"
".filter-grid .color-dot{width:32px;height:32px;border-radius:8px;}"
".area-mqtt{grid-column:span 6;}"
".area-actions{grid-column:span 12;}"
".action-row{display:flex;gap:12px;}"
".action-row .btn{flex:1;}"
".hint{font-size:0.75rem;color:var(--text-dim);margin:4px 0 0 0;}"
".threshold-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:20px;}"
".threshold-node{background:rgba(255,255,255,0.02);border:1px solid var(--border);border-radius:12px;padding:16px;}"
".threshold-node h4{margin:0 0 14px;font-size:0.82rem;color:var(--accent);font-weight:600;text-align:center;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
".threshold-sub h5{margin:0 0 10px;font-size:0.72rem;color:var(--text-dim);font-weight:600;text-transform:uppercase;letter-spacing:0.04em;}"
".threshold-sub{margin-bottom:16px;}"
".threshold-sub:last-child{margin-bottom:0;}"
"@media(max-width:900px){"
".area-net,.area-api,.area-appearance,.area-mqtt{grid-column:span 12;}"
".action-row{flex-direction:column;}"
".action-row .btn{width:100%;}"
".filter-item{flex:1 0 45%;}"
".threshold-grid{grid-template-columns:1fr;}"
"}"
"</style></head>"
"<body><div class=\"container\">"
"<header><h1>NINA DISPLAY</h1><p>System Configuration</p></header>"
"<div class=\"bento-grid\">"
"<div class=\"tile area-net\"><h3>Connectivity</h3>"
"<div class=\"group\"><label>SSID</label><input type=\"text\" id=\"ssid\"></div>"
"<div class=\"group\"><label>Password</label><input type=\"password\" id=\"pass\"></div>"
"<div class=\"group\"><label>NTP Server</label><input type=\"text\" id=\"ntp\"></div></div>"
"<div class=\"tile area-api\"><h3>NINA Endpoints</h3>"
"<div class=\"group\"><label>NINA 1</label><input type=\"text\" id=\"url1\" placeholder=\"astromele2.lan\" oninput=\"updateNodeLabels()\"></div>"
"<div class=\"group\"><label>NINA 2</label><input type=\"text\" id=\"url2\" placeholder=\"astromele3.lan\" oninput=\"updateNodeLabels()\"></div>"
"<div class=\"group\"><label>NINA 3</label><input type=\"text\" id=\"url3\" placeholder=\"astromele4.lan\" oninput=\"updateNodeLabels()\"></div></div>"
"<div class=\"tile area-appearance\"><h3>Display</h3>"
"<div class=\"group\"><label>Theme</label><select id=\"theme_select\" onchange=\"setTheme(this.value)\">"
"<option value=\"0\">Bento Default</option><option value=\"1\">OLED Black</option><option value=\"2\">Deep Space</option><option value=\"3\">Red Night</option><option value=\"4\">Cyberpunk</option><option value=\"5\">Midnight Green</option><option value=\"6\">Solarized Dark</option><option value=\"7\">Monochrome</option><option value=\"8\">Crimson</option><option value=\"9\">Slate</option><option value=\"10\">All Black</option><option value=\"11\">Kumar</option><option value=\"12\">Aquamarine Dream</option><option value=\"13\">Midnight Industrial</option><option value=\"14\">Electric Prism</option>"
"</select></div>"
"<div class=\"group\"><label>Backlight</label><div class=\"slider-wrapper\"><input type=\"range\" id=\"brightness\" min=\"0\" max=\"100\" value=\"50\" oninput=\"setBrightness(this.value)\"><span id=\"bright_val\" style=\"width:35px;font-size:0.8rem\">50%</span></div></div>"
"<div class=\"group\"><label>Text Brightness</label><div class=\"slider-wrapper\"><input type=\"range\" id=\"color_brightness\" min=\"0\" max=\"100\" value=\"100\" oninput=\"setColorBrightness(this.value)\"><span id=\"cbright_val\" style=\"width:35px;font-size:0.8rem\">100%</span></div></div></div>"
"<div class=\"tile area-thresholds\"><h3>NINA Settings</h3>"
"<div class=\"threshold-grid\">"
"<div class=\"threshold-node\"><h4 id=\"tnode0\">Node 1</h4>"
"<div class=\"threshold-sub\"><h5>Filters</h5><div id=\"filters_0\" class=\"filter-grid\"></div></div>"
"<div class=\"threshold-sub\"><h5>RMS</h5>"
"<div class=\"group\"><label>Target (Optimal)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"rms_good_max_0\"><input type=\"color\" id=\"rms_good_color_0\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Warning (Caution)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"rms_ok_max_0\"><input type=\"color\" id=\"rms_ok_color_0\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Critical Color</label><input type=\"color\" id=\"rms_bad_color_0\" class=\"color-rect\" style=\"width:100%\"></div></div>"
"<div class=\"threshold-sub\"><h5>HFR</h5>"
"<div class=\"group\"><label>Target (Optimal)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"hfr_good_max_0\"><input type=\"color\" id=\"hfr_good_color_0\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Warning (Caution)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"hfr_ok_max_0\"><input type=\"color\" id=\"hfr_ok_color_0\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Critical Color</label><input type=\"color\" id=\"hfr_bad_color_0\" class=\"color-rect\" style=\"width:100%\"></div></div></div>"
"<div class=\"threshold-node\"><h4 id=\"tnode1\">Node 2</h4>"
"<div class=\"threshold-sub\"><h5>Filters</h5><div id=\"filters_1\" class=\"filter-grid\"></div></div>"
"<div class=\"threshold-sub\"><h5>RMS</h5>"
"<div class=\"group\"><label>Target (Optimal)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"rms_good_max_1\"><input type=\"color\" id=\"rms_good_color_1\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Warning (Caution)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"rms_ok_max_1\"><input type=\"color\" id=\"rms_ok_color_1\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Critical Color</label><input type=\"color\" id=\"rms_bad_color_1\" class=\"color-rect\" style=\"width:100%\"></div></div>"
"<div class=\"threshold-sub\"><h5>HFR</h5>"
"<div class=\"group\"><label>Target (Optimal)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"hfr_good_max_1\"><input type=\"color\" id=\"hfr_good_color_1\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Warning (Caution)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"hfr_ok_max_1\"><input type=\"color\" id=\"hfr_ok_color_1\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Critical Color</label><input type=\"color\" id=\"hfr_bad_color_1\" class=\"color-rect\" style=\"width:100%\"></div></div></div>"
"<div class=\"threshold-node\"><h4 id=\"tnode2\">Node 3</h4>"
"<div class=\"threshold-sub\"><h5>Filters</h5><div id=\"filters_2\" class=\"filter-grid\"></div></div>"
"<div class=\"threshold-sub\"><h5>RMS</h5>"
"<div class=\"group\"><label>Target (Optimal)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"rms_good_max_2\"><input type=\"color\" id=\"rms_good_color_2\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Warning (Caution)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"rms_ok_max_2\"><input type=\"color\" id=\"rms_ok_color_2\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Critical Color</label><input type=\"color\" id=\"rms_bad_color_2\" class=\"color-rect\" style=\"width:100%\"></div></div>"
"<div class=\"threshold-sub\"><h5>HFR</h5>"
"<div class=\"group\"><label>Target (Optimal)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"hfr_good_max_2\"><input type=\"color\" id=\"hfr_good_color_2\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Warning (Caution)</label><div style=\"display:flex;gap:8px\"><input type=\"text\" id=\"hfr_ok_max_2\"><input type=\"color\" id=\"hfr_ok_color_2\" class=\"color-rect\"></div></div>"
"<div class=\"group\"><label>Critical Color</label><input type=\"color\" id=\"hfr_bad_color_2\" class=\"color-rect\" style=\"width:100%\"></div></div></div>"
"</div></div>"
"<div class=\"tile area-mqtt\"><h3>MQTT / Home Assistant</h3>"
"<div class=\"group\" style=\"display:flex;align-items:center;justify-content:space-between\"><label style=\"margin:0\">Enable MQTT</label><label class=\"toggle\"><input type=\"checkbox\" id=\"mqtt_enabled\"><span class=\"toggle-slider\"></span></label></div>"
"<div class=\"group\"><label>Broker Host</label><input type=\"text\" id=\"mqtt_broker\" placeholder=\"192.168.1.250\"></div>"
"<div class=\"group\"><label>Port</label><input type=\"text\" id=\"mqtt_port\" placeholder=\"1883\"></div>"
"<div class=\"group\"><label>Username</label><input type=\"text\" id=\"mqtt_user\"></div>"
"<div class=\"group\"><label>Password</label><input type=\"password\" id=\"mqtt_pass\"></div>"
"<div class=\"group\"><label>Topic Prefix</label><input type=\"text\" id=\"mqtt_prefix\" placeholder=\"ninadisplay\"></div>"
"<p class=\"hint\">Reboot required after changing MQTT settings.</p></div>"
"<div class=\"tile area-actions\"><h3>Actions</h3>"
"<div class=\"action-row\">"
"<button class=\"btn btn-primary\" id=\"saveBtn\" onclick=\"save()\">SAVE</button>"
"<button class=\"btn btn-outline\" onclick=\"saveAndReboot()\">REBOOT</button>"
"<button class=\"btn btn-danger\" onclick=\"factoryReset()\">FACTORY RESET</button>"
"</div>"
"<p class=\"hint\">Reboot is required to apply WiFi, NTP, or MQTT changes.</p></div>"
"</div></div>"
"<script>"
"let filterColorsArr=[{},{},{}];"
"function renderFilterColors(i){"
"const c=document.getElementById('filters_'+i);if(!c)return;c.innerHTML='';"
"const obj=filterColorsArr[i];const fs=Object.keys(obj).sort();"
"if(fs.length===0){c.innerHTML='<p style=\"color:var(--text-dim);font-style:italic;font-size:0.7rem\">No filters</p>';return;}"
"fs.slice(0,8).forEach(f=>{"
"const w=document.createElement('div');w.className='filter-item';"
"w.innerHTML=`<label>${f}</label>"
"<input type='color' class='color-dot' value='${obj[f]}' onchange='updateFilterColor(${i},\"${f}\",this.value)'>`;"
"c.appendChild(w);});"
"}"
"function updateFilterColor(i,n,v){filterColorsArr[i][n]=v;}"
"function updateNodeLabels(){"
"['url1','url2','url3'].forEach((id,i)=>{"
"const v=document.getElementById(id).value.trim();"
"document.getElementById('tnode'+i).innerText=v||('Node '+(i+1));"
"});}"
"function setTheme(v){fetch('/api/theme',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({theme_index:parseInt(v)})});}"
"let brightTimer=null;function setBrightness(v){document.getElementById('bright_val').innerText=v+'%';clearTimeout(brightTimer);brightTimer=setTimeout(()=>{fetch('/api/brightness',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brightness:parseInt(v)})});},150);}"
"let cbrightTimer=null;function setColorBrightness(v){document.getElementById('cbright_val').innerText=v+'%';clearTimeout(cbrightTimer);cbrightTimer=setTimeout(()=>{fetch('/api/color-brightness',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({color_brightness:parseInt(v)})});},150);}"
"function getRmsObj(i){return{good_max:parseFloat(document.getElementById('rms_good_max_'+i).value)||0.5,ok_max:parseFloat(document.getElementById('rms_ok_max_'+i).value)||1.0,good_color:document.getElementById('rms_good_color_'+i).value,ok_color:document.getElementById('rms_ok_color_'+i).value,bad_color:document.getElementById('rms_bad_color_'+i).value};}"
"function getHfrObj(i){return{good_max:parseFloat(document.getElementById('hfr_good_max_'+i).value)||2.0,ok_max:parseFloat(document.getElementById('hfr_ok_max_'+i).value)||3.5,good_color:document.getElementById('hfr_good_color_'+i).value,ok_color:document.getElementById('hfr_ok_color_'+i).value,bad_color:document.getElementById('hfr_bad_color_'+i).value};}"
"function getConfigData(){"
"const h1=document.getElementById('url1').value.trim();const h2=document.getElementById('url2').value.trim();const h3=document.getElementById('url3').value.trim();"
"const u1=h1?'http://'+h1+':1888/v2/api/':'';const u2=h2?'http://'+h2+':1888/v2/api/':'';const u3=h3?'http://'+h3+':1888/v2/api/':'';"
"const mb=document.getElementById('mqtt_broker').value.trim();const mu=mb?'mqtt://'+mb:'';"
"return{ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value,url1:u1,url2:u2,url3:u3,ntp:document.getElementById('ntp').value,theme_index:parseInt(document.getElementById('theme_select').value),brightness:parseInt(document.getElementById('brightness').value),color_brightness:parseInt(document.getElementById('color_brightness').value),filter_colors_1:JSON.stringify(filterColorsArr[0]),filter_colors_2:JSON.stringify(filterColorsArr[1]),filter_colors_3:JSON.stringify(filterColorsArr[2]),rms_thresholds_1:JSON.stringify(getRmsObj(0)),rms_thresholds_2:JSON.stringify(getRmsObj(1)),rms_thresholds_3:JSON.stringify(getRmsObj(2)),hfr_thresholds_1:JSON.stringify(getHfrObj(0)),hfr_thresholds_2:JSON.stringify(getHfrObj(1)),hfr_thresholds_3:JSON.stringify(getHfrObj(2)),mqtt_enabled:document.getElementById('mqtt_enabled').checked,mqtt_broker_url:mu,mqtt_port:parseInt(document.getElementById('mqtt_port').value)||1883,mqtt_username:document.getElementById('mqtt_user').value,mqtt_password:document.getElementById('mqtt_pass').value,mqtt_topic_prefix:document.getElementById('mqtt_prefix').value||'ninadisplay'};}"
"function loadThresholds(d,i,rkey,hkey){"
"try{const r=JSON.parse(d[rkey]||'{}');document.getElementById('rms_good_max_'+i).value=r.good_max||0.5;document.getElementById('rms_ok_max_'+i).value=r.ok_max||1.0;document.getElementById('rms_good_color_'+i).value=r.good_color||'#10b981';document.getElementById('rms_ok_color_'+i).value=r.ok_color||'#eab308';document.getElementById('rms_bad_color_'+i).value=r.bad_color||'#ef4444';}catch(e){}"
"try{const h=JSON.parse(d[hkey]||'{}');document.getElementById('hfr_good_max_'+i).value=h.good_max||2.0;document.getElementById('hfr_ok_max_'+i).value=h.ok_max||3.5;document.getElementById('hfr_good_color_'+i).value=h.good_color||'#10b981';document.getElementById('hfr_ok_color_'+i).value=h.ok_color||'#eab308';document.getElementById('hfr_bad_color_'+i).value=h.bad_color||'#ef4444';}catch(e){}"
"}"
"function save(){"
"const b=document.getElementById('saveBtn');const ot=b.innerText;b.innerText='SYNCING...';b.disabled=true;"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(getConfigData())})"
".then(r=>{if(r.ok){b.innerText='SUCCESS';setTimeout(()=>{b.innerText=ot;b.disabled=false;},2000);}else{b.innerText='ERROR';setTimeout(()=>{b.innerText=ot;b.disabled=false;},2000);}}).catch(()=>{b.innerText='FAIL';b.disabled=false;});"
"}"
"function saveAndReboot(){"
"if(!confirm('Apply changes and reboot now?'))return;"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(getConfigData())}).then(r=>{if(r.ok)fetch('/api/reboot',{method:'POST'});});"
"}"
"function factoryReset(){if(confirm('Wipe all settings?')&&confirm('Confirm Reset?'))fetch('/api/factory-reset',{method:'POST'});}"
"fetch('/api/config').then(r=>r.json()).then(d=>{"
"document.getElementById('ssid').value=d.ssid||'';document.getElementById('pass').value=d.pass||'';document.getElementById('ntp').value=d.ntp||'';"
"const extractHost=u=>{if(!u)return '';const m=u.match(/^https?:\\/\\/([^:\\/]+)/);return m?m[1]:u;};"
"document.getElementById('url1').value=extractHost(d.url1);document.getElementById('url2').value=extractHost(d.url2);document.getElementById('url3').value=extractHost(d.url3||'');"
"document.getElementById('theme_select').value=d.theme_index||0;"
"var br=d.brightness!=null?d.brightness:50;document.getElementById('brightness').value=br;document.getElementById('bright_val').innerText=br+'%';"
"var cb=d.color_brightness!=null?d.color_brightness:100;document.getElementById('color_brightness').value=cb;document.getElementById('cbright_val').innerText=cb+'%';"
"try{filterColorsArr[0]=JSON.parse(d.filter_colors_1||'{}');}catch(e){}"
"try{filterColorsArr[1]=JSON.parse(d.filter_colors_2||'{}');}catch(e){}"
"try{filterColorsArr[2]=JSON.parse(d.filter_colors_3||'{}');}catch(e){}"
"renderFilterColors(0);renderFilterColors(1);renderFilterColors(2);"
"loadThresholds(d,0,'rms_thresholds_1','hfr_thresholds_1');"
"loadThresholds(d,1,'rms_thresholds_2','hfr_thresholds_2');"
"loadThresholds(d,2,'rms_thresholds_3','hfr_thresholds_3');"
"updateNodeLabels();"
"document.getElementById('mqtt_enabled').checked=!!d.mqtt_enabled;"
"const extractBrokerHost=u=>{if(!u)return '';const m=u.match(/^mqtts?:\\/\\/([^:\\/]+)/);return m?m[1]:u;};"
"document.getElementById('mqtt_broker').value=extractBrokerHost(d.mqtt_broker_url);"
"document.getElementById('mqtt_port').value=d.mqtt_port||1883;"
"document.getElementById('mqtt_user').value=d.mqtt_username||'';"
"document.getElementById('mqtt_pass').value=d.mqtt_password||'';"
"document.getElementById('mqtt_prefix').value=d.mqtt_topic_prefix||'ninadisplay';"
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
    cJSON_AddStringToObject(root, "filter_colors_1", cfg->filter_colors_1);
    cJSON_AddStringToObject(root, "filter_colors_2", cfg->filter_colors_2);
    cJSON_AddStringToObject(root, "filter_colors_3", cfg->filter_colors_3);
    cJSON_AddStringToObject(root, "rms_thresholds_1", cfg->rms_thresholds_1);
    cJSON_AddStringToObject(root, "rms_thresholds_2", cfg->rms_thresholds_2);
    cJSON_AddStringToObject(root, "rms_thresholds_3", cfg->rms_thresholds_3);
    cJSON_AddStringToObject(root, "hfr_thresholds_1", cfg->hfr_thresholds_1);
    cJSON_AddStringToObject(root, "hfr_thresholds_2", cfg->hfr_thresholds_2);
    cJSON_AddStringToObject(root, "hfr_thresholds_3", cfg->hfr_thresholds_3);
    cJSON_AddNumberToObject(root, "theme_index", cfg->theme_index);
    cJSON_AddNumberToObject(root, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(root, "color_brightness", cfg->color_brightness);
    cJSON_AddBoolToObject(root, "mqtt_enabled", cfg->mqtt_enabled);
    cJSON_AddStringToObject(root, "mqtt_broker_url", cfg->mqtt_broker_url);
    cJSON_AddNumberToObject(root, "mqtt_port", cfg->mqtt_port);
    cJSON_AddStringToObject(root, "mqtt_username", cfg->mqtt_username);
    cJSON_AddStringToObject(root, "mqtt_password", cfg->mqtt_password);
    cJSON_AddStringToObject(root, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);

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
    #define POST_BUF_SIZE 5120
    int remaining = req->content_len;

    if (remaining >= POST_BUF_SIZE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = malloc(POST_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    app_config_t *cfg = malloc(sizeof(app_config_t));
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    // Load current to preserve anything not sent
    memcpy(cfg, app_config_get(), sizeof(app_config_t));

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    if (cJSON_IsString(ssid)) {
        strncpy(cfg->wifi_ssid, ssid->valuestring, sizeof(cfg->wifi_ssid) - 1);
        cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    }

    cJSON *pass = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(pass)) {
        strncpy(cfg->wifi_pass, pass->valuestring, sizeof(cfg->wifi_pass) - 1);
        cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1] = '\0';
    }

    cJSON *url1 = cJSON_GetObjectItem(root, "url1");
    if (cJSON_IsString(url1)) {
        strncpy(cfg->api_url_1, url1->valuestring, sizeof(cfg->api_url_1) - 1);
        cfg->api_url_1[sizeof(cfg->api_url_1) - 1] = '\0';
    }

    cJSON *url2 = cJSON_GetObjectItem(root, "url2");
    if (cJSON_IsString(url2)) {
        strncpy(cfg->api_url_2, url2->valuestring, sizeof(cfg->api_url_2) - 1);
        cfg->api_url_2[sizeof(cfg->api_url_2) - 1] = '\0';
    }

    cJSON *url3 = cJSON_GetObjectItem(root, "url3");
    if (cJSON_IsString(url3)) {
        strncpy(cfg->api_url_3, url3->valuestring, sizeof(cfg->api_url_3) - 1);
        cfg->api_url_3[sizeof(cfg->api_url_3) - 1] = '\0';
    }

    cJSON *ntp = cJSON_GetObjectItem(root, "ntp");
    if (cJSON_IsString(ntp)) {
        strncpy(cfg->ntp_server, ntp->valuestring, sizeof(cfg->ntp_server) - 1);
        cfg->ntp_server[sizeof(cfg->ntp_server) - 1] = '\0';
    }

    cJSON *theme_index = cJSON_GetObjectItem(root, "theme_index");
    if (cJSON_IsNumber(theme_index)) {
        cfg->theme_index = theme_index->valueint;
    }

    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        int val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->brightness = val;
    }

    cJSON *filter_colors_1 = cJSON_GetObjectItem(root, "filter_colors_1");
    if (cJSON_IsString(filter_colors_1)) {
        strncpy(cfg->filter_colors_1, filter_colors_1->valuestring, sizeof(cfg->filter_colors_1) - 1);
        cfg->filter_colors_1[sizeof(cfg->filter_colors_1) - 1] = '\0';
    }
    cJSON *filter_colors_2 = cJSON_GetObjectItem(root, "filter_colors_2");
    if (cJSON_IsString(filter_colors_2)) {
        strncpy(cfg->filter_colors_2, filter_colors_2->valuestring, sizeof(cfg->filter_colors_2) - 1);
        cfg->filter_colors_2[sizeof(cfg->filter_colors_2) - 1] = '\0';
    }
    cJSON *filter_colors_3 = cJSON_GetObjectItem(root, "filter_colors_3");
    if (cJSON_IsString(filter_colors_3)) {
        strncpy(cfg->filter_colors_3, filter_colors_3->valuestring, sizeof(cfg->filter_colors_3) - 1);
        cfg->filter_colors_3[sizeof(cfg->filter_colors_3) - 1] = '\0';
    }

    cJSON *rms_thresholds_1 = cJSON_GetObjectItem(root, "rms_thresholds_1");
    if (cJSON_IsString(rms_thresholds_1)) {
        strncpy(cfg->rms_thresholds_1, rms_thresholds_1->valuestring, sizeof(cfg->rms_thresholds_1) - 1);
        cfg->rms_thresholds_1[sizeof(cfg->rms_thresholds_1) - 1] = '\0';
    }
    cJSON *rms_thresholds_2 = cJSON_GetObjectItem(root, "rms_thresholds_2");
    if (cJSON_IsString(rms_thresholds_2)) {
        strncpy(cfg->rms_thresholds_2, rms_thresholds_2->valuestring, sizeof(cfg->rms_thresholds_2) - 1);
        cfg->rms_thresholds_2[sizeof(cfg->rms_thresholds_2) - 1] = '\0';
    }
    cJSON *rms_thresholds_3 = cJSON_GetObjectItem(root, "rms_thresholds_3");
    if (cJSON_IsString(rms_thresholds_3)) {
        strncpy(cfg->rms_thresholds_3, rms_thresholds_3->valuestring, sizeof(cfg->rms_thresholds_3) - 1);
        cfg->rms_thresholds_3[sizeof(cfg->rms_thresholds_3) - 1] = '\0';
    }

    cJSON *hfr_thresholds_1 = cJSON_GetObjectItem(root, "hfr_thresholds_1");
    if (cJSON_IsString(hfr_thresholds_1)) {
        strncpy(cfg->hfr_thresholds_1, hfr_thresholds_1->valuestring, sizeof(cfg->hfr_thresholds_1) - 1);
        cfg->hfr_thresholds_1[sizeof(cfg->hfr_thresholds_1) - 1] = '\0';
    }
    cJSON *hfr_thresholds_2 = cJSON_GetObjectItem(root, "hfr_thresholds_2");
    if (cJSON_IsString(hfr_thresholds_2)) {
        strncpy(cfg->hfr_thresholds_2, hfr_thresholds_2->valuestring, sizeof(cfg->hfr_thresholds_2) - 1);
        cfg->hfr_thresholds_2[sizeof(cfg->hfr_thresholds_2) - 1] = '\0';
    }
    cJSON *hfr_thresholds_3 = cJSON_GetObjectItem(root, "hfr_thresholds_3");
    if (cJSON_IsString(hfr_thresholds_3)) {
        strncpy(cfg->hfr_thresholds_3, hfr_thresholds_3->valuestring, sizeof(cfg->hfr_thresholds_3) - 1);
        cfg->hfr_thresholds_3[sizeof(cfg->hfr_thresholds_3) - 1] = '\0';
    }

    cJSON *color_brightness = cJSON_GetObjectItem(root, "color_brightness");
    if (cJSON_IsNumber(color_brightness)) {
        int val = color_brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->color_brightness = val;
    }

    cJSON *mqtt_enabled = cJSON_GetObjectItem(root, "mqtt_enabled");
    if (cJSON_IsBool(mqtt_enabled)) {
        cfg->mqtt_enabled = cJSON_IsTrue(mqtt_enabled);
    }

    cJSON *mqtt_broker_url = cJSON_GetObjectItem(root, "mqtt_broker_url");
    if (cJSON_IsString(mqtt_broker_url)) {
        strncpy(cfg->mqtt_broker_url, mqtt_broker_url->valuestring, sizeof(cfg->mqtt_broker_url) - 1);
        cfg->mqtt_broker_url[sizeof(cfg->mqtt_broker_url) - 1] = '\0';
    }

    cJSON *mqtt_port = cJSON_GetObjectItem(root, "mqtt_port");
    if (cJSON_IsNumber(mqtt_port)) {
        cfg->mqtt_port = (uint16_t)mqtt_port->valueint;
    }

    cJSON *mqtt_username = cJSON_GetObjectItem(root, "mqtt_username");
    if (cJSON_IsString(mqtt_username)) {
        strncpy(cfg->mqtt_username, mqtt_username->valuestring, sizeof(cfg->mqtt_username) - 1);
        cfg->mqtt_username[sizeof(cfg->mqtt_username) - 1] = '\0';
    }

    cJSON *mqtt_password = cJSON_GetObjectItem(root, "mqtt_password");
    if (cJSON_IsString(mqtt_password)) {
        strncpy(cfg->mqtt_password, mqtt_password->valuestring, sizeof(cfg->mqtt_password) - 1);
        cfg->mqtt_password[sizeof(cfg->mqtt_password) - 1] = '\0';
    }

    cJSON *mqtt_topic_prefix = cJSON_GetObjectItem(root, "mqtt_topic_prefix");
    if (cJSON_IsString(mqtt_topic_prefix)) {
        strncpy(cfg->mqtt_topic_prefix, mqtt_topic_prefix->valuestring, sizeof(cfg->mqtt_topic_prefix) - 1);
        cfg->mqtt_topic_prefix[sizeof(cfg->mqtt_topic_prefix) - 1] = '\0';
    }

    app_config_save(cfg);
    free(cfg);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config saved to NVS");

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
        mqtt_ha_publish_state();
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
        mqtt_ha_publish_state();
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