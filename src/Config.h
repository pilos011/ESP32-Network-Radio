#pragma once
#include <Arduino.h>
#include "RadioStationManager.h"

class Config {
public:
  static constexpr uint8_t MAX_STATIONS = 20;

  String wifiSSID;
  String wifiPass;
  String tzInfo;
  String radioProxy;

  uint8_t  stationFontSize = 24;
  uint8_t  maxVolume = 11;

  // Direct (non-proxy) stream volume boost (0=disabled, 0..15).
  // Added to currentVolume when station URL does NOT start with radioProxy.
  // Compensates for proxy's ffmpeg +GAIN_DB gain (~+6dB = 2x amplitude).
  uint8_t  directStreamBoost = 8;

  // Hourly chime (mode-configurable)
  // chimeMode bitmask:
  //   bit 0 (=1) -> play in clock mode  (systemOn = false)
  //   bit 1 (=2) -> play in radio mode  (systemOn = true)
  //   so values: 0=off, 1=clock only, 2=radio only, 3=both
  // Default: 1 (clock mode only)
  uint8_t  chimeMode         = 1;
  uint8_t  chimeVolume       = 9;        // setVolume() level 0..21
  uint16_t chimeMuteStartMin = 23 * 60;  // DND start (23:00 default)
  uint16_t chimeMuteEndMin   = 8  * 60;  // DND end   (08:00 default)

  bool diagMode = false;

  // ---- Home Assistant / MQTT integration --------------------------
  // Leave mqttBroker empty to disable MQTT entirely.
  // When set, the device publishes four MQTT-discovery entities to HA:
  //   switch (power), select (source), number (volume), sensor (station)
  // Configure in WebUI or set defaults in credentials.h.
  String   mqttBroker   = "";
  uint16_t mqttPort     = 1883;
  String   mqttUser     = "";
  String   mqttPass     = "";
  // Device ID is used as the MQTT topic prefix and HA unique_id base.
  // Example "silverline1" -> topics: silverline1/state, silverline1/cmd/power
  String   mqttDeviceId = "silverline1";

  RadioStation stations[MAX_STATIONS];
  uint8_t      stationCount = 0;

  void loadDefaults();
  bool loadFromNvs();
  bool saveToNvs();
  bool clearNvs();

  void clearStations();
  bool addStation(const String& name, const String& url);

  String stationsToText() const;
  void   parseStationsText(const String& text);
  void   printAll() const;
};
