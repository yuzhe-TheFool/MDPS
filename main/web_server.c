/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_lvgl_port.h"
#include "ina226.h"
#include "lm75a.h"
#include "fan.h"
#include "sw3538.h"
#include "power_ui.h"
#include "web_server.h"

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;

/* ===== 单页 HTML（内嵌，无需文件系统）
   注意：源码里避免出现双引号与反斜杠，全部用单引号，便于 C 字符串书写。 */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MDPS</title>"
"<style>"
"body{font-family:system-ui,Arial;background:#111;color:#eee;margin:0 auto;padding:12px;max-width:1020px}"
"h1{font-size:22px;margin:0;color:#4FC3F7;letter-spacing:1px}"
".sub{font-size:11px;color:#7d8fa3;margin:2px 0 12px}"
"h2{font-size:13px;margin:0 0 8px;color:#4FC3F7;letter-spacing:.5px}"
".card{background:#1b2430;border:1px solid #2c3a4a;border-radius:8px;padding:10px}"
"#wrap{display:grid;grid-template-columns:1fr;gap:10px}"
".row{display:flex;justify-content:space-between;align-items:center;padding:3px 0;font-size:14px}"
".k{color:#8fa6bb}.v{color:#fff;font-weight:600}"
".big{font-size:20px;color:#fff;font-weight:700}"
".ctl{display:flex;align-items:center;gap:6px}"
"input{width:82px;background:#0d1318;color:#fff;border:1px solid #3a4a5a;border-radius:6px;padding:6px;text-align:right}"
"button{background:#0e4a5a;color:#fff;border:1px solid #00e5ff55;border-radius:6px;padding:8px 12px;margin-right:8px}"
"button.st{padding:6px 0;margin:0;font-size:16px;line-height:1;min-width:36px}"
"button.on{background:#8c2b2b;border-color:#ff5252}"
"button.sel{background:#1565a0;border-color:#4FC3F7}"
"button:disabled,input:disabled{opacity:.3}"
".u{font-size:11px;color:#7d8fa3;width:16px}"
"table{width:100%;border-collapse:collapse;font-size:13px}"
"th,td{text-align:right;padding:4px 2px;border-bottom:1px solid #26313d}"
"th:first-child,td:first-child{text-align:left}"
".g{color:#00e676}.a{color:#ffa726}.r{color:#ff5252}"
"@media(min-width:820px){#wrap{grid-template-columns:1fr 1fr}#wrap .card:first-child{grid-column:1/-1}}"
"</style></head><body>"
"<h1>MDPS</h1><div class='sub'>Multifunction Desktop Power Supply</div>"
"<div id='wrap'>"
"<div class='card'><h2>USB Fast Charger</h2><table id='chg'></table></div>"
"<div class='card'><h2>Adjustable Supply</h2>"
"<div class='row'><span class='k'>Actual</span><span class='big' id='act'>--</span></div>"
"<div class='row'><span class='k'>Output</span><span class='v'><span id='enstate'>--</span> "
"<button class='st' id='enbtn' onclick='toggleEn()'>EN</button></span></div>"
"<div class='row'><span class='k'>Voltage</span><span class='ctl'>"
"<input id='sv' type='number' min='1' max='28' step='0.01'><span class='u'>V</span>"
"<button class='st' onclick='stepV(-1)'>-</button>"
"<button class='st' onclick='stepV(1)'>+</button></span></div>"
"<div class='row'><span class='k'>Current</span><span class='ctl'>"
"<input id='si' type='number' min='0.05' max='5' step='0.05'><span class='u'>A</span>"
"<button class='st' onclick='stepI(-1)'>-</button>"
"<button class='st' onclick='stepI(1)'>+</button></span></div></div>"
"<div class='card'><h2>Fan</h2>"
"<div class='row'><span class='k'>Status</span><span class='v' id='faninfo'>-</span></div>"
"<div class='row'><span class='k'>Temperature</span><span class='v' id='temp'>-</span></div>"
"<div class='row'><span class='k'>Speed</span><span class='ctl'>"
"<input id='fpct' type='number' min='0' max='100' step='5'><span class='u'>%</span>"
"<button class='st' id='fmdn' onclick='stepF(-1)'>-</button>"
"<button class='st' id='fmdp' onclick='stepF(1)'>+</button></span></div>"
"<div style='margin-top:8px'><button id='fman' onclick='setFan()'>Manual</button>"
"<button id='fauto' onclick='setFanAuto()'>Auto</button></div></div></div>"
"<script>"
"function q(i){return document.getElementById(i)}"
"function stamp(u){return u+(u.indexOf('?')<0?'?':'&')+'_='+Date.now()}"
"async function cmd(u){try{await fetch(stamp(u),{cache:'no-store'})}catch(e){}tick()}"
"async function tick(){"
" try{"
"  const r=await fetch(stamp('/api/status'),{cache:'no-store'});const j=await r.json();"
"  let h='<tr><th>Port</th><th>V</th><th>A</th><th>W</th></tr>';"
"  for(const c of j.chg){const w=c.ok?(c.v*c.i/1e6):0;"
"   h+=`<tr><td>${c.n}</td><td>${c.ok?(c.v/1000).toFixed(2):'--'}</td><td>${c.ok?(c.i/1000).toFixed(2):'--'}</td><td>${c.ok?w.toFixed(1):'--'}</td></tr>`;}"
"  q('chg').innerHTML=h;"
"  const cl=(v,mx)=>Math.min(mx,Math.max(0,v));"
"  const av=j.out_v<0?'--':cl(j.out_v/1000,28).toFixed(2)+' V';"
"  const ai=j.out_i<0?'--':cl(j.out_i/1000,5).toFixed(2)+' A';"
"  const aw=j.out_v<0?'--':cl(j.out_v*j.out_i/1e6,140).toFixed(2)+' W';"
"  q('act').textContent=av+' / '+ai+' / '+aw;"
"  q('enstate').innerHTML=j.en?`<span class='g'>ON</span>`:`<span class='r'>OFF</span>`;"
"  q('enbtn').className='st'+(j.en?' on':'');"
"  if(document.activeElement!==q('sv'))q('sv').value=(j.set_v/1000).toFixed(2);"
"  if(document.activeElement!==q('si'))q('si').value=(j.set_i/1000).toFixed(2);"
"  q('temp').textContent=j.temp10<-1000?'--':(j.temp10/10).toFixed(1)+' C';"
"  const fs=j.fan.rpm>0?j.fan.rpm+' RPM':(j.fan.state==2?'STALL':'stopped');"
"  q('faninfo').textContent=(j.fan.manual?'Manual ':'Auto ')+j.fan.duty+'% '+fs;"
"  q('fman').className=j.fan.manual?'sel':'';"
"  q('fauto').className=j.fan.manual?'':'sel';"
"  q('fpct').disabled=!j.fan.manual;q('fmdn').disabled=!j.fan.manual;q('fmdp').disabled=!j.fan.manual;"
"  if(document.activeElement!==q('fpct'))q('fpct').value=j.fan.duty;"
" }catch(e){}"
"}"
"function fv(){const v=parseFloat(q('sv').value);return isNaN(v)?null:Math.min(28,Math.max(1,v))}"
"function fi(){const v=parseFloat(q('si').value);return isNaN(v)?null:Math.min(5,Math.max(0.05,v))}"
"function pushSet(){const v=fv(),i=fi();if(v==null||i==null)return;cmd('/api/set?v='+Math.round(v*1000)+'&i='+Math.round(i*1000))}"
"function stepV(d){const e=q('sv'),c=fv();e.value=Math.min(28,Math.max(1,(c==null?1:c)+d*0.01)).toFixed(2);pushSet()}"
"function stepI(d){const e=q('si'),c=fi();e.value=Math.min(5,Math.max(0.05,(c==null?0.05:c)+d*0.05)).toFixed(2);pushSet()}"
"function stepF(d){const e=q('fpct');if(e.disabled)return;e.value=Math.min(100,Math.max(0,((+e.value)||0)+d*5));setFan()}"
"function toggleEn(){cmd('/api/set?en='+(q('enstate').textContent=='ON'?'0':'1'))}"
"function setFan(){cmd('/api/fan?pct='+q('fpct').value)}"
"function setFanAuto(){cmd('/api/fan?pct=auto')}"
"q('sv').onchange=pushSet;q('si').onchange=pushSet;q('fpct').onchange=setFan;"
"tick();setInterval(tick,1000);"
"</script></body></html>";

