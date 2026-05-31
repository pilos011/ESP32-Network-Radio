// =====================================================================
//  ESP32-S3 N16R8 Internet Web Radio
//
//  Diagnostic / verbose mode is now a *runtime* setting, toggleable
//  from the web setup page (http://<radio-ip>/). Effects when on:
//    - Verbose serial logging at every step
//    - No auto-restart on WiFi failure or low heap
//    - 10-second periodic health snapshot (vs 60 s in production)
//
//  pins_config.h still has a DIAG_MODE macro -- but it's only used as
//  the *first-boot* default before NVS has any saved settings, and as
//  the first-boot station list switch in stations.h. Once you save
//  via the web UI the NVS value wins and the macro is irrelevant.
//
//  Controls:
//    LEFT  short -> volume -1
//    LEFT  long  -> previous station
//    RIGHT short -> volume +1
//    RIGHT long  -> next station
//    LEFT + RIGHT held 1 s -> system ON / OFF (soft)
// =====================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <PubSubClient.h>    // MQTT for Home Assistant integration

#include "pins_config.h"
#include "credentials.h"
#include "StreamPlayer.h"
#include "DisplayUI.h"
#include "RadioStationManager.h"
#include "ButtonControl.h"
#include "stations.h"
#include "Config.h"
#include "WebUI.h"

// ---------- DIAG: runtime-controlled via web UI -----------------------
//
// Was a compile-time #if; now a global bool that main.cpp keeps in
// sync with config.diagMode after Config::loadFromNvs(). The compile-
// time DIAG_MODE macro in pins_config.h is still used as the very
// first-boot default (before NVS is loaded), so existing builds keep
// behaving the same on flash-erase.
//
// LOG = always on (essential lines incl. boot/wifi/stream).
// DIAG = only when diag mode is enabled.
static bool g_diagMode = (DIAG_MODE != 0);

#define DIAG(fmt, ...)   do { if (g_diagMode) Serial.printf("[DIAG] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOG(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)

// Health-snapshot period: 10 s when diag is on, 60 s otherwise. Read
// at the point of comparison so toggling diag from the web UI takes
// effect immediately (the next save reboots us anyway, but this also
// covers any future hot-toggle path).
static inline unsigned long healthPeriodMs() {
  return g_diagMode ? 10000UL : 60000UL;
}

// ---------- Runtime mode ---------------------------------------------
//
// MODE_STA       - normal radio operation (WiFi connected)
// MODE_SETUP_AP  - SoftAP + captive portal for first-boot / WiFi setup
//
// Audio / NTP / stream are only started in MODE_STA. In MODE_SETUP_AP
// loop() runs DNS + web UI only.
enum RuntimeMode {
  MODE_STA,
  MODE_SETUP_AP,
};
static RuntimeMode  runtimeMode = MODE_STA;
static DNSServer    dnsServer;

// ---------- Objects ---------------------------------------------------
//
// Order matters: stationManager holds a pointer to config, so config
// must be constructed first. C++ guarantees same-TU global construction
// order matches declaration order, so we're safe here.
Config               config;
StreamPlayer         player(I2S_BCLK, I2S_LRC, I2S_DOUT);
DisplayUI            display;
RadioStationManager  stationManager(&config);
ButtonControl        buttons(BTN_LEFT, BTN_RIGHT);
Preferences          prefs;
WebUI                webUI;

// ---------- Runtime state --------------------------------------------
static uint8_t  currentVolume = 12;
static bool     systemOn      = false;
static String   lastSong      = "";
static int      wifiFailCount = 0;
static unsigned long streamConnectMs = 0;   // wall time of last connect

// ---- MQTT (Home Assistant) -----------------------------------------
static WiFiClient   _mqttWifi;
static PubSubClient _mqtt(_mqttWifi);
static unsigned long _mqttLastReconnect = 0;
static unsigned long _mqttLastPublish   = 0;

// ---------- Forward decls --------------------------------------------
static void connectWifi();
static void initClock();
static void applyStationVolume();
static void powerOn();
static void powerOff();
static void saveVolume();
static void saveStation();
static void loadState();
static void selectStation(uint8_t index);
static void switchStation(int delta);
static void changeVolume(int delta);
static void refreshUI();
static void printResetReason();
static void printBootBanner();
static bool wifiCredentialsLookDefault();
static void startSetupAp();
static void healthSnapshot();
static const char* resetReasonStr(esp_reset_reason_t r);
static void mqttReconnect();
static void mqttPublishState();
static void mqttPublishDiscovery();

// =====================================================================
//  ESP32-audioI2S weak callbacks  (everything routed to Serial in DIAG)
// =====================================================================
void audio_showstreamtitle(const char *info) {
  if (!info) return;
  String s(info);
  if (s.startsWith("HTTP") || s.length() == 0) lastSong = "";
  else                                          lastSong = s;
  LOG("ICY", "streamtitle: %s", lastSong.c_str());
}

void audio_showstation(const char *info) {
  if (info) LOG("ICY", "station    : %s", info);
}

void audio_bitrate(const char *info) {
  if (info) LOG("ICY", "bitrate    : %s", info);
}

void audio_icyurl(const char *info) {
  if (info) DIAG("[ICY] url        : %s", info);
}

void audio_lasthost(const char *info) {
  if (info) DIAG("[ICY] lasthost   : %s", info);
}

void audio_id3data(const char *info) {
  if (info) DIAG("[ID3] %s", info);
}

void audio_eof_stream(const char *info) {
  DIAG("[STREAM] EOF: %s", info ? info : "(null)");
  lastSong = "";
}

void audio_info(const char *info) {
  if (!info || !g_diagMode) return;
  // Filter out the most verbose messages even when diag is on.
  if (strncmp(info, "slow stream", 11) == 0 ||
      strncmp(info, "ringbuffer", 10) == 0) return;
  LOG("AUDIO", "%s", info);
}

// =====================================================================
//  Boot diagnostics
// =====================================================================
static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON  : return "POWERON";
    case ESP_RST_EXT      : return "EXT(EN pin)";
    case ESP_RST_SW       : return "SW(ESP.restart)";
    case ESP_RST_PANIC    : return "PANIC";
    case ESP_RST_INT_WDT  : return "INT_WDT";
    case ESP_RST_TASK_WDT : return "TASK_WDT";
    case ESP_RST_WDT      : return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT : return "BROWNOUT";
    case ESP_RST_SDIO     : return "SDIO";
    default               : return "UNKNOWN";
  }
}

static void printResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  LOG("BOOT", "reset reason : %d (%s)", (int)r, resetReasonStr(r));
}

