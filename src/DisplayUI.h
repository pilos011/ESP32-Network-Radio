#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>     // Korean font support (UTF-8)

class DisplayUI {
public:
  DisplayUI();

  void begin();
  void setRotation(uint8_t r);

  void showBoot(const char* msg);
  void showOff();                     // soft-off screen (fallback when no NTP)

  // AP / first-boot setup screen. Shown when STA fails or the user has
  // not configured WiFi yet. Tells them which SSID to join and where
  // to point their browser.
  void showSetupMode(const char* apSsid, const char* apIp);

  // Big digital clock + Korean date. Used as the "stopped" screen.
  // hour24 is 0..23; internally split into 12-hour + AM/PM.
  // dateText is rendered with u8g2 unifont (UTF-8), so passing a
  // Korean string like "2026년 5월 16일 (토)" works directly.
  // Safe to call every second (text re-stamp with bg-color erase).
  void showClock(int hour24, int minute,
                 const String& dateText, bool colonOn);

  void showRadioUI(const char* mode,
                   const char* station,
                   const char* song,
                   const char* time_,
                   int wifiDbm,
                   uint8_t volume,
                   uint8_t volMax,
                   uint8_t stationIdx,
                   uint8_t stationCnt);

  // Force the whole UI to be repainted on the next showRadioUI call.
  void invalidateAll();

  String getCurrentTime();

private:
  Adafruit_ST7789 _tft;
  U8G2_FOR_ADAFRUIT_GFX _u8g2;     // UTF-8 / Korean text engine on top of _tft

  // Draw a UTF-8 string at (x, baselineY) using unifont_t_korean1, but
  // falling back to unifont_t_korean2 per-codepoint when korean1 lacks
  // the glyph (e.g. "토" lives only in korean2). Returns total width
  // drawn so callers can center.
  // Caller should set foreground color and font mode before calling.
  int  drawKoreanUTF8(int x, int baselineY, const String& s);
  int  measureKoreanUTF8(const String& s);   // same algorithm, no draw

  // Same idea as drawKoreanUTF8, but using the bundled D2Coding 24 px
  // font for ASCII + Hangul (covers all 11172 syllables). For any
  // codepoint outside D2Coding's coverage (e.g. CJK ideographs in a
  // J-pop title), we transparently fall back to unifont_t_korean1/2
  // so the user never sees a missing-glyph box on a real station name
  // or song. Glyphs are 24 px tall; ASCII is half-width.
  int  drawLargeUTF8(int x, int baselineY, const String& s);
  int  measureLargeUTF8(const String& s);

  // Cached state for partial redraws
  String  _lastMode;
  String  _lastStation;
  String  _lastSong;
  String  _lastTime;
  int     _lastWifi      = -999;
  int8_t  _lastWifiBars  = -1;
  int     _lastVol       = -1;
  int     _lastIdx       = -1;
  bool    _firstDraw     = true;
  bool    _isOff         = false;
  bool    _clockMode     = false;     // true while showClock is the current screen

  // Layout constants
  static constexpr int H_HEADER       = 26;
  static constexpr int Y_STATION      = 36;
  static constexpr int H_STATION      = 60;
  static constexpr int Y_SONG         = H_HEADER + H_STATION + 14;   // ~100
  static constexpr int H_SONG         = 80;
  static constexpr int Y_FOOTER       = 204;
  static constexpr int H_FOOTER       = 36;

  void drawHeader(const char* mode, const char* time_, int wifiDbm);
  void drawStation(const char* station);
  void drawSong(const char* song);
  void drawFooter(uint8_t volume, uint8_t volMax,
                  uint8_t stationIdx, uint8_t stationCnt);
  void drawWifiBars(int wifiDbm, int x, int y);
  int  wifiToBars(int dbm);
  void drawCenteredText(const char* str, int x, int y, int w, int h,
                        uint8_t sizePref, uint16_t color);
};