/* ---------- 处理器 ---------- */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/status → 全部状态（JSON） */
static esp_err_t h_status(httpd_req_t *req)
{
    int out_v = -1, out_i = -1;
    ina226_get_last(&out_v, &out_i);            /* 失败时保持 -1 */

    int set_v = 0, set_i = 0;
    bool en = false;
    power_ui_get_output(&set_v, &set_i, &en);

    int t10 = -10000;                            /* 无效值 */
    lm75a_get_temp(&t10);

    char *buf = malloc(1200);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    int n = snprintf(buf, 1200,
        "{\"out_v\":%d,\"out_i\":%d,\"set_v\":%d,\"set_i\":%d,\"en\":%d,"
        "\"temp10\":%d,"
        "\"fan\":{\"manual\":%d,\"duty\":%d,\"rpm\":%d,\"state\":%d},"
        "\"chg\":[",
        out_v, out_i, set_v, set_i, en ? 1 : 0,
        t10,
        fan_is_manual() ? 1 : 0, fan_get_duty(), fan_get_rpm(), fan_get_state());

    static const char *names[4] = {"C1", "A1", "C2", "A2"};
    for (int inst = 0; inst < SW3538_INST_NUM; inst++) {
        sw3538_data_t d = {0};
        bool ok = sw3538_get_last(inst, &d);
        for (int p = 0; p < 2; p++) {
            int idx = inst * 2 + p;
            int v_mv = ok ? d.outputVoltagemV : 0;
            int i_ma = ok ? (p == 0 ? d.currentPath1mA : d.currentPath2mA) : 0;
            n += snprintf(buf + n, 1200 - n,
                          "%s{\"n\":\"%s\",\"ok\":%d,\"v\":%d,\"i\":%d}",
                          idx == 0 ? "" : ",", names[idx], ok ? 1 : 0, v_mv, i_ma);
        }
    }
    if (n > 1160) {
        n = 1160;   /* 防御：避免后续 snprintf 尺寸为负 */
    }
    snprintf(buf + n, 1200 - n, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");   /* 防浏览器缓存状态 */
    esp_err_t r = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return r;
}

/* GET /api/set?v=<mV>&i=<mA>&en=<0|1> —— 参数都可省略 */
static esp_err_t h_set(httpd_req_t *req)
{
    char q[128] = {0};
    char val[24];
    int v = -1, i = -1, en = -1;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no query");
    }
    if (httpd_query_key_value(q, "v", val, sizeof(val)) == ESP_OK) {
        v = atoi(val);
    }
    if (httpd_query_key_value(q, "i", val, sizeof(val)) == ESP_OK) {
        i = atoi(val);
    }
    if (httpd_query_key_value(q, "en", val, sizeof(val)) == ESP_OK) {
        en = atoi(val);
    }

    ESP_LOGW(TAG, "api/set v=%d i=%d en=%d", v, i, en);

    /* power_ui_set_output 会更新 LVGL 控件 → 必须在 LVGL 锁内调用 */
    if (lvgl_port_lock(0)) {
        int cur_v = 0, cur_i = 0;
        bool cur_en = false;
        power_ui_get_output(&cur_v, &cur_i, &cur_en);
        bool new_en = (en < 0) ? cur_en : (en != 0);
        power_ui_set_output(v > 0 ? v : -1, i > 0 ? i : -1, new_en);
        lvgl_port_unlock();
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "ok");
}