static void printBootBanner() {
  Serial.println();
  Serial.println("#########################################################");
  if (g_diagMode) {
    Serial.println("#  ESP32-S3 N16R8 Web Radio    [DIAG MODE - VERBOSE]    #");
  } else {
    Serial.println("#  ESP32-S3 N16R8 Web Radio                             #");
  }
  Serial.println("#########################################################");

  LOG("BOOT", "chip         : %s rev %d, %d core(s) @ %u MHz",
      ESP.getChipModel(), ESP.getChipRevision(),
      ESP.getChipCores(), ESP.getCpuFreqMHz());
  LOG("BOOT", "flash        : %u MB, mode=%u (0=QIO 1=QOUT 2=DIO 3=DOUT)",
      ESP.getFlashChipSize() / (1024U * 1024U),
      ESP.getFlashChipMode());
  LOG("BOOT", "heap (SRAM)  : free=%u  total=%u  min-ever=%u",
      ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap());
  LOG("BOOT", "psram        : free=%u  total=%u",
      ESP.getFreePsram(), ESP.getPsramSize());
  printResetReason();
  LOG("BOOT", "SDK          : %s", ESP.getSdkVersion());
  LOG("BOOT", "DIAG_MODE    : %d (initial; runtime value may change after NVS load)",
      DIAG_MODE);
  LOG("BOOT", "station count: %u (will be updated after config load)",
      (unsigned)DEFAULT_STATION_COUNT);
}

// =====================================================================
//  WiFi
// =====================================================================
static void connectWifi() {
  LOG("WIFI", "connecting to '%s' ...", config.wifiSSID.c_str());
  display.showBoot("WiFi connecting...");

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(config.wifiSSID.c_str(), config.wifiPass.c_str());

  unsigned long start = millis();
  unsigned long lastDot = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 30000) {
    if (millis() - lastDot > 500) {
      Serial.print('.');
      lastDot = millis();
    }
    delay(50);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiFailCount = 0;
    LOG("WIFI", "CONNECTED in %lu ms", millis() - start);
    LOG("WIFI", "  SSID    : %s", WiFi.SSID().c_str());
    LOG("WIFI", "  IP      : %s", WiFi.localIP().toString().c_str());
    LOG("WIFI", "  Gateway : %s", WiFi.gatewayIP().toString().c_str());
    LOG("WIFI", "  DNS     : %s", WiFi.dnsIP().toString().c_str());
    LOG("WIFI", "  Subnet  : %s", WiFi.subnetMask().toString().c_str());
    LOG("WIFI", "  RSSI    : %d dBm", WiFi.RSSI());
    LOG("WIFI", "  Channel : %d", WiFi.channel());
    LOG("WIFI", "  MAC     : %s", WiFi.macAddress().c_str());
  } else {
    LOG("WIFI", "FAILED after %lu ms (status=%d)",
        millis() - start, (int)WiFi.status());
  }
}

// =====================================================================
//  NTP
// =====================================================================
static void initClock() {
  LOG("NTP", "syncing (TZ=%s) ...", config.tzInfo.c_str());
  configTzTime(config.tzInfo.c_str(),
               "pool.ntp.org", "time.google.com", "time.nist.gov");

  // In production mode the hourly chime needs correct local time from
  // the first loop iteration. We wait up to 8 s for NTP in all modes.
  // 8 s is generous; on a home LAN with pool.ntp.org it usually takes
  // < 2 s. The TWDT timeout is 15 s so we have headroom.
  struct tm t;
  unsigned long s = millis();
  const int maxWaitMs = 8000;
  for (int i = 0; i < maxWaitMs / 200; ++i) {
    if (getLocalTime(&t, 0) && t.tm_year >= (2024 - 1900)) break;
    delay(200);
  }
  if (getLocalTime(&t, 0) && t.tm_year >= (2024 - 1900)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    LOG("NTP", "synced in %lu ms : %s", millis() - s, buf);
  } else {
    LOG("NTP", "not synced after %d ms -- dimmer/chime will activate once sync completes",
        maxWaitMs);
  }
}

// =====================================================================
//  Setup-mode (SoftAP + captive portal)
// =====================================================================
//
// Triggered when (a) the user never edited credentials.h before
// flashing, or (b) STA connect fails. Brings up an open SoftAP whose
// name encodes the chip's MAC (so two boards in the same room don't
// clash), hijacks DNS so every name resolves to us, and launches the
// web UI in captive mode. The phone/laptop joining the AP gets the
// setup page popped automatically.
//
// No audio, no NTP, no stream is started in this mode; only loop()'s
// captive-portal section runs until the user saves new credentials,
// which triggers ESP.restart() inside WebUI.

static bool wifiCredentialsLookDefault() {
  // Heuristics:
  //   - SSID empty            -> definitely not configured
  //   - NOT loaded from NVS and password still equals the factory
  //     placeholder in credentials.h  -> user flashed without editing
  // If NVS overlaid the config (user has saved through the web UI at
  // least once), we always trust those values.
  if (config.wifiSSID.length() == 0) return true;
  // The factory placeholder in credentials.h. Update this string if
  // you ever change the default password literal there.
  if (config.wifiPass == "YourPassword") return true;
  return false;
}

static void startSetupAp() {
  // Build a unique-ish SSID from the chip's MAC (last 2 octets).
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "ClaudeRadio-Setup-%02X%02X", mac[4], mac[5]);

  LOG("AP", "starting SoftAP (open, no password)");
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(ssid);     // open network
  delay(200);

  IPAddress apIp = WiFi.softAPIP();
  LOG("AP", "  SSID    : %s", ssid);
  LOG("AP", "  IP      : %s", apIp.toString().c_str());

  // DNS hijack: any name -> our IP (captive portal anchor).
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIp);
  LOG("AP", "  DNS hijack active on UDP/53");

  // Web UI in captive mode (extra redirect endpoints + onNotFound).
  webUI.begin(&config, true);
  LOG("AP", "  setup URL: http://%s/", apIp.toString().c_str());

  // On-screen guidance.
  display.showSetupMode(ssid, apIp.toString().c_str());

  digitalWrite(LED_STATUS, HIGH);
}

// =====================================================================
//  Select station directly by index
// =====================================================================
static void selectStation(uint8_t index) {
  if (index >= stationManager.getCount()) return;
  stationManager.setStation(index);
  lastSong = "";
  RadioStation cur = stationManager.getCurrentStation();
  applyStationVolume();
  bool ok = player.play(cur.url.c_str());
  streamConnectMs = millis();
  LOG("STREAM", "selectStation[%u] %s -> %s",
      index, cur.name.c_str(), ok ? "OK" : "FAIL");
  saveStation();
  display.invalidateAll();
  refreshUI();
  mqttPublishState();
}

