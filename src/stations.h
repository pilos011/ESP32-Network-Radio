#pragma once
#include "pins_config.h"          // for DIAG_MODE
#include "credentials.h"          // for RADIO_PROXY
#include "RadioStationManager.h"

// =====================================================================
//  Default radio station list (fallback / first-boot values)
//
//  The runtime list lives in Config (Config.h). On first boot Config
//  populates itself from this array; the web UI then lets the user
//  edit and persist their own list to NVS. Subsequent boots load the
//  NVS list and ignore this file -- so editing here only affects
//  brand-new boards or NVS resets.
//
//  Two kinds of entries:
//   1. Direct MP3 ICY streams  -> played by ESP32-audioI2S directly.
//      Rock-solid. Example: SomaFM.
//   2. HLS stations (MBC / KBS / SBS ...) -> ESP32-audioI2S 3.0.12
//      cannot decode HLS. These MUST go through the transcoding proxy:
//      run radio-proxy/server.js on your PC, then point the URL at
//      RADIO_PROXY "/<id>" where <id> matches radio-proxy/stations.json.
//
//  RADIO_PROXY is defined in credentials.h (your PC's LAN IP).
//  C string-literal concatenation builds the final URL:
//      RADIO_PROXY "/mbc-sfm"  ->  "http://192.168.10.119:8080/mbc-sfm"
// =====================================================================

#if DIAG_MODE

inline RadioStation defaultStations[] = {
  // Proxy test: requires radio-proxy/server.js running on your PC.
  {"MBC SFM (proxy)", RADIO_PROXY "/mbcsfm"},{"Groove Salad","http://ice3.somafm.com/groovesalad-128-mp3"}

  // ESP32-only test (no PC needed): uncomment if you want to verify the
  // audio pipeline without the proxy, then comment out the line above.
  // {"TEST: Groove Salad", "http://ice3.somafm.com/groovesalad-128-mp3"},
};

#else

inline RadioStation defaultStations[] = {
  // -------- HLS stations via the transcoding proxy -------------------
  {"MBC 표준 FM", RADIO_PROXY "/mbcsfm"},
  {"MBC FM4U", RADIO_PROXY "/mbcfm"},
  {"CBS 표준 FM", RADIO_PROXY "/cbssfm"},
  {"CBS Music FM", RADIO_PROXY "/cbsfm"},
  {"SBS 파워 FM", RADIO_PROXY "/sbsfm"},
  {"SBS 러브 FM", RADIO_PROXY "/sbs2fm"},
  {"KBS 제1라디오", RADIO_PROXY "/kbs1radio"},
  {"KBS 쿨FM", RADIO_PROXY "/kbs2fm"},
  {"KBS 클래식FM", RADIO_PROXY "/kbsfm"},
  {"KBS 해피FM", RADIO_PROXY "/kbs2radio"},


  // -------- Direct MP3 ICY streams (no proxy needed) -----------------
  {"NPR News",       "https://npr-ice.streamguys1.com/live.mp3"},
};

#endif

constexpr uint8_t DEFAULT_STATION_COUNT =
  sizeof(defaultStations) / sizeof(defaultStations[0]);
