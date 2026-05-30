#include "Config.h"
#include "credentials.h"
#include "stations.h"
#include <Preferences.h>

static const char* NS = "radio";

// =====================================================================
//  Defaults
// =====================================================================
void Config::loadDefaults() {
  wifiSSID   = WIFI_SSID;
  wifiPass   = WIFI_PASS;
  tzInfo     = TZ_INFO;
  radioProxy = RADIO_PROXY;
  stationFontSize  = 24;
  maxVolume        = 16;   // 8Ω 5W 병렬(4Ω) 스피커 — 여유 충분, 11→16
  directStreamBoost = 8;
  chimeMode         = 1;
  chimeVolume       = 9;
  chimeMuteStartMin = 23 * 60;
  chimeMuteEndMin   = 8  * 60;
  chimeMode         = 1;
  diagMode   = (DIAG_MODE != 0);

  clearStations();
  for (uint8_t i = 0; i < DEFAULT_STATION_COUNT && stationCount < MAX_STATIONS; ++i) {
    stations[stationCount].name = defaultStations[i].name;
    stations[stationCount].url  = defaultStations[i].url;
    stationCount++;
  }
}

// =====================================================================
//  NVS overlay
// =====================================================================
bool Config::loadFromNvs() {
  Preferences p;
  if (!p.begin(NS, true)) return false;
  bool any = false;

  if (p.isKey("wifi_ssid")) { wifiSSID   = p.getString("wifi_ssid", "");  any = true; }
  if (p.isKey("wifi_pass")) { wifiPass   = p.getString("wifi_pass", "");  any = true; }
  if (p.isKey("tz"))        { tzInfo     = p.getString("tz", "");         any = true; }
  if (p.isKey("proxy"))     { radioProxy = p.getString("proxy", "");      any = true; }
  if (p.isKey("station_fs")){ stationFontSize = p.getUChar("station_fs", 24);     any = true; }
  if (p.isKey("max_vol"))   { maxVolume = p.getUChar("max_vol", 11);              any = true; }
  if (p.isKey("dir_boost")) { directStreamBoost = p.getUChar("dir_boost", 8);    any = true; }
  if (p.isKey("chime_mode")){ chimeMode = p.getUChar("chime_mode", 1);           any = true; }
  if (p.isKey("chime_vol")) { chimeVolume = p.getUChar("chime_vol", 9);          any = true; }
  if (p.isKey("chime_ms"))  { chimeMuteStartMin = p.getUShort("chime_ms", 23*60); any = true; }
  if (p.isKey("chime_me"))  { chimeMuteEndMin   = p.getUShort("chime_me", 8*60);  any = true; }
  if (p.isKey("chime_mode")){ chimeMode = p.getUChar("chime_mode", 1);            any = true; }
  if (p.isKey("diag"))      { diagMode   = p.getBool("diag", false);      any = true; }
  if (p.isKey("mqtt_br"))   { mqttBroker   = p.getString("mqtt_br", "");    any = true; }
  if (p.isKey("mqtt_port")) { mqttPort     = p.getUShort("mqtt_port", 1883); any = true; }
  if (p.isKey("mqtt_user")) { mqttUser     = p.getString("mqtt_user", "");   any = true; }
  if (p.isKey("mqtt_pass")) { mqttPass     = p.getString("mqtt_pass", "");   any = true; }
  if (p.isKey("mqtt_dev"))  { mqttDeviceId = p.getString("mqtt_dev", "silverline1"); any = true; }

  if (p.isKey("stations")) {
    String blob = p.getString("stations", "");
    if (blob.length() > 0) {
      parseStationsText(blob);
      any = true;
    }
  }

  p.end();
  return any;
}

bool Config::saveToNvs() {
  Preferences p;
  if (!p.begin(NS, false)) return false;

  p.putString("wifi_ssid", wifiSSID);
  p.putString("wifi_pass", wifiPass);
  p.putString("tz",        tzInfo);
  p.putString("proxy",     radioProxy);
  p.putUChar ("station_fs",stationFontSize);
  p.putUChar ("max_vol",   maxVolume);
  p.putUChar ("dir_boost", directStreamBoost);
  p.putUChar ("chime_mode", chimeMode);
  p.putUChar ("chime_vol", chimeVolume);
  p.putUShort("chime_ms",  chimeMuteStartMin);
  p.putUShort("chime_me",  chimeMuteEndMin);
  p.putUChar ("chime_mode",chimeMode);
  p.putBool  ("diag",      diagMode);
  p.putString("mqtt_br",   mqttBroker);
  p.putUShort("mqtt_port", mqttPort);
  p.putString("mqtt_user", mqttUser);
  p.putString("mqtt_pass", mqttPass);
  p.putString("mqtt_dev",  mqttDeviceId);
  p.putString("stations",  stationsToText());

  p.end();
  return true;
}