// =====================================================================
//  MQTT / Home Assistant integration
// =====================================================================
//  Publishes 4 MQTT-discovery entities so HA auto-creates them:
//    switch  – power on/off
//    select  – station selection (full list)
//    number  – volume (0..maxVolume)
//    sensor  – current station name
//
//  State topic   : <deviceId>/state     (JSON, retained, every 5 s)
//  Command topics: <deviceId>/cmd/power  -> "ON"/"OFF"
//                  <deviceId>/cmd/volume -> "7"  (0..maxVolume)
//                  <deviceId>/cmd/station-> station name or "next"/"prev"
// =====================================================================

static String _mt(const char* sub) {   // topic helper
  return config.mqttDeviceId + "/" + sub;
}

static void mqttPublishState() {
  if (!_mqtt.connected()) return;
  String station = systemOn ? stationManager.getCurrentStation().name : "";
  station.replace("\"", "'");

  // volume_pct: 0.0~1.0 float — required by HA media_player entity
  float volPct = (config.maxVolume > 0)
    ? ((float)currentVolume / (float)config.maxVolume)
    : 0.0f;

  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"power\":\"%s\",\"state\":\"%s\",\"station\":\"%s\","
    "\"volume\":%u,\"volume_pct\":%.3f,\"max_volume\":%u}",
    systemOn ? "ON"      : "OFF",
    systemOn ? "playing" : "off",   // HA media_player state values
    station.c_str(),
    (unsigned)currentVolume,
    volPct,
    (unsigned)config.maxVolume);

  _mqtt.publish(_mt("state").c_str(), buf, true);
  _mqttLastPublish = millis();
}

static void mqttPublishDiscovery() {
  const String& pfx  = config.mqttDeviceId;
  const String  avail = pfx + "/availability";
  const String  stat  = pfx + "/state";

  // Shared device block
  String dev = ",\"device\":{\"identifiers\":[\"" + pfx + "\"]"
               + ",\"name\":\"Silver Line 1호\""
               + ",\"model\":\"ESP32 Internet Radio\""
               + ",\"manufacturer\":\"DIY ESP32-S3\"}";

  // Helper: publish discovery payload via beginPublish (bypasses
  // PubSubClient internal buffer limit for large payloads)
  auto pub = [&](const String& topic, const String& payload) {
    _mqtt.beginPublish(topic.c_str(), payload.length(), true);
    _mqtt.print(payload);
    _mqtt.endPublish();
    delay(30);
  };

  // 1. switch – power
  pub("homeassistant/switch/" + pfx + "_power/config",
      "{\"name\":\"Power\",\"unique_id\":\"" + pfx + "_power\""
      + ",\"state_topic\":\"" + stat + "\""
      + ",\"value_template\":\"{{ value_json.power | default('OFF') }}\""
      + ",\"command_topic\":\"" + pfx + "/cmd/power\""
      + ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\""
      + ",\"availability_topic\":\"" + avail + "\"" + dev + "}");

  // 2. number – volume
  pub("homeassistant/number/" + pfx + "_volume/config",
      "{\"name\":\"Volume\",\"unique_id\":\"" + pfx + "_volume\""
      + ",\"state_topic\":\"" + stat + "\""
      + ",\"value_template\":\"{{ value_json.volume | default(0) }}\""
      + ",\"command_topic\":\"" + pfx + "/cmd/volume\""
      + ",\"min\":0,\"max\":" + String(config.maxVolume) + ",\"step\":1"
      + ",\"availability_topic\":\"" + avail + "\"" + dev + "}");

  // 3. sensor – station name
  pub("homeassistant/sensor/" + pfx + "_station/config",
      "{\"name\":\"Station\",\"unique_id\":\"" + pfx + "_station\""
      + ",\"state_topic\":\"" + stat + "\""
      + ",\"value_template\":\"{{ value_json.station | default('') }}\""
      + ",\"availability_topic\":\"" + avail + "\"" + dev + "}");

  // Build station options array — shared by select and media_player
  String opts = "[";
  for (uint8_t i = 0; i < stationManager.getCount(); i++) {
    if (i) opts += ",";
    String n = stationManager.getStation(i).name;
    n.replace("\"", "'");
    opts += "\"" + n + "\"";
  }
  opts += "]";

  // 4. select – source list
  pub("homeassistant/select/" + pfx + "_source/config",
      "{\"name\":\"Source\",\"unique_id\":\"" + pfx + "_source\""
      + ",\"state_topic\":\"" + stat + "\""
      + ",\"value_template\":\"{{ value_json.station | default('') }}\""
      + ",\"command_topic\":\"" + pfx + "/cmd/station\""
      + ",\"options\":" + opts
      + ",\"availability_topic\":\"" + avail + "\"" + dev + "}");

  LOG("MQTT", "discovery published for device '%s' (%u stations)",
      pfx.c_str(), (unsigned)stationManager.getCount());
}

static void mqttCallback(char* topic, uint8_t* payload, unsigned int len) {
  if (len == 0 || len > 128) return;
  String topicStr = topic;
  String val;
  for (unsigned int i = 0; i < len; i++) val += (char)payload[i];

  LOG("MQTT", "cmd %s = '%s'", topicStr.c_str(), val.c_str());

  const String& pfx = config.mqttDeviceId;

  if (topicStr == pfx + "/cmd/power") {
    if (val == "ON"  && !systemOn) powerOn();
    if (val == "OFF" &&  systemOn) powerOff();

  // ── media_player: play / stop / next_track / previous_track ────────
  } else if (topicStr == pfx + "/cmd/control") {
    if (val == "play" && !systemOn) {
      powerOn();
    } else if (val == "stop" && systemOn) {
      powerOff();
    } else if (val == "next_track") {
      if (!systemOn) powerOn();
      switchStation(+1);
      return;   // switchStation already calls mqttPublishState
    } else if (val == "previous_track") {
      if (!systemOn) powerOn();
      switchStation(-1);
      return;   // switchStation already calls mqttPublishState
    }

  // ── media_player: volume as 0.0~1.0 float ──────────────────────────
  } else if (topicStr == pfx + "/cmd/volume_pct") {
    float pct = constrain(val.toFloat(), 0.0f, 1.0f);
    int v = (int)roundf(pct * (float)config.maxVolume);
    currentVolume = (uint8_t)constrain(v, 0, (int)config.maxVolume);
    applyStationVolume();
    saveVolume();
    refreshUI();

  // ── number entity: volume as integer ───────────────────────────────
  } else if (topicStr == pfx + "/cmd/volume") {
    int v = constrain(val.toInt(), 0, (int)config.maxVolume);
    currentVolume = (uint8_t)v;
    applyStationVolume();
    saveVolume();
    refreshUI();

  } else if (topicStr == pfx + "/cmd/station") {
    if (!systemOn) powerOn();
    if (val == "next") {
      switchStation(+1);
      return;
    } else if (val == "prev") {
      switchStation(-1);
      return;
    } else {
      for (uint8_t i = 0; i < stationManager.getCount(); i++) {
        if (stationManager.getStation(i).name == val) {
          selectStation(i);
          return;
        }
      }
    }
  }

  mqttPublishState();
}

