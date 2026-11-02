#include "motion_web_server.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <inttypes.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "actuator_control.h"
#include "app_face_follow.h"
#include "motion_behavior.h"
#include "motion_manager.h"

namespace {

constexpr char kTag[] = "motion_web";
constexpr size_t kMaxMotionBody = 127;
httpd_handle_t s_server = nullptr;
std::atomic<uint32_t> s_web_epoch{0};
SemaphoreHandle_t s_web_mutex = nullptr;

bool lock_web_control()
{
    return s_web_mutex != nullptr &&
           xSemaphoreTake(s_web_mutex, portMAX_DELAY) == pdTRUE;
}

void unlock_web_control()
{
    xSemaphoreGive(s_web_mutex);
}

void invalidate_web_motion()
{
    const bool locked = lock_web_control();
    s_web_epoch.fetch_add(1, std::memory_order_acq_rel);
    motion_behavior_release_web();
    if (locked) {
        unlock_web_control();
    }
}

constexpr char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>T-Bao S31 · 移动控制</title>
<style>
:root{color-scheme:dark;--bg:#10161f;--panel:#182230;--line:#334155;--ink:#f8fafc;--muted:#9aa8ba;--blue:#4ea1ff;--green:#42d392;--red:#ff6b6b;--amber:#f6c453}
*{box-sizing:border-box;letter-spacing:0}
body{margin:0;min-height:100vh;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{width:min(760px,100%);min-height:100vh;margin:auto;padding:18px 16px 24px}
header{display:flex;align-items:flex-start;justify-content:space-between;gap:16px;border-bottom:1px solid var(--line);padding-bottom:14px}
h1{font-size:24px;line-height:1.15;margin:0}.eyebrow{color:var(--blue);font-size:12px;font-weight:700;letter-spacing:.08em;margin:0 0 5px;text-transform:uppercase}.sub{color:var(--muted);font-size:13px;margin:5px 0 0}
button{border:1px solid var(--line);border-radius:7px;background:#202d3d;color:var(--ink);font:inherit;font-weight:700;min-height:44px;padding:0 16px;cursor:pointer}button:active{transform:translateY(1px)}button:disabled{opacity:.55;cursor:wait}.stop{background:var(--red);border-color:var(--red);color:#220b0b;white-space:nowrap}
.status{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 0;color:var(--muted)}.status strong{color:var(--green)}.status strong.warn{color:var(--amber)}.status strong.bad{color:var(--red)}
.workspace{display:grid;grid-template-columns:minmax(280px,360px) 1fr;gap:18px;align-items:center}.panel{border:1px solid var(--line);border-radius:8px;background:var(--panel);padding:16px}.panel h2{font-size:16px;margin:0 0 12px}
.pad{position:relative;width:min(100%,340px);aspect-ratio:1;margin:auto;border:1px solid #50617a;border-radius:50%;background:#1c2939;overflow:hidden;touch-action:none;user-select:none;box-shadow:inset 0 0 0 14px #172232}
.pad:before,.pad:after{content:"";position:absolute;background:#33465e;opacity:.8}.pad:before{left:8%;right:8%;top:50%;height:1px}.pad:after{top:8%;bottom:8%;left:50%;width:1px}
.pad-label{position:absolute;color:#aebbd0;font-size:12px;font-weight:700;pointer-events:none}.pad-label.top{top:18px;left:50%;transform:translateX(-50%)}.pad-label.bottom{bottom:18px;left:50%;transform:translateX(-50%)}.pad-label.left{left:18px;top:50%;transform:translateY(-50%)}.pad-label.right{right:18px;top:50%;transform:translateY(-50%)}
.knob{position:absolute;left:50%;top:50%;width:27%;aspect-ratio:1;border:2px solid #b7dcff;border-radius:50%;background:var(--blue);box-shadow:0 5px 20px rgba(0,0,0,.35);transform:translate(-50%,-50%);pointer-events:none;transition:left .08s ease-out,top .08s ease-out}
.joystick-caption{text-align:center;color:var(--muted);font-size:12px;margin:10px 0 0}.control-panel{display:flex;flex-direction:column;gap:16px}.readout{display:flex;align-items:baseline;justify-content:space-between;gap:12px}.readout output{font-size:28px;font-weight:800;font-variant-numeric:tabular-nums}.hint{color:var(--muted);font-size:12px}.switch{display:flex;align-items:center;gap:7px;color:var(--muted);font-size:12px}.switch input{width:18px;height:18px;accent-color:var(--blue)}#centerServo{width:100%;min-height:38px;margin-top:10px}input[type=range]{width:100%;height:34px;accent-color:var(--blue)}.scale{display:flex;justify-content:space-between;color:var(--muted);font-size:12px}
.telemetry{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:18px}.metric{border-top:1px solid var(--line);padding-top:10px}.metric label{display:block;color:var(--muted);font-size:12px;margin-bottom:3px}.metric output{font-variant-numeric:tabular-nums}.notice{border-left:3px solid var(--amber);color:#f6d98b;font-size:12px;margin:18px 0 0;padding:2px 0 2px 10px}.footer{border-top:1px solid var(--line);color:var(--muted);font-size:12px;margin-top:18px;padding-top:12px}
@media(max-width:650px){main{padding:14px 12px 20px}.workspace{grid-template-columns:1fr;gap:14px}.pad{width:min(86vw,340px)}.control-panel{padding:14px}.telemetry{margin-top:14px}}
</style>
</head>
<body>
<main>
<header><div><p class="eyebrow">T-BAO S31 / MOTION</p><h1>桌面移动控制</h1><p class="sub">差速摇杆 · 浏览器实时续租 · 释放即停车</p></div><button id="stop" class="stop" type="button">停止全部</button></header>
<div class="status"><span>设备连接</span><strong id="link">连接中</strong></div>
<section class="workspace">
<div><div id="pad" class="pad" role="application" aria-label="小车移动摇杆"><span class="pad-label top">前</span><span class="pad-label bottom">后</span><span class="pad-label left">左</span><span class="pad-label right">右</span><span id="knob" class="knob"></span></div><p class="joystick-caption">按住并拖动摇杆，松开后自动停车</p></div>
  <div class="panel control-panel"><div><h2>速度上限</h2><div class="readout"><output id="speedValue">50%</output><span class="hint">由运动管理器执行加减速</span></div><input id="speed" type="range" min="10" max="100" step="5" value="50"><div class="scale"><span>慢</span><span>中</span><span>快</span></div></div><div><h2>头部点头</h2><div class="readout"><output id="servoValue">中心 1500 us</output><label class="switch"><input id="servoEnabled" type="checkbox">保持</label></div><input id="servoUs" type="range" min="1100" max="1900" step="10" value="1500"><div class="scale"><span>低头</span><span>中心</span><span>抬头</span></div><button id="centerServo" type="button">回中心</button></div><div class="readout"><span class="hint">摇杆指令</span><output id="vector">0 / 0</output></div><div class="readout"><span class="hint">电机输出</span><output id="motors">A 0% · B 0%</output></div></div>
</section>
<section class="telemetry"><div class="metric"><label>估算位置</label><output id="pose">--</output></div><div class="metric"><label>估算朝向</label><output id="heading">--</output></div><div class="metric"><label>当前来源</label><output id="source">--</output></div><div class="metric"><label>保护状态</label><output id="safety">--</output></div></section>
<p class="notice">软件桌面边界已关闭，位置仅用于观察，不能防止跌落。请只在桌面中央、远离边缘测试。</p>
<div class="footer">Web 控制服务：设备 IP 的 81 端口 · 连接中断后请求会自动过期</div>
</main>
<script>
const $=id=>document.getElementById(id),pad=$('pad'),knob=$('knob'),speed=$('speed'),servoUs=$('servoUs'),servoEnabled=$('servoEnabled');
let active=false,pointerId=null,x=0,y=0,webEpoch=0,motionInFlight=false,actionInFlight=false,motionPending=false,motionAbort=null,pendingAction='',servoControlOwned=false,stateRequestSerial=0;
function paintVector(){ $('vector').textContent=`${x} / ${y}`; $('speedValue').textContent=`${speed.value}%`; $('servoValue').textContent=`${servoEnabled.checked?'保持 ':'关闭 '}${servoUs.value} us`; }
function resetKnob(){x=0;y=0;knob.style.left='50%';knob.style.top='50%';paintVector();}
function setPoint(ev){const r=pad.getBoundingClientRect(),cx=r.left+r.width/2,cy=r.top+r.height/2,limit=r.width*.38;let dx=(ev.clientX-cx)/limit,dy=(cy-ev.clientY)/limit,len=Math.hypot(dx,dy);if(len>1){dx/=len;dy/=len}x=Math.round(dx*100);y=Math.round(dy*100);knob.style.left=`${50+dx*38}%`;knob.style.top=`${50-dy*38}%`;paintVector();}
function flushAction(){if(actionInFlight||!pendingAction){return}const action=pendingAction;pendingAction='';actionInFlight=true;fetch(action,{method:'POST',cache:'no-store'}).then(async r=>{if(!r.ok)throw new Error('action');const state=await r.json();applyState(state);if(action==='/api/release'||action==='/api/stop'){servoUs.value=1500;servoEnabled.checked=false;servoControlOwned=false;paintVector()}}).catch(()=>setLink('连接断开','bad')).finally(()=>{actionInFlight=false;if(pendingAction){flushAction()}else if((active||servoEnabled.checked)&&motionPending){motionPending=false;sendMotion()}})}
function queueAction(action){active=false;pointerId=null;motionPending=false;servoControlOwned=true;stateRequestSerial++;if(action==='/api/stop'||action==='/api/release'){servoEnabled.checked=false;paintVector()}if(motionAbort!==null)motionAbort.abort();pendingAction=action;resetKnob();if(!actionInFlight)flushAction()}
function sendMotion(){if(!active&&!servoEnabled.checked){return}servoControlOwned=true;stateRequestSerial++;if(motionInFlight||actionInFlight){motionPending=true;return}motionInFlight=true;const controller=new AbortController();motionAbort=controller;const body=new URLSearchParams({x:String(x),y:String(y),speed:String(speed.value),servoUs:String(servoUs.value),servoEnabled:servoEnabled.checked?'1':'0',epoch:String(webEpoch)});fetch('/api/motion',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body,cache:'no-store',signal:controller.signal}).then(async r=>{if(!r.ok)throw new Error('motion');const result=await r.json();if(result.ok===false){loadState();return}setLink('在线','')}).catch(e=>{if(e.name!=='AbortError')setLink('控制不可用','bad')}).finally(()=>{if(motionAbort===controller)motionAbort=null;motionInFlight=false;if(pendingAction){flushAction()}else if((active||servoEnabled.checked)&&motionPending){motionPending=false;sendMotion()}else{motionPending=false}})}
function releaseMotion(){active=false;pointerId=null;motionPending=false;if(motionAbort!==null)motionAbort.abort();resetKnob();if(servoEnabled.checked){sendMotion()}else{queueAction('/api/release')}}
function beaconRelease(){active=false;pointerId=null;motionPending=false;servoEnabled.checked=false;servoControlOwned=false;stateRequestSerial++;if(motionAbort!==null)motionAbort.abort();pendingAction='';resetKnob();navigator.sendBeacon('/api/release','');}
function setLink(text,kind){const node=$('link');node.textContent=text;node.className=kind||'';}
function applyState(s){if(typeof s.webEpoch==='number'&&s.webEpoch>=webEpoch){webEpoch=s.webEpoch}if(!s.available)setLink('运动不可用','bad');else if(s.faulted)setLink('故障锁定','bad');else if(s.failsafe)setLink('保护停车','warn');else setLink('在线','');$('motors').textContent=`A ${s.motorA}% · B ${s.motorB}%`;$('pose').textContent=`${Number(s.poseX).toFixed(1)}, ${Number(s.poseY).toFixed(1)} cm`;$('heading').textContent=`${s.headingDeg}°`;$('source').textContent=s.source||'空闲';$('safety').textContent=s.boundaryLimited?'边界限制':'软件边界关闭';if(!servoControlOwned){const nextServoUs=Number(s.servoUs);if(Number.isFinite(nextServoUs)&&nextServoUs>=1100&&nextServoUs<=1900){servoUs.value=nextServoUs}if(typeof s.servoEnabled==='boolean'){servoEnabled.checked=s.servoEnabled}}paintVector();}
pad.addEventListener('pointerdown',ev=>{active=true;pointerId=ev.pointerId;pad.setPointerCapture(pointerId);setPoint(ev);sendMotion()});pad.addEventListener('pointermove',ev=>{if(active&&ev.pointerId===pointerId){setPoint(ev)}});pad.addEventListener('pointerup',releaseMotion);pad.addEventListener('pointercancel',releaseMotion);pad.addEventListener('lostpointercapture',()=>{if(active)releaseMotion()});speed.addEventListener('input',()=>{paintVector();if(active||servoEnabled.checked)sendMotion()});servoUs.addEventListener('input',()=>{servoControlOwned=true;servoEnabled.checked=true;paintVector();sendMotion()});servoEnabled.addEventListener('change',()=>{servoControlOwned=true;paintVector();if(servoEnabled.checked)sendMotion();else queueAction('/api/release')});$('centerServo').addEventListener('click',()=>{servoControlOwned=true;servoUs.value=1500;servoEnabled.checked=true;paintVector();sendMotion()});$('stop').addEventListener('click',()=>queueAction('/api/stop'));setInterval(()=>{if(active||servoEnabled.checked)sendMotion()},50);setInterval(loadState,250);document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='hidden')beaconRelease()});window.addEventListener('pagehide',beaconRelease);async function loadState(){const requestSerial=++stateRequestSerial;try{const r=await fetch('/api/state',{cache:'no-store'});if(!r.ok)throw new Error('state');if(requestSerial!==stateRequestSerial)return;applyState(await r.json())}catch(e){if(e.name!=='AbortError')setLink('连接失败','bad')}}paintVector();loadState();
</script>
</body>
</html>)HTML";

const char *source_name(motion_source_t source)
{
    switch (source) {
    case MOTION_SOURCE_WEB:
        return "Web";
    case MOTION_SOURCE_TOUCH:
        return "Touch";
    case MOTION_SOURCE_MCP:
        return "MCP";
    case MOTION_SOURCE_VOICE:
        return "Voice";
    case MOTION_SOURCE_EXPRESSION:
        return "Expression";
    case MOTION_SOURCE_SAFETY:
        return "Safety";
    case MOTION_SOURCE_FOLLOW:
        return "Follow";
    default:
        return "Idle";
    }
}

esp_err_t send_state(httpd_req_t *request)
{
    motion_state_t state = {};
    motion_manager_get_state(&state);
    const int heading = static_cast<int>(std::lround(
        state.pose.theta_rad * 180.0 / 3.14159265358979323846));
    char response[448];
    const int length = std::snprintf(
        response,
        sizeof(response),
        "{\"available\":%s,\"faulted\":%s,\"failsafe\":%s,"
        "\"webEpoch\":%" PRIu32 ",\"boundaryLimited\":%s,"
        "\"motorA\":%d,\"motorB\":%d,"
        "\"servoUs\":%d,\"servoEnabled\":%s,"
        "\"poseX\":%.1f,\"poseY\":%.1f,\"headingDeg\":%d,"
        "\"source\":\"%s\"}",
        state.available ? "true" : "false",
        state.faulted ? "true" : "false",
        state.failsafe_active ? "true" : "false",
        s_web_epoch.load(std::memory_order_relaxed),
        state.boundary_limited ? "true" : "false",
        state.current_motor_a_percent,
        state.current_motor_b_percent,
        state.current_servo_pulse_us,
        state.servo_enabled ? "true" : "false",
        static_cast<double>(state.pose.x_cm),
        static_cast<double>(state.pose.y_cm),
        heading,
        source_name(state.active_source));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(response)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "State response too large");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, length);
}

esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(
        request,
        "Content-Security-Policy",
        "default-src 'self'; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'; connect-src 'self'");
    return httpd_resp_send(request, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t receive_form(httpd_req_t *request, char *buffer, size_t capacity)
{
    if (request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int result = httpd_req_recv(
            request,
            buffer + received,
            request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += static_cast<size_t>(result);
    }
    buffer[received] = '\0';
    return ESP_OK;
}

bool parse_integer(const char *form,
                   const char *key,
                   int minimum,
                   int maximum,
                   int *result)
{
    char value[16];
    if (httpd_query_key_value(form, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *result = static_cast<int>(parsed);
    return true;
}

bool parse_uint32(const char *form, const char *key, uint32_t *result)
{
    char value[16];
    if (httpd_query_key_value(form, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *result = static_cast<uint32_t>(parsed);
    return true;
}

esp_err_t send_motion_ack(httpd_req_t *request, bool accepted)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request,
                           accepted ? "{\"ok\":true}"
                                    : "{\"ok\":false,\"stale\":true}",
                           HTTPD_RESP_USE_STRLEN);
}

esp_err_t motion_handler(httpd_req_t *request)
{
    char body[kMaxMotionBody + 1];
    if (receive_form(request, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid motion body");
    }

    int x = 0;
    int y = 0;
    int speed = 0;
    int servo_us = ACTUATOR_SERVO_PULSE_CENTER_US;
    int servo_enabled = 0;
    uint32_t epoch = 0;
    if (!parse_integer(body, "x", -100, 100, &x) ||
        !parse_integer(body, "y", -100, 100, &y) ||
        !parse_integer(body, "speed", 0, 100, &speed) ||
        !parse_integer(body, "servoUs", ACTUATOR_SERVO_PULSE_MIN_US,
                       ACTUATOR_SERVO_PULSE_MAX_US, &servo_us) ||
        !parse_integer(body, "servoEnabled", 0, 1, &servo_enabled) ||
        !parse_uint32(body, "epoch", &epoch)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Motion value out of range");
    }

    if (!lock_web_control()) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Web control unavailable");
    }
    const bool accepted = epoch == s_web_epoch.load(std::memory_order_acquire);
    if (accepted) {
        const float speed_scale = static_cast<float>(speed) / 100.0f;
        const float linear = static_cast<float>(y) / 100.0f * speed_scale;
        const float angular = -static_cast<float>(x) / 100.0f * speed_scale;
        motion_behavior_submit_web_control(linear, angular, servo_us,
                                            servo_enabled != 0);
    }
    unlock_web_control();
    if (!accepted) {
        return send_motion_ack(request, false);
    }
    return send_motion_ack(request, true);
}

esp_err_t state_handler(httpd_req_t *request)
{
    return send_state(request);
}

esp_err_t release_handler(httpd_req_t *request)
{
    invalidate_web_motion();
    return send_state(request);
}

esp_err_t stop_handler(httpd_req_t *request)
{
    invalidate_web_motion();
    (void)app_face_follow_stop();
    motion_manager_stop_all();
    return send_state(request);
}

const httpd_uri_t kIndexUri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = nullptr,
};

const httpd_uri_t kStateUri = {
    .uri = "/api/state",
    .method = HTTP_GET,
    .handler = state_handler,
    .user_ctx = nullptr,
};

const httpd_uri_t kMotionUri = {
    .uri = "/api/motion",
    .method = HTTP_POST,
    .handler = motion_handler,
    .user_ctx = nullptr,
};

const httpd_uri_t kReleaseUri = {
    .uri = "/api/release",
    .method = HTTP_POST,
    .handler = release_handler,
    .user_ctx = nullptr,
};

const httpd_uri_t kStopUri = {
    .uri = "/api/stop",
    .method = HTTP_POST,
    .handler = stop_handler,
    .user_ctx = nullptr,
};

}  // namespace

extern "C" esp_err_t motion_web_server_start(void)
{
    if (s_server != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (s_web_mutex == nullptr) {
        s_web_mutex = xSemaphoreCreateMutex();
        if (s_web_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    config.server_port = 81;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(kTag, "HTTP control server start failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t *uris[] = {
        &kIndexUri,
        &kStateUri,
        &kMotionUri,
        &kReleaseUri,
        &kStopUri,
    };
    for (const httpd_uri_t *uri : uris) {
        err = httpd_register_uri_handler(s_server, uri);
        if (err != ESP_OK) {
            httpd_stop(s_server);
            s_server = nullptr;
            ESP_LOGE(kTag, "HTTP control route registration failed: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    char ip[16] = {};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
        }
    }
    ESP_LOGI(kTag, "motion web control: http://%s:81/",
             ip[0] != '\0' ? ip : "<sta-ip>");
    return ESP_OK;
}

extern "C" void motion_web_server_stop(void)
{
    invalidate_web_motion();
    if (s_server == nullptr) {
        return;
    }
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(kTag, "HTTP control server stopped");
}
