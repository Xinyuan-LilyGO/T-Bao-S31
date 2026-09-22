#include "web_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "actuator_control.h"
#include "esp_http_server.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "control_web";
constexpr size_t kMaxControlBody = 255;

httpd_handle_t s_server = nullptr;

constexpr char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-Bao-S31 Control</title>
<style>
:root{color-scheme:light;--ink:#101828;--muted:#667085;--line:#d0d5dd;--panel:#f8fafc;--blue:#175cd3;--green:#067647;--amber:#b54708;--red:#b42318}
*{box-sizing:border-box;letter-spacing:0}
body{margin:0;background:#eef2f6;color:var(--ink);font:15px/1.4 system-ui,-apple-system,"Segoe UI",sans-serif}
main{width:min(760px,100%);min-height:100vh;margin:auto;background:#fff;padding:18px}
header{display:flex;align-items:center;justify-content:space-between;gap:16px;border-bottom:1px solid var(--line);padding-bottom:14px}
h1{font-size:24px;line-height:1.15;margin:0} .sub{color:var(--muted);font-size:13px;margin-top:4px}
button{border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink);font:inherit;font-weight:650;min-height:42px;padding:0 16px;cursor:pointer}
button:active{transform:translateY(1px)} .danger{background:var(--red);border-color:var(--red);color:#fff}
.status{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 0;color:var(--muted)}
.status strong{color:var(--green)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.panel{border:1px solid var(--line);border-radius:8px;background:var(--panel);padding:16px}
.panel h2{font-size:16px;margin:0 0 14px}.readout{display:flex;align-items:baseline;justify-content:space-between;gap:12px;margin-bottom:8px}
.readout output{font-size:24px;font-weight:750;font-variant-numeric:tabular-nums}.hint{color:var(--muted);font-size:12px}
input[type=range]{width:100%;height:32px;accent-color:var(--blue)}
.scale{display:flex;justify-content:space-between;color:var(--muted);font-size:12px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:12px}
.switch{display:flex;align-items:center;gap:10px;font-weight:650}.switch input{width:22px;height:22px;accent-color:var(--blue)}
.servo{margin-top:12px}.footer{border-top:1px solid var(--line);color:var(--muted);font-size:12px;margin-top:16px;padding-top:12px;display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap}
.failsafe{color:var(--red)!important}
@media(max-width:600px){main{padding:14px}.grid{grid-template-columns:1fr}header{align-items:flex-start}h1{font-size:21px}.danger{padding:0 12px}}
</style>
</head>
<body>
<main>
<header>
  <div><h1>T-Bao-S31 Bench Control</h1><div class="sub">DRV8833 motors and ES9051 servo</div></div>
  <button id="stopAll" class="danger" type="button">STOP ALL</button>
</header>
<div class="status"><span>Device link</span><strong id="linkState">Connecting...</strong></div>
<section class="grid">
  <div class="panel">
    <h2>Motor A</h2>
    <div class="readout"><output id="motorAValue">0%</output><span class="hint">GPIO15 / GPIO14</span></div>
    <input id="motorA" type="range" min="-100" max="100" step="5" value="0">
    <div class="scale"><span>Reverse</span><span>Stop</span><span>Forward</span></div>
    <div class="row"><span class="hint">Signed PWM command</span><button id="zeroA" type="button">ZERO A</button></div>
  </div>
  <div class="panel">
    <h2>Motor B</h2>
    <div class="readout"><output id="motorBValue">0%</output><span class="hint">GPIO13 / GPIO12</span></div>
    <input id="motorB" type="range" min="-100" max="100" step="5" value="0">
    <div class="scale"><span>Reverse</span><span>Stop</span><span>Forward</span></div>
    <div class="row"><span class="hint">Signed PWM command</span><button id="zeroB" type="button">ZERO B</button></div>
  </div>
</section>
<section class="panel servo">
  <div class="row" style="margin-top:0"><h2 style="margin:0">ES9051 Servo</h2><label class="switch"><input id="servoEnabled" type="checkbox">PWM enabled</label></div>
  <div class="readout"><output id="servoValue">1500 us</output><span class="hint">GPIO5</span></div>
  <input id="servoUs" type="range" min="1100" max="1900" step="10" value="1500">
  <div class="scale"><span>1100 us</span><span>1500 us</span><span>1900 us</span></div>
  <div class="row"><span class="hint">50 Hz command pulse</span><button id="centerServo" type="button">CENTER</button></div>
</section>
<div class="footer"><span id="safetyState">2 s heartbeat failsafe</span><span>Local network test controller</span></div>
</main>
<script>
const el=id=>document.getElementById(id);
const controls={motorA:el('motorA'),motorB:el('motorB'),servoUs:el('servoUs'),servoEnabled:el('servoEnabled')};
let sendTimer=0,sending=false;
function paint(){el('motorAValue').value=`${controls.motorA.value}%`;el('motorBValue').value=`${controls.motorB.value}%`;el('servoValue').value=`${controls.servoUs.value} us`;}
function applyState(s){controls.motorA.value=s.motorA;controls.motorB.value=s.motorB;controls.servoUs.value=s.servoUs;controls.servoEnabled.checked=s.servoEnabled;paint();el('safetyState').textContent=s.failsafe?'FAILSAFE ACTIVE':'2 s heartbeat failsafe';el('safetyState').className=s.failsafe?'failsafe':'';}
function body(){return new URLSearchParams({motorA:controls.motorA.value,motorB:controls.motorB.value,servoUs:controls.servoUs.value,servoEnabled:controls.servoEnabled.checked?'1':'0'}).toString();}
async function sendControl(){if(sending)return;sending=true;try{const r=await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body(),cache:'no-store'});if(!r.ok)throw new Error();applyState(await r.json());el('linkState').textContent='Live';el('linkState').style.color='var(--green)';}catch(e){el('linkState').textContent='Control unavailable';el('linkState').style.color='var(--red)';}finally{sending=false;}}
function queueSend(){paint();clearTimeout(sendTimer);sendTimer=setTimeout(sendControl,80);}
Object.values(controls).forEach(node=>node.addEventListener('input',queueSend));
el('zeroA').onclick=()=>{controls.motorA.value=0;queueSend();};
el('zeroB').onclick=()=>{controls.motorB.value=0;queueSend();};
el('centerServo').onclick=()=>{controls.servoUs.value=1500;controls.servoEnabled.checked=true;queueSend();};
el('stopAll').onclick=async()=>{try{const r=await fetch('/api/stop',{method:'POST',cache:'no-store'});applyState(await r.json());}catch(e){};};
async function loadState(){try{const r=await fetch('/api/state',{cache:'no-store'});applyState(await r.json());el('linkState').textContent='Live';}catch(e){el('linkState').textContent='Offline';el('linkState').style.color='var(--red)';}}
setInterval(()=>{if(document.visibilityState==='visible')sendControl();},700);
window.addEventListener('pagehide',()=>navigator.sendBeacon('/api/stop',''));
paint();loadState();
</script>
</body>
</html>)HTML";

esp_err_t send_state(httpd_req_t *request)
{
    const ActuatorState state = actuator_control_get_state();
    char response[192];
    const int length = std::snprintf(
        response,
        sizeof(response),
        "{\"motorA\":%d,\"motorB\":%d,\"servoUs\":%d,"
        "\"servoEnabled\":%s,\"failsafe\":%s}",
        state.motor_a_percent,
        state.motor_b_percent,
        state.servo_pulse_us,
        state.servo_enabled ? "true" : "false",
        state.failsafe_active ? "true" : "false");
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

esp_err_t state_handler(httpd_req_t *request)
{
    return send_state(request);
}

bool parse_integer(
    const char *form,
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

esp_err_t control_handler(httpd_req_t *request)
{
    char body_buffer[kMaxControlBody + 1];
    if (receive_form(request, body_buffer, sizeof(body_buffer)) != ESP_OK) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "Invalid control body");
    }

    int motor_a = 0;
    int motor_b = 0;
    int servo_us = 0;
    int servo_enabled = 0;
    if (!parse_integer(
            body_buffer,
            "motorA",
            kMotorPercentMin,
            kMotorPercentMax,
            &motor_a) ||
        !parse_integer(
            body_buffer,
            "motorB",
            kMotorPercentMin,
            kMotorPercentMax,
            &motor_b) ||
        !parse_integer(
            body_buffer,
            "servoUs",
            kServoPulseMinUs,
            kServoPulseMaxUs,
            &servo_us) ||
        !parse_integer(
            body_buffer, "servoEnabled", 0, 1, &servo_enabled)) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "Control value out of range");
    }

    const esp_err_t err = actuator_control_apply(
        motor_a, motor_b, servo_us, servo_enabled != 0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Apply control failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "PWM update failed");
    }
    return send_state(request);
}

esp_err_t stop_handler(httpd_req_t *request)
{
    actuator_control_stop_all(false);
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

const httpd_uri_t kControlUri = {
    .uri = "/api/control",
    .method = HTTP_POST,
    .handler = control_handler,
    .user_ctx = nullptr,
};

const httpd_uri_t kStopUri = {
    .uri = "/api/stop",
    .method = HTTP_POST,
    .handler = stop_handler,
    .user_ctx = nullptr,
};

}  // namespace

esp_err_t web_server_start()
{
    if (s_server != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = nullptr;
        return err;
    }

    const httpd_uri_t *uris[] = {
        &kIndexUri,
        &kStateUri,
        &kControlUri,
        &kStopUri,
    };
    for (const httpd_uri_t *uri : uris) {
        err = httpd_register_uri_handler(s_server, uri);
        if (err != ESP_OK) {
            httpd_stop(s_server);
            s_server = nullptr;
            return err;
        }
    }

    ESP_LOGI(kTag, "HTTP control server started");
    return ESP_OK;
}

void web_server_stop()
{
    if (s_server == nullptr) {
        return;
    }
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(kTag, "HTTP control server stopped");
}