static void mqttReconnect() {
  if (config.mqttBroker.length() == 0) return;

  // One-time initialisation (idempotent in PubSubClient)
  static bool inited = false;
  if (!inited) {
    _mqtt.setServer(config.mqttBroker.c_str(), config.mqttPort);
    _mqtt.setCallback(mqttCallback);
    _mqtt.setBufferSize(1024);   // for incoming cmd payloads
    inited = true;
  }

  String clientId = "SilverLine_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  String lwt      = _mt("availability");

  bool ok = (config.mqttUser.length() > 0)
    ? _mqtt.connect(clientId.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str(),
                    lwt.c_str(), 1, true, "offline")
    : _mqtt.connect(clientId.c_str(), nullptr, nullptr,
                    lwt.c_str(), 1, true, "offline");

  if (!ok) {
    LOG("MQTT", "connect FAILED (state=%d) broker=%s:%u",
        _mqtt.state(), config.mqttBroker.c_str(), config.mqttPort);
    return;
  }

  _mqtt.publish(lwt.c_str(), "online", true);
  _mqtt.subscribe((_mt("cmd/+")).c_str());
  mqttPublishDiscovery();
  mqttPublishState();
  LOG("MQTT", "connected -> %s:%u  id=%s",
      config.mqttBroker.c_str(), config.mqttPort, clientId.c_str());
}

// =====================================================================
//  Power (soft)
// =====================================================================
static void powerOff() {
  LOG("PWR", "soft OFF (button) -- stopping stream and entering clock mode");

  // Stop network audio FIRST so no more bytes come in. audio.stopSong()
  // closes the TCP connection inside ESP32-audioI2S and clears the
  // input buffer. We then deliberately do NOT call player.loop() in
  // the main loop while systemOn==false, so the library will not
  // re-open or push more data anywhere.
  player.stop();

  // Clear what we last knew so the radio UI doesn't briefly flash old
  // metadata when we power back on.
  lastSong = "";
  streamConnectMs = 0;

  systemOn = false;

  // The clock will be drawn on the next loop() iteration; show a
  // brief placeholder now in case NTP isn't available yet.
  display.showOff();
  mqttPublishState();

  LOG("PWR", "stream stopped, audio.loop() will no longer be called");
}

static void powerOn() {
  LOG("PWR", "soft ON (button) -- restarting stream");
  systemOn = true;
  display.invalidateAll();                    // also clears _clockMode
  RadioStation cur = stationManager.getCurrentStation();
  applyStationVolume();   // applies direct-stream boost if needed
  player.play(cur.url.c_str());
  streamConnectMs = millis();
  refreshUI();
  mqttPublishState();
}

// =====================================================================
//  State persistence
// =====================================================================
static void saveVolume() {
  prefs.begin("radio", false);
  prefs.putUChar("vol", currentVolume);
  prefs.end();
}
static void saveStation() {
  prefs.begin("radio", false);
  prefs.putUChar("idx", stationManager.getCurrentIndex());
  prefs.end();
}
static void loadState() {
  prefs.begin("radio", true);
  currentVolume = prefs.getUChar("vol", 12);
  uint8_t idx = prefs.getUChar("idx", 0);
  prefs.end();
  if (currentVolume > 21) currentVolume = 12;
  // Honor the maxVolume cap from web UI -- a saved value of 18 must
  // drop to 11 if the user later tightened the cap.
  if (currentVolume > config.maxVolume) currentVolume = config.maxVolume;
  stationManager.setStation(idx);
  LOG("NVS", "loaded vol=%u (max=%u)  station_idx=%u",
      currentVolume, config.maxVolume, idx);
}

// =====================================================================
//  Actions
// =====================================================================

// Apply the correct volume for the currently-playing station.
// Proxy streams (HLS via radio-proxy) already have +GAIN_DB applied
// by ffmpeg. Direct streams (SomaFM etc.) skip the proxy and are
// quieter. directStreamBoost compensates by adding to the base volume.
static void applyStationVolume() {
  RadioStation cur = stationManager.getCurrentStation();
  bool isProxy = config.radioProxy.length() > 0 &&
                 cur.url.startsWith(config.radioProxy);
  uint8_t vol = currentVolume;
  if (!isProxy && config.directStreamBoost > 0) {
    int boosted = (int)currentVolume + (int)config.directStreamBoost;
    vol = (uint8_t)constrain(boosted, 0, 21);
  }
  player.setVolume(vol);
  LOG("VOL", "base=%u  %s(+%u)  applied=%u",
      currentVolume,
      isProxy ? "proxy" : "direct",
      isProxy ? 0 : (unsigned)config.directStreamBoost,
      (unsigned)vol);
}

static void changeVolume(int delta) {
  int v = (int)currentVolume + delta;
  v = constrain(v, 0, (int)config.maxVolume);
  if ((uint8_t)v == currentVolume) return;
  currentVolume = (uint8_t)v;
  applyStationVolume();
  saveVolume();
  refreshUI();
  mqttPublishState();
  LOG("ACT", "volume = %u  (max=%u)", currentVolume, config.maxVolume);
}

static void switchStation(int delta) {
  lastSong = "";
  if (delta > 0) stationManager.nextStation();
  else           stationManager.previousStation();
  RadioStation cur = stationManager.getCurrentStation();
  LOG("ACT", "switch -> [%u/%u] %s",
      stationManager.getCurrentIndex() + 1,
      stationManager.getCount(), cur.name.c_str());

  unsigned long t0 = millis();
  bool ok = player.play(cur.url.c_str());
  streamConnectMs = millis();
  LOG("STREAM", "connect %s in %lu ms",
      ok ? "OK" : "FAILED", millis() - t0);

  applyStationVolume();   // apply boost for direct/proxy after station change
  saveStation();
  display.invalidateAll();
  refreshUI();
  mqttPublishState();
}

static void refreshUI() {
  RadioStation cur = stationManager.getCurrentStation();
  const char* song = lastSong.length() ? lastSong.c_str() : "";
  display.showRadioUI("Internet Radio",
                      cur.name.c_str(),
                      song,
                      display.getCurrentTime().c_str(),
                      WiFi.RSSI(),
                      currentVolume, config.maxVolume,
                      stationManager.getCurrentIndex(), stationManager.getCount());
}