bool Config::clearNvs() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = p.clear();
  p.end();
  return ok;
}

// =====================================================================
//  Stations
// =====================================================================
void Config::clearStations() {
  for (uint8_t i = 0; i < MAX_STATIONS; ++i) {
    stations[i].name = "";
    stations[i].url  = "";
  }
  stationCount = 0;
}

bool Config::addStation(const String& name, const String& url) {
  if (stationCount >= MAX_STATIONS)            return false;
  if (name.length() == 0 || url.length() == 0) return false;
  stations[stationCount].name = name;
  stations[stationCount].url  = url;
  stationCount++;
  return true;
}

String Config::stationsToText() const {
  String out;
  out.reserve(stationCount * 80);
  for (uint8_t i = 0; i < stationCount; ++i) {
    out += stations[i].name;
    out += '|';
    out += stations[i].url;
    out += '\n';
  }
  return out;
}

void Config::parseStationsText(const String& text) {
  clearStations();
  int start = 0;
  const int len = (int)text.length();
  while (start < len && stationCount < MAX_STATIONS) {
    int eol = text.indexOf('\n', start);
    if (eol < 0) eol = len;
    String line = text.substring(start, eol);
    line.trim();
    start = eol + 1;

    if (line.length() == 0)   continue;
    if (line.startsWith("#")) continue;

    int sep = line.indexOf('|');
    if (sep <= 0) continue;
    String name = line.substring(0, sep);
    String url  = line.substring(sep + 1);
    name.trim();
    url.trim();
    addStation(name, url);
  }
}

// =====================================================================
//  Diagnostics
// =====================================================================
void Config::printAll() const {
  Serial.println("[CONFIG] ----- effective values -----");
  Serial.printf ("[CONFIG]   wifi_ssid    : %s\n", wifiSSID.c_str());
  Serial.printf ("[CONFIG]   wifi_pass    : %s (len=%u)\n",
                 wifiPass.length() ? "********" : "(empty)",
                 (unsigned)wifiPass.length());
  Serial.printf ("[CONFIG]   tz           : %s\n", tzInfo.c_str());
  Serial.printf ("[CONFIG]   proxy        : %s\n", radioProxy.c_str());
  Serial.printf ("[CONFIG]   station_fs   : %u px\n", (unsigned)stationFontSize);
  Serial.printf ("[CONFIG]   max_vol      : %u\n", (unsigned)maxVolume);
  Serial.printf ("[CONFIG]   dir_boost    : %u  (added to vol for non-proxy streams)\n",
                 (unsigned)directStreamBoost);
  Serial.printf ("[CONFIG]   chime_mode   : %u  (0=off, 1=clock, 2=radio, 3=both)\n",
                 (unsigned)chimeMode);
  Serial.printf ("[CONFIG]   chime_vol    : %u\n", (unsigned)chimeVolume);
  Serial.printf ("[CONFIG]   chime_mute   : %02u:%02u..%02u:%02u\n",
                 chimeMuteStartMin / 60, chimeMuteStartMin % 60,
                 chimeMuteEndMin   / 60, chimeMuteEndMin   % 60);
  {
    const char* m = "?";
    switch (chimeMode) {
      case 0: m = "off";        break;
      case 1: m = "clock only"; break;
      case 2: m = "radio only"; break;
      case 3: m = "both";       break;
    }
    Serial.printf ("[CONFIG]   chime_mode   : %u (%s)\n", (unsigned)chimeMode, m);
  }
  Serial.printf ("[CONFIG]   diag         : %s\n", diagMode ? "ON" : "off");
  if (mqttBroker.length() > 0) {
    Serial.printf("[CONFIG]   mqtt_broker  : %s:%u  dev=%s\n",
                  mqttBroker.c_str(), (unsigned)mqttPort, mqttDeviceId.c_str());
  } else {
    Serial.println("[CONFIG]   mqtt_broker  : (disabled)");
  }
  Serial.printf ("[CONFIG]   stations     : %u\n", stationCount);
  for (uint8_t i = 0; i < stationCount; ++i) {
    Serial.printf("[CONFIG]     [%u] %s | %s\n",
                  i + 1, stations[i].name.c_str(), stations[i].url.c_str());
  }
  Serial.println("[CONFIG] -----------------------------");
}