/* GET /api/fan?pct=<0..100|auto> */
static esp_err_t h_fan(httpd_req_t *req)
{
    char q[64] = {0};
    char val[16];

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no query");
    }
    if (httpd_query_key_value(q, "pct", val, sizeof(val)) == ESP_OK) {
        ESP_LOGW(TAG, "api/fan pct=%s", val);
        if (strcmp(val, "auto") == 0) {
            fan_set_manual(-1);
        } else {
            fan_set_manual(atoi(val));
        }
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "ok");
}

void web_server_start(void)
{
    if (s_server) {
        return;   /* 已启动 */
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;   /* 连接数满时回收最久未用，避免 503 */
    cfg.recv_wait_timeout = 3;     /* 网络不佳时尽快释放僵死连接 */
    cfg.send_wait_timeout = 3;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return;
    }

    httpd_uri_t u_root = {.uri = "/",           .method = HTTP_GET, .handler = h_root};
    httpd_uri_t u_st   = {.uri = "/api/status", .method = HTTP_GET, .handler = h_status};
    httpd_uri_t u_set  = {.uri = "/api/set",    .method = HTTP_GET, .handler = h_set};
    httpd_uri_t u_fan  = {.uri = "/api/fan",    .method = HTTP_GET, .handler = h_fan};
    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_st);
    httpd_register_uri_handler(s_server, &u_set);
    httpd_register_uri_handler(s_server, &u_fan);

    /* WARN 级：当前全局日志级别下可见，便于确认已启动 */
    ESP_LOGW(TAG, "web server started on port 80");
}