// =====================================================================
//  Health snapshot
// =====================================================================
static void healthSnapshot() {
  Audio& a = player.getAudio();

  if (!systemOn) {
    // Stopped / clock mode: NOT streaming. The audioI2S input buffer
    // may still hold some leftover bytes from before player.stop()
    // but no new data is being received (player.loop() is not called
    // while systemOn==false). We print a different format here so
    // the bytes-in-buffer counter doesn't look like live streaming.
    struct tm t;
    char clockStr[16] = "(no NTP)";
    if (getLocalTime(&t, 0) && t.tm_year >= (2024 - 1900)) {
      strftime(clockStr, sizeof(clockStr), "%H:%M:%S", &t);
    }
    DIAG("STOPPED  uptime=%lus  heap=%u  psram=%u  wifi=%s rssi=%d  "
         "clock=%s  (no network audio in progress)",
         millis() / 1000,
         ESP.getFreeHeap(), ESP.getFreePsram(),
         (WiFi.status() == WL_CONNECTED ? "OK" : "DOWN"),
         WiFi.RSSI(),
         clockStr);

    webUI.setStatusText("stopped (clock mode)");
    return;
  }

  // ---- Active radio playback ----
  RadioStation cur = stationManager.getCurrentStation();

  // PSRAM input ringbuffer usage. A healthy stream keeps this well
  // above 0%; if it repeatedly drains to 0% the network can't keep up.
  uint32_t bufSz   = player.bufferSize();
  uint32_t bufFill = player.bufferFilled();
  uint32_t bufPct  = bufSz ? (uint32_t)((uint64_t)bufFill * 100 / bufSz) : 0;

  DIAG("uptime=%lus  heap=%u  psram=%u  wifi=%s rssi=%d  audio.running=%d  "
       "buf=%u/%uKB(%u%%)  vol=%u  station=[%u/%u] %s  song=\"%s\"",
       millis() / 1000,
       ESP.getFreeHeap(), ESP.getFreePsram(),
       (WiFi.status() == WL_CONNECTED ? "OK" : "DOWN"),
       WiFi.RSSI(),
       (int)a.isRunning(),
       bufFill / 1024, bufSz / 1024, bufPct,
       (unsigned)currentVolume,
       (unsigned)(stationManager.getCurrentIndex() + 1),
       (unsigned)stationManager.getCount(),
       cur.name.c_str(),
       lastSong.c_str());

  // Push a one-line status to the web UI's /api/status endpoint.
  String s;
  s.reserve(160);
  s += "[";
  s += String(stationManager.getCurrentIndex() + 1);
  s += "/";
  s += String(stationManager.getCount());
  s += "] ";
  s += cur.name;
  s += "  -  vol=";
  s += String(currentVolume);
  s += a.isRunning() ? "  -  playing" : "  -  idle";
  if (lastSong.length() > 0) {
    s += "  -  ";
    s += lastSong;
  }
  webUI.setStatusText(s);
}

// =====================================================================
//  setup / loop
// =====================================================================
void setup() {
  // ---- 1. Serial -----------------------------------------------------
  Serial.begin(115200);
  // ESP32-S3 USB-CDC needs the host to enumerate before any output
  // becomes visible. 2 s is generous but harmless.
  delay(2000);

  printBootBanner();

  // ---- 2. WiFi power save off ---------------------------------------
  LOG("INIT", "esp_wifi_set_ps(WIFI_PS_NONE)");
  esp_wifi_set_ps(WIFI_PS_NONE);

  // ---- 3. Status LED -------------------------------------------------
  LOG("INIT", "status LED on GPIO %d", LED_STATUS);
  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LOW);

  // ---- 4. Display ----------------------------------------------------
  LOG("INIT", "display begin (CS=%d MOSI=%d SCK=%d DC=%d RST=%d, no BL)...",
      TFT_CS, TFT_MOSI, TFT_SCLK, TFT_DC, TFT_RST);
  display.begin();
  display.showBoot("Booting...");
  LOG("INIT", "display OK");

  // ---- 5. Buttons ----------------------------------------------------
  LOG("INIT", "buttons begin (LEFT=GPIO %d, RIGHT=GPIO %d, INPUT_PULLUP)",
      BTN_LEFT, BTN_RIGHT);
  buttons.begin();
  LOG("INIT", "buttons read at boot: LEFT=%s RIGHT=%s",
      digitalRead(BTN_LEFT)  == HIGH ? "HIGH(idle)" : "LOW(pressed)",
      digitalRead(BTN_RIGHT) == HIGH ? "HIGH(idle)" : "LOW(pressed)");

  // ---- 6. Runtime config (NVS overlay over compile-time defaults) ---
  //   First boot   : NVS empty -> defaults from credentials.h apply.
  //   Later boots  : whatever the web UI saved wins.
  config.loadDefaults();
  bool fromNvs = config.loadFromNvs();
  LOG("CONFIG", "%s + %u stations",
      fromNvs ? "loaded from NVS" : "first boot (defaults only)",
      (unsigned)config.stationCount);

  // From this point on use the user's saved diag preference instead of
  // the compile-time default. Affects DIAG() calls and health period.
  g_diagMode = config.diagMode;
  if (g_diagMode) {
    config.printAll();
  }

  // ---- 6b. NVS / restored runtime state -----------------------------
  //   Must run AFTER config load so stationManager has its stations
  //   array; otherwise setStation(idx) is a no-op.
  loadState();

  // ---- 7. WiFi STA (with AP fallback) ------------------------------
  // Override path: if BOTH buttons are held while booting, skip STA
  // entirely and go straight to setup-AP mode. This is the manual
  // escape hatch when WiFi credentials are wrong or the user wants
  // to reconfigure without flashing.
  //
  // Also: a transient NVS flag "force_ap" is set when the user holds
  // both buttons for 10 s during normal operation -- that triggers
  // a reboot into AP mode. We consume the flag here so the next
  // boot is back to normal STA.
  bool forceAp = (digitalRead(BTN_LEFT) == LOW) && (digitalRead(BTN_RIGHT) == LOW);
  {
    Preferences fp;
    if (fp.begin("radio", false)) {
      if (fp.getBool("force_ap", false)) {
        forceAp = true;
        fp.remove("force_ap");
        LOG("WIFI", "force_ap flag from previous boot -- forcing setup AP");
      }
      fp.end();
    }
  }
  if (forceAp) {
    LOG("WIFI", "forcing setup AP mode (button hold or NVS flag)");
  } else if (wifiCredentialsLookDefault()) {
    LOG("WIFI", "credentials are placeholders -> jumping to setup AP");
  } else {
    connectWifi();
  }

  if (!forceAp && WiFi.status() == WL_CONNECTED) {
    // ============== STA MODE ==============================
    runtimeMode = MODE_STA;
    digitalWrite(LED_STATUS, HIGH);

    // Wait for lwIP to fully initialize before opening sockets.
    // Without this, socket() calls immediately after WiFi connect
    // sometimes return fd=0 (invalid), causing:
    //   [E][WiFiClient.cpp:320] setSocketOption(): fail on 0, errno: 9
    // A 1-second pause is enough for the TCP/IP stack to stabilize.
    LOG("WIFI", "waiting 1s for lwIP stack to stabilize...");
    delay(1000);

    initClock();

    // ---- 7b. Web UI (normal STA mode, no captive) -------------------
    webUI.begin(&config, false);
    LOG("WEB", "setup page: http://%s/",
        WiFi.localIP().toString().c_str());

    // ---- 8. Audio init -------------------------------------------------
    LOG("AUDIO", "init begin");
    display.showBoot("Tuning in...");
    player.begin();
    LOG("AUDIO", "setPinout BCLK=%d LRC=%d DOUT=%d",
        I2S_BCLK, I2S_LRC, I2S_DOUT);
    LOG("AUDIO", "PSRAM input buffer requested: 1024 KB");
    applyStationVolume();   // sets correct volume with boost for first station
    LOG("AUDIO", "init done (free heap: %u, free psram: %u)",
        ESP.getFreeHeap(), ESP.getFreePsram());

    // ---- 9. Boot in clock mode (radio not auto-started) ----------------
    // systemOn defaults to false. The radio stream stays stopped until
    // the user presses RIGHT_LONG to power on. The clock display is
    // drawn by the !systemOn branch in loop() once NTP is synced.
    LOG("BOOT", "clock mode active (radio stopped). Long-press RIGHT to start radio.");
    display.showOff();   // brief placeholder until first showClock() in loop()
  } else {
    // ============== SETUP AP MODE =========================
    // Either STA failed or we never tried (default credentials).
    // Skip audio/stream entirely and stay in captive portal until the
    // user saves new settings (WebUI then reboots us).
    runtimeMode = MODE_SETUP_AP;
    startSetupAp();
  }

  // ---- 9. Task Watchdog Timer ---------------------------------------
  // Last-resort recovery from a hard hang inside the audio library:
  // if loop() does not call esp_task_wdt_reset() within 15 s, the
  // CPU panics and we reboot. We want a generous timeout because
  // normal play() / stop() calls can be slow on a flaky network.
  esp_task_wdt_init(15, true);    // panic + reboot on timeout
  esp_task_wdt_add(NULL);         // watch the current (loop) task

  // ---- MQTT initial connect (if broker is configured) ---------------
  if (config.mqttBroker.length() > 0 && runtimeMode == MODE_STA) {
    LOG("MQTT", "connecting to broker %s:%u", config.mqttBroker.c_str(), config.mqttPort);
    mqttReconnect();
  }

  LOG("BOOT", "setup() complete -- entering main loop");
  Serial.println("---------------------------------------------------------");
}

