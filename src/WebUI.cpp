#include "WebUI.h"
#include "Config.h"
#include <WiFi.h>

// ---------------------------------------------------------------------
//  HTML template. Placeholders {{KEY}} are substituted on each request.
// ---------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang='ko'>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP32 Radio Setup</title>
<style>
  body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;max-width:680px;margin:20px auto;padding:0 14px;background:#fafafa;color:#222}
  h1{font-size:1.4em;margin-bottom:4px}
  h2{font-size:1.1em;margin:28px 0 4px;color:#333;border-bottom:1px solid #ddd;padding-bottom:4px}
  .sub{color:#666;font-size:0.9em;margin-bottom:18px}
  .status{padding:10px 12px;background:#e8f4ff;border-radius:6px;margin-bottom:20px;font-size:0.9em;line-height:1.5}
  label{display:block;margin-top:14px;font-weight:600;font-size:0.95em}
  input,textarea,select{width:100%;padding:8px 10px;font-size:14px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;margin-top:4px}
  textarea{height:240px;font-family:Consolas,Menlo,monospace;font-size:13px;line-height:1.45}
  .hint{color:#888;font-size:0.85em;margin-top:3px}
  button{padding:12px 24px;font-size:15px;margin-top:22px;cursor:pointer;background:#0066cc;color:#fff;border:none;border-radius:6px}
  button:hover{background:#0055aa}
  .row2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
</style>
</head>
<body>
<h1>ESP32-S3 Web Radio</h1>
<div class='sub'>설정 변경 후 저장하면 자동 재부팅됩니다.</div>
<div class='status' id='status'>Loading status...</div>
<form action='/save' method='POST'>

  <h2>WiFi / 시스템</h2>
  <label>WiFi SSID</label>
  <input name='ssid' value='{{SSID}}' required maxlength='32'>

  <label>WiFi Password</label>
  <input name='pass' type='password' value='{{PASS}}' maxlength='64'>
  <div class='hint'>개방 네트워크면 비워두기.</div>

  <label>Timezone (POSIX TZ)</label>
  <input name='tz' value='{{TZ}}' maxlength='32'>
  <div class='hint'>예: KST-9, EST5EDT, CET-1CEST,M3.5.0,M10.5.0/3</div>

  <label>Radio proxy URL</label>
  <input name='proxy' value='{{PROXY}}' maxlength='128'>
  <div class='hint'>HLS 방송국용 PC 프록시. 정각알림음도 이 서버를 통해 재생됨.</div>

  <h2>볼륨</h2>
  <label>최대 볼륨 (0 ~ 21, 프록시 스트림용)</label>
  <input name='max_vol' type='number' min='1' max='21' value='{{MAX_VOL}}'>
  <div class='hint'>프록시 경유 스트림(한국 라디오)의 최대 볼륨. MAX98357A가 지지직거리면 낮추세요 (권장 10~12). 음량 보충은 proxy 서버 GAIN_DB 환경변수로.</div>

  <label>직접 스트림 볼륨 부스트 (0=끔, 0~15)</label>
  <input name='dir_boost' type='number' min='0' max='15' value='{{DIR_BOOST}}'>
  <div class='hint'>프록시를 거치지 않는 직접 스트림(SomaFM 등)은 ffmpeg 게인이 없어 조용함. 이 값을 현재 볼륨에 더해 보정 (예: 볼륨10 + 부스트8 → 실제18). 기본값 8.</div>

  <h2>정각 알림음</h2>
  <label>재생 모드</label>
  <select name='chime_mode'>
    <option value='0' {{CM_0}}>끔</option>
    <option value='1' {{CM_1}}>시계 모드에서만</option>
    <option value='2' {{CM_2}}>라디오 모드에서만</option>
    <option value='3' {{CM_3}}>시계 + 라디오 모드 모두</option>
  </select>
  <div class='hint'>매 시 정각 10초 전(HH:59:50)에 proxy 서버의 /chime 에서 MP3 재생. proxy 폴더에 chime.mp3 파일이 있어야 함. 라디오 모드 재생 시 현재 방송이 잠시 멈추고 시보 후 자동 재개.</div>

  <label>정각 알림 볼륨 (0~21)</label>
  <input name='chime_vol' type='number' min='0' max='21' value='{{CHIME_VOL}}'>
  <div class='row2'>
    <div>
      <label>방해금지 시작</label>
      <input name='chime_ms' type='time' value='{{CHIME_MS}}' required>
    </div>
    <div>
      <label>방해금지 종료</label>
      <input name='chime_me' type='time' value='{{CHIME_ME}}' required>
    </div>
  </div>
  <div class='hint'>이 시간대에는 정각 알림음 미재생. 기본 23:00~08:00. 같은 시각으로 설정하면 방해금지 비활성.</div>

  <h2>화면</h2>
  <label>방송국 이름 폰트 크기</label>
  <select name='station_fs'>
    <option value='16' {{FS_16}}>16 px - 작게 (unifont, 가는 글씨)</option>
    <option value='24' {{FS_24}}>24 px - 크게 (D2Coding, 굵은 글씨)</option>
  </select>

  <h2>Home Assistant / MQTT</h2>
  <label>MQTT Broker IP (비워두면 비활성)</label>
  <input name='mqtt_br' value='{{MQTT_BR}}' maxlength='64' placeholder='192.168.10.x'>
  <div class='hint'>HomeAssistant Mosquitto 브로커 IP. 설정하면 HA에 자동으로 4개 엔티티가 등록됩니다.</div>

  <div class='row2'>
    <div>
      <label>Port</label>
      <input name='mqtt_port' type='number' min='1' max='65535' value='{{MQTT_PORT}}'>
    </div>
    <div>
      <label>Device ID (토픽 prefix)</label>
      <input name='mqtt_dev' value='{{MQTT_DEV}}' maxlength='32'>
      <div class='hint'>예: silverline1</div>
    </div>
  </div>
  <div class='row2'>
    <div>
      <label>Username (선택)</label>
      <input name='mqtt_user' value='{{MQTT_USER}}' maxlength='64'>
    </div>
    <div>
      <label>Password (선택)</label>
      <input name='mqtt_pass' type='password' value='{{MQTT_PASS}}' maxlength='64'>
    </div>
  </div>

  <h2>디버그</h2>
  <label><input type='checkbox' name='diag' value='1' {{DIAG_CHECKED}}> Diagnostic mode (verbose serial log)</label>
  <div class='hint'>켜면 10초마다 상세 health 로그. 평소엔 끔.</div>

  <h2>방송국 목록</h2>
  <label>Stations (한 줄에 한 개: 이름|URL)</label>
  <textarea name='stations'>{{STATIONS}}</textarea>
  <div class='hint'>최대 20개. '#'으로 시작하는 줄은 주석.</div>

  <button type='submit'>저장하고 재부팅</button>
</form>

<hr style='margin:40px 0 20px;border:none;border-top:1px solid #ddd'>
<h3 style='font-size:1.05em;color:#a33'>Factory Reset</h3>
<p style='font-size:0.88em;color:#666;line-height:1.5'>
  NVS에 저장된 모든 설정을 지우고 setup 모드로 재진입합니다.
</p>
<form action='/factory_reset' method='POST'
      onsubmit='return confirm("모든 설정을 지우고 setup 모드로 재부팅합니다. 계속하시겠습니까?");'>
  <button type='submit' style='background:#a33'>모든 설정 지우고 재부팅 (Factory Reset)</button>
</form>

<script>
fetch('/api/status').then(function(r){return r.json();}).then(function(s){
  document.getElementById('status').innerHTML =
    '<b>IP:</b> ' + s.ip + ' &nbsp; <b>RSSI:</b> ' + s.rssi + ' dBm &nbsp; <b>Uptime:</b> ' + s.uptime + 's<br>' +
    '<b>Status:</b> ' + s.status;
}).catch(function(e){ document.getElementById('status').textContent = 'status error'; });
</script>
</body>
</html>
)HTML";

// ---------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------
static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;";  break;
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:  out += c;
    }
  }
  return out;
}

static void substitute(String& tpl, const char* key, const String& value) {
  String marker = String("{{") + key + "}}";
  tpl.replace(marker, htmlEscape(value));
}

// ---------------------------------------------------------------------
//  WebUI
// ---------------------------------------------------------------------
void WebUI::begin(Config* cfg, bool captivePortal) {
  _cfg = cfg;
  _captive = captivePortal;
  _status = captivePortal ? "setup mode" : "ready";

  _server.on("/",            HTTP_GET,  [this](){ handleRoot(); });
  _server.on("/save",        HTTP_POST, [this](){ handleSave(); });
  _server.on("/factory_reset", HTTP_POST, [this](){ handleFactoryReset(); });
  _server.on("/api/status",  HTTP_GET,  [this](){ handleStatus(); });

  if (_captive) {
    auto redir = [this](){ handleCaptiveRedirect(); };
    _server.on("/generate_204",          HTTP_GET, redir);
    _server.on("/gen_204",               HTTP_GET, redir);
    _server.on("/hotspot-detect.html",   HTTP_GET, redir);
    _server.on("/library/test/success.html", HTTP_GET, redir);
    _server.on("/ncsi.txt",              HTTP_GET, redir);
    _server.on("/connecttest.txt",       HTTP_GET, redir);
    _server.on("/canonical.html",        HTTP_GET, redir);
    _server.on("/redirect",              HTTP_GET, redir);
  }

  _server.onNotFound([this](){ handleNotFound(); });
  _server.begin();
}

void WebUI::handle() {
  _server.handleClient();
}

void WebUI::handleRoot() {
  if (!_cfg) { _server.send(500, "text/plain", "no config"); return; }

  String html(FPSTR(INDEX_HTML));
  substitute(html, "SSID",         _cfg->wifiSSID);
  substitute(html, "PASS",         _cfg->wifiPass);
  substitute(html, "TZ",           _cfg->tzInfo);
  substitute(html, "PROXY",        _cfg->radioProxy);
  substitute(html, "MAX_VOL",      String(_cfg->maxVolume));
  substitute(html, "DIR_BOOST",    String(_cfg->directStreamBoost));
  substitute(html, "CHIME_VOL",    String(_cfg->chimeVolume));
  substitute(html, "CM_0", _cfg->chimeMode == 0 ? "selected" : "");
  substitute(html, "CM_1", _cfg->chimeMode == 1 ? "selected" : "");
  substitute(html, "CM_2", _cfg->chimeMode == 2 ? "selected" : "");
  substitute(html, "CM_3", _cfg->chimeMode == 3 ? "selected" : "");
  {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u",
             _cfg->chimeMuteStartMin / 60, _cfg->chimeMuteStartMin % 60);
    substitute(html, "CHIME_MS", String(buf));
    snprintf(buf, sizeof(buf), "%02u:%02u",
             _cfg->chimeMuteEndMin / 60, _cfg->chimeMuteEndMin % 60);
    substitute(html, "CHIME_ME", String(buf));
  }
  substitute(html, "FS_16",        _cfg->stationFontSize == 16 ? "selected" : "");
  substitute(html, "FS_24",        _cfg->stationFontSize >= 24 ? "selected" : "");
  substitute(html, "DIAG_CHECKED", _cfg->diagMode ? "checked" : "");
  substitute(html, "MQTT_BR",   _cfg->mqttBroker);
  substitute(html, "MQTT_PORT", String(_cfg->mqttPort));
  substitute(html, "MQTT_DEV",  _cfg->mqttDeviceId);
  substitute(html, "MQTT_USER", _cfg->mqttUser);
  substitute(html, "MQTT_PASS", _cfg->mqttPass);
  substitute(html, "STATIONS",     _cfg->stationsToText());

  _server.send(200, "text/html; charset=utf-8", html);
}

void WebUI::handleSave() {
  if (!_cfg) { _server.send(500, "text/plain", "no config"); return; }

  if (_server.hasArg("ssid"))  _cfg->wifiSSID   = _server.arg("ssid");
  if (_server.hasArg("pass"))  _cfg->wifiPass   = _server.arg("pass");
  if (_server.hasArg("tz"))    _cfg->tzInfo     = _server.arg("tz");
  if (_server.hasArg("proxy")) _cfg->radioProxy = _server.arg("proxy");

  auto parseHHMM = [&](const char* arg, uint16_t fallback) -> uint16_t {
    if (!_server.hasArg(arg)) return fallback;
    String s = _server.arg(arg);
    int c = s.indexOf(':');
    if (c < 1) return fallback;
    int h = s.substring(0, c).toInt();
    int m = s.substring(c + 1).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
    return (uint16_t)(h * 60 + m);
  };

  if (_server.hasArg("max_vol")) {
    int v = _server.arg("max_vol").toInt();
    if (v < 1)  v = 1;
    if (v > 21) v = 21;
    _cfg->maxVolume = (uint8_t)v;
  }
  if (_server.hasArg("dir_boost")) {
    int v = _server.arg("dir_boost").toInt();
    if (v < 0)  v = 0;
    if (v > 15) v = 15;
    _cfg->directStreamBoost = (uint8_t)v;
  }
  if (_server.hasArg("chime_vol")) {
    int v = _server.arg("chime_vol").toInt();
    if (v < 0)  v = 0;
    if (v > 21) v = 21;
    _cfg->chimeVolume = (uint8_t)v;
  }
  if (_server.hasArg("chime_mode")) {
    int v = _server.arg("chime_mode").toInt();
    if (v < 0 || v > 3) v = 1;
    _cfg->chimeMode = (uint8_t)v;
  }
  _cfg->chimeMuteStartMin = parseHHMM("chime_ms", _cfg->chimeMuteStartMin);
  _cfg->chimeMuteEndMin   = parseHHMM("chime_me", _cfg->chimeMuteEndMin);

  if (_server.hasArg("station_fs")) {
    int v = _server.arg("station_fs").toInt();
    _cfg->stationFontSize = (v == 16) ? 16 : 24;
  }

  _cfg->diagMode = _server.hasArg("diag") && _server.arg("diag") == "1";

  if (_server.hasArg("mqtt_br"))   _cfg->mqttBroker   = _server.arg("mqtt_br");
  if (_server.hasArg("mqtt_port")) {
    int v = _server.arg("mqtt_port").toInt();
    if (v > 0 && v < 65536) _cfg->mqttPort = (uint16_t)v;
  }
  if (_server.hasArg("mqtt_dev"))  _cfg->mqttDeviceId = _server.arg("mqtt_dev");
  if (_server.hasArg("mqtt_user")) _cfg->mqttUser     = _server.arg("mqtt_user");
  if (_server.hasArg("mqtt_pass")) _cfg->mqttPass     = _server.arg("mqtt_pass");

  if (_server.hasArg("stations")) {
    _cfg->parseStationsText(_server.arg("stations"));
  }

  _cfg->saveToNvs();

  _server.send(200, "text/html; charset=utf-8",
    "<html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='3;url=/'></head>"
    "<body style='font-family:sans-serif;padding:30px'>"
    "<h2>저장 완료. 재부팅 중...</h2></body></html>");

  delay(500);
  ESP.restart();
}

void WebUI::handleFactoryReset() {
  if (_cfg) _cfg->clearNvs();
  _server.send(200, "text/html; charset=utf-8",
    "<html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='3;url=/'></head>"
    "<body style='font-family:sans-serif;padding:30px'>"
    "<h2>Factory Reset. 재부팅 중...</h2></body></html>");
  delay(500);
  ESP.restart();
}

void WebUI::handleStatus() {
  String json = "{\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"rssi\":";
  json += String(WiFi.RSSI());
  json += ",\"uptime\":";
  json += String(millis() / 1000);
  json += ",\"status\":\"";
  json += htmlEscape(_status);
  json += "\"}";
  _server.send(200, "application/json", json);
}

void WebUI::handleCaptiveRedirect() {
  _server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  _server.send(302, "text/plain", "");
}

void WebUI::handleNotFound() {
  if (_captive) {
    handleCaptiveRedirect();
  } else {
    _server.send(404, "text/plain", "Not found");
  }
}

// setStatusText is defined inline in WebUI.h