void loop() {
  // ---- Task Watchdog Timer reset ----------------------------------
  // We have 15 s to come back here or the CPU panics and reboots. If
  // we ever sit hung inside audio.loop() / player.play() / a blocking
  // network call, this is the safety net that gets us out.
  esp_task_wdt_reset();

  // ---- Long-term stability guards ---------------------------------
  // In DIAG mode we keep the device awake for inspection (no auto-
  // restart on low heap or 24 h uptime). In production those guards
  // are active.
  if (ESP.getFreeHeap() < 25000) {
    if (g_diagMode) {
      static bool warned = false;
      if (!warned) {
        LOG("MEM", "LOW HEAP %u (would restart in production)", ESP.getFreeHeap());
        warned = true;
      }
    } else {
      Serial.printf("[MEM] low heap %u -> restart\n", ESP.getFreeHeap());
      delay(500);
      ESP.restart();
    }
  }
  if (!g_diagMode && millis() > 86400000UL) {     // 24 h scheduled restart
    Serial.println("[SYS] 24h scheduled restart");
    delay(500);
    ESP.restart();
  }

  // ---- Setup-AP mode: tiny loop -- DNS + web UI only --------------
  // Stay here until the user POSTs new settings; WebUI calls
  // ESP.restart() inside handleSave(), and on reboot we try STA again.
  if (runtimeMode == MODE_SETUP_AP) {
    dnsServer.processNextRequest();
    webUI.handle();

    // Periodic AP-mode heartbeat so we can see clients joining etc.
    // Cheap stand-in for healthSnapshot.
    static unsigned long lastApBeat = 0;
    if (millis() - lastApBeat >= healthPeriodMs()) {
      lastApBeat = millis();
      LOG("AP", "uptime=%lus  clients=%u  heap=%u  psram=%u",
          millis() / 1000,
          (unsigned)WiFi.softAPgetStationNum(),
          ESP.getFreeHeap(), ESP.getFreePsram());
    }
    delay(2);
    return;     // skip audio/buttons/health below
  }

  // ---- Chime state -------------------------------------------------
  // chimeContext: 0=idle, 1=from clock mode, 2=from radio mode.
  // While non-zero, player.loop() must be driven to advance MP3 decode.
  // When the clip finishes we either go silent (clock) or resume the
  // radio stream (radio mode).
  static int            chimeContext   = 0;
  static uint8_t        chimeTargetHour= 255;   // upcoming hour to play
  static unsigned long  chimeStartMs   = 0;
  static String         chimeResumeUrl = "";    // radio URL to resume after chime

  // ---- Audio ------------------------------------------------------
  if (systemOn || chimeContext != 0) player.loop();

  // ---- Stream watchdog: auto-reconnect on hang/EOF -----------------
  // ffmpeg can hand us a clean ICY header and then go silent (HLS
  // playlist expired, upstream server closed, etc). When that happens
  // audio.isRunning() drops to false but our main loop keeps spinning
  // -- without this watchdog the radio sits there forever and buttons
  // appear to "freeze" because the user thinks the device is hung.
  //
  // Strategy:
  //   - Track the last moment isRunning() was true (or the last fresh
  //     play() call).
  //   - If it stays false for > 8 seconds AND we're past a backoff
  //     deadline, reconnect to the same station.
  //   - First 5 reconnect attempts: 5-second backoff between tries.
  //   - After that: 60-second backoff, keep trying forever (so the
  //     station auto-recovers when the upstream comes back online).
  //   - Changing the station resets the counter.
  //   - chimeContext != 0: chime is playing or about to resume, skip
  //     watchdog entirely so it doesn't interfere.
  if (systemOn && chimeContext == 0 && WiFi.status() == WL_CONNECTED) {
    static unsigned long lastStreamAlive   = 0;
    static unsigned long nextStreamRetryMs = 0;
    static uint8_t       streamRetryCount  = 0;
    static int           lastStationIdx    = -1;

    Audio&   a       = player.getAudio();
    int      curIdx  = (int)stationManager.getCurrentIndex();
    bool     stationChanged = (curIdx != lastStationIdx);
    lastStationIdx = curIdx;

    if (stationChanged) {
      streamRetryCount  = 0;
      nextStreamRetryMs = 0;
      lastStreamAlive   = millis();
    }

    if (a.isRunning()) {
      lastStreamAlive   = millis();
      if (streamRetryCount > 0) {
        LOG("STREAM", "recovered after %u retries", streamRetryCount);
      }
      streamRetryCount  = 0;
      nextStreamRetryMs = 0;
    } else {
      // 4-second grace after each play() so connecttohost() can finish.
      unsigned long sinceConnect = millis() - streamConnectMs;
      unsigned long sinceAlive   = millis() - lastStreamAlive;

      if (sinceConnect > 4000 && sinceAlive > 8000
          && millis() >= nextStreamRetryMs) {
        RadioStation cur = stationManager.getCurrentStation();
        ++streamRetryCount;

        // Hard reboot after enough failures. The proxy may have leaked
        // zombie ffmpegs after a quick reconnect storm; a fresh boot
        // gives the proxy time to settle and us a clean audio state.
        if (streamRetryCount >= 10 && !g_diagMode) {
          LOG("STREAM", "%u reconnects failed -- rebooting", streamRetryCount);
          delay(500);
          ESP.restart();
        }

        unsigned long backoffMs = (streamRetryCount < 5) ? 5000UL : 60000UL;
        LOG("STREAM",
            "no audio for %lus -> auto-reconnect #%u (next backoff %lus) : %s",
            sinceAlive / 1000, streamRetryCount, backoffMs / 1000,
            cur.url.c_str());
        player.stop();
        delay(200);
        player.play(cur.url.c_str());
        streamConnectMs   = millis();
        lastStreamAlive   = millis();      // grace
        nextStreamRetryMs = millis() + backoffMs;
      }
    }
  }

  // ---- Hourly chime state machine (clock + radio modes) ----------
  // Trigger: HH:59:50  ->  plays chime announcing (HH+1):00
  // Mode mask: bit0 = clock mode, bit1 = radio mode
  // In radio mode: save current URL, interrupt, play chime, resume.
  //
  // chimeContext meaning (declared at top of loop):
  //   0 = idle
  //   1 = chime started from clock mode (was systemOn = false)
  //   2 = chime started from radio mode (was systemOn = true)
  // chimeTargetHour: the "next hour" we last chimed for (0..23, 255=none).

  // --- Start: should we begin a new chime? ---
  if (chimeContext == 0 && config.chimeVolume > 0 && config.chimeMode != 0 &&
      config.radioProxy.length() > 0 && WiFi.status() == WL_CONNECTED) {

    bool modeOk = (!systemOn && (config.chimeMode & 0x1)) ||
                  ( systemOn && (config.chimeMode & 0x2));

    if (modeOk) {
      struct tm ct;
      if (getLocalTime(&ct, 0) && ct.tm_year >= (2024 - 1900) &&
          ct.tm_min == 59 && ct.tm_sec >= 50) {
        uint8_t nextHour = (uint8_t)((ct.tm_hour + 1) % 24);

        if (nextHour != chimeTargetHour) {
          // DND check uses CURRENT time (not the target). If we're at
          // 07:59:50 and DND ends 08:00, we still suppress.
          uint16_t nowMin = (uint16_t)(ct.tm_hour * 60 + ct.tm_min);
          uint16_t ms     = config.chimeMuteStartMin;
          uint16_t me     = config.chimeMuteEndMin;
          bool muted = false;
          if (ms != me) {
            if (ms < me) muted = (nowMin >= ms) && (nowMin < me);
            else         muted = (nowMin >= ms) || (nowMin < me);
          }

          if (muted) {
            LOG("CHIME", "skipping for %02d:00 -- DND (%02u:%02u..%02u:%02u)",
                (int)nextHour, ms / 60, ms % 60, me / 60, me % 60);
            chimeTargetHour = nextHour;
          } else {
            String chimeUrl = config.radioProxy + "/chime";
            // Save current radio URL if we're in radio mode so we can
            // resume after the chime ends.
            if (systemOn) {
              chimeResumeUrl = stationManager.getCurrentStation().url;
              LOG("CHIME", "radio interrupt for %02d:00 -- saving '%s'",
                  (int)nextHour, chimeResumeUrl.c_str());
            } else {
              chimeResumeUrl = "";
              LOG("CHIME", "clock mode chime for %02d:00", (int)nextHour);
            }

            player.setVolume(config.chimeVolume);
            bool ok = player.play(chimeUrl.c_str());
            if (ok) {
              chimeContext   = systemOn ? 2 : 1;
              chimeStartMs   = millis();
              chimeTargetHour= nextHour;
              // Reset the stream watchdog grace so it doesn't try to
              // reconnect while the chime plays (we will resume manually).
              streamConnectMs = millis();
            } else {
              LOG("CHIME", "play() FAILED -- skipping");
              chimeTargetHour = nextHour;
            }
          }
        }
      }
    }
  }

  // --- Drive the chime + handle end -------------------------------
  if (chimeContext != 0) {
    unsigned long age = millis() - chimeStartMs;

    // Detect end of chime (after a 3 s connection grace period)
    if (age > 3000 && !player.getAudio().isRunning()) {
      LOG("CHIME", "finished after %lu ms", age);
      if (chimeContext == 2) {
        // Resume the radio that was playing before the chime.
        if (chimeResumeUrl.length() > 0) {
          applyStationVolume();   // restore radio volume + boost
          bool ok = player.play(chimeResumeUrl.c_str());
          streamConnectMs = millis();
          LOG("CHIME", "radio resume %s -> %s",
              ok ? "OK" : "FAIL", chimeResumeUrl.c_str());
        }
      } else {
        // Clock mode: just go silent.
        player.stop();
        player.setVolume(0);
      }
      chimeContext   = 0;
      chimeResumeUrl = "";
    }

    // Safety timeout: chime file is ~11 s; 60 s is plenty.
    if (age > 60000) {
      LOG("CHIME", "TIMEOUT after %lu ms -- forcing stop", age);
      if (chimeContext == 2 && chimeResumeUrl.length() > 0) {
        applyStationVolume();
        player.play(chimeResumeUrl.c_str());
        streamConnectMs = millis();
      } else {
        player.stop();
        player.setVolume(0);
      }
      chimeContext   = 0;
      chimeResumeUrl = "";
    }
  }

  // ---- Clock mode display update (1 Hz) ---------------------------
  // Only the digital-clock TFT rendering. Chime audio handling above
  // covers both modes.
  if (!systemOn) {
    static unsigned long lastTick = 0;
    if (millis() - lastTick >= 1000) {
      lastTick = millis();
      struct tm t;
      bool haveTime = getLocalTime(&t, 0) && t.tm_year >= (2024 - 1900);
      if (haveTime) {
        static const char* const DOWS_KOR[] =
          {"일","월","화","수","목","금","토"};
        char dateBuf[48];
        snprintf(dateBuf, sizeof(dateBuf), "%d년 %d월 %d일 (%s)",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 DOWS_KOR[t.tm_wday]);
        bool colonOn = (t.tm_sec & 1) == 0;
        display.showClock(t.tm_hour, t.tm_min, String(dateBuf), colonOn);
      }
    }
  }

  // ---- Web UI (cheap when no client) -------------------------------
  if (WiFi.status() == WL_CONNECTED) webUI.handle();

  // ---- Buttons ----------------------------------------------------
  ButtonControl::Event evt = buttons.update();
  switch (evt) {

    // ---- SHORT (< 1 s) --------------------------------------------
    case ButtonControl::RIGHT_SHORT:
      if (systemOn) {
        LOG("BTN", "RIGHT short -> vol +1");
        changeVolume(+1);
      } else {
        LOG("BTN", "RIGHT short (clock) -> radio ON");
        powerOn();
      }
      break;

    case ButtonControl::LEFT_SHORT:
      LOG("BTN", "LEFT short -> vol -1");
      if (systemOn) changeVolume(-1);
      break;

    // ---- MEDIUM (1 s ~ 2 s) : station change ----------------------
    case ButtonControl::RIGHT_MEDIUM:
      LOG("BTN", "RIGHT medium (1s) -> next station");
      if (systemOn) switchStation(+1);
      break;

    case ButtonControl::LEFT_MEDIUM:
      LOG("BTN", "LEFT medium (1s) -> prev station");
      if (systemOn) switchStation(-1);
      break;

    // ---- LONG (>= 2 s) : 라디오 모드에서만 시계 모드 전환 ------------
    // 시계 모드에서 LONG은 아무것도 안 함 (오동작 방지)
    // 시계 모드 → 라디오 ON은 RIGHT SHORT 사용
    case ButtonControl::RIGHT_LONG:
      if (systemOn) {
        LOG("BTN", "RIGHT long (2s) -> clock mode");
        powerOff();
      }
      break;

    case ButtonControl::LEFT_LONG:
      if (systemOn) {
        LOG("BTN", "LEFT long (2s) -> clock mode");
        powerOff();
      }
      break;

    // ---- BOTH held 10 s -- universal AP fallback ------------------
    case ButtonControl::BOTH_HOLD_LONG: {
      LOG("BTN", "BOTH hold 10s -> AP setup mode");
      Preferences fp;
      if (fp.begin("radio", false)) { fp.putBool("force_ap", true); fp.end(); }
      player.stop();
      delay(800);
      ESP.restart();
      break;
    }

    default: break;
  }

  // ---- MQTT (Home Assistant) --------------------------------------
  if (config.mqttBroker.length() > 0 && runtimeMode == MODE_STA) {
    if (_mqtt.connected()) {
      _mqtt.loop();
      if (millis() - _mqttLastPublish >= 5000) {
        mqttPublishState();
      }
    } else {
      if (millis() - _mqttLastReconnect >= 30000) {
        _mqttLastReconnect = millis();
        LOG("MQTT", "reconnecting...");
        mqttReconnect();
      }
    }
  }

  // ---- WiFi watchdog ---------------------------------------------
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      digitalWrite(LED_STATUS, LOW);
      LOG("WIFI", "status=%d (disconnected) -- reconnecting",
          (int)WiFi.status());
      connectWifi();
      if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(LED_STATUS, HIGH);
        if (systemOn) {
          RadioStation cur = stationManager.getCurrentStation();
          LOG("STREAM", "reconnecting stream after WiFi recovery");
          player.play(cur.url.c_str());
          streamConnectMs = millis();
        }
      }
      else if (!g_diagMode && ++wifiFailCount >= 3) {
        Serial.println("[WIFI] 3 failures -> restart");
        delay(500);
        ESP.restart();
      }
    }
  }

  // ---- Periodic UI refresh (time, song, RSSI) --------------------
  static unsigned long lastUI = 0;
  if (systemOn && (millis() - lastUI >= 3000)) {
    lastUI = millis();
    refreshUI();
  }

  // ---- NTP re-sync if first sync failed (no valid year) ----------
  // WiFi stack isn't always fully up by the time setup() calls
  // initClock(). If getLocalTime() still reports a pre-2024 year
  // after 2 minutes, trigger a fresh configTzTime() so the clock
  // and hourly chime can start working without a full reboot.
  static unsigned long lastNtpRetry = 0;
  static bool ntpSynced = false;
  if (!ntpSynced && runtimeMode == MODE_STA &&
      WiFi.status() == WL_CONNECTED &&
      millis() - lastNtpRetry >= 120000) {        // every 2 minutes
    lastNtpRetry = millis();
    struct tm tCheck;
    if (getLocalTime(&tCheck, 0) && tCheck.tm_year >= (2024 - 1900)) {
      ntpSynced = true;
      LOG("NTP", "confirmed synced (year=%d)", tCheck.tm_year + 1900);
    } else {
      LOG("NTP", "still not synced -- retrying configTzTime");
      configTzTime(config.tzInfo.c_str(),
                   "pool.ntp.org", "time.google.com", "time.nist.gov");
    }
  }
  // Mark as synced once we have a valid year (catches the case where
  // initClock()'s 8 s wait succeeded on first boot).
  if (!ntpSynced) {
    struct tm tq;
    if (getLocalTime(&tq, 0) && tq.tm_year >= (2024 - 1900)) {
      ntpSynced = true;
      LOG("NTP", "synced on first check (year=%d)", tq.tm_year + 1900);
    }
  }

  // ---- Periodic health snapshot ----------------------------------
  static unsigned long lastHealth = 0;
  if (millis() - lastHealth >= healthPeriodMs()) {
    lastHealth = millis();
    healthSnapshot();
  }
}
