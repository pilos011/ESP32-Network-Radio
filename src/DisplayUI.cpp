#include "DisplayUI.h"
#include "pins_config.h"
#include "fonts/d2coding24.h"   // 24-bit indexed D2Coding font, ASCII + Hangul
#include "Config.h"
#include <SPI.h>
#include <time.h>

// Defined in main.cpp; lets drawStation pick a font size at runtime
// based on the user's web-UI selection.
extern Config config;

// ST77XX color helpers (already provided by the library, kept as a reminder)
//   ST77XX_BLACK, ST77XX_WHITE, ST77XX_GREEN, ST77XX_RED,
//   ST77XX_BLUE,  ST77XX_YELLOW, ST77XX_CYAN, ST77XX_MAGENTA, ST77XX_ORANGE
static constexpr uint16_t COL_BG       = ST77XX_BLACK;
static constexpr uint16_t COL_TEXT     = ST77XX_WHITE;
static constexpr uint16_t COL_DIM      = 0x7BEF;   // light grey (RGB565)
static constexpr uint16_t COL_DIM2     = 0x39C7;   // dark grey  (RGB565) - empty vol slots
static constexpr uint16_t COL_ACCENT   = ST77XX_CYAN;
static constexpr uint16_t COL_OK       = ST77XX_GREEN;
static constexpr uint16_t COL_WARN     = ST77XX_YELLOW;
static constexpr uint16_t COL_BAD      = ST77XX_RED;

DisplayUI::DisplayUI()
  : _tft(TFT_CS, TFT_DC, TFT_RST) {}

void DisplayUI::begin() {
  // Backlight is hardwired to VCC on this display module's PCB
  // (no BL pin exposed). All software backlight control has been
  // removed accordingly.

  // Remap hardware SPI to the requested pins (ESP32 supports this)
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);

  _tft.init(240, 320);              // physical resolution (portrait)
  _tft.setRotation(TFT_ROTATION);   // landscape -> 320 x 240
  // 40 MHz is the Adafruit default. If you see corrupted pixels or
  // random flicker on a breadboard, drop this to 20-26 MHz.
  _tft.setSPISpeed(40000000);
  _tft.fillScreen(COL_BG);

  // Wire U8g2 on top of Adafruit_GFX so we can draw Korean / UTF-8
  // strings through _u8g2 while still using _tft for everything else.
  _u8g2.begin(_tft);

  _firstDraw = true;
  _isOff = false;
}

// =====================================================================
//  Korean text helpers (private)
//
//  unifont_t_korean1 only holds about half of the 11172 Hangul
//  syllables; the rest live in unifont_t_korean2 (see u8g2 issue
//  #830 / wiki/fntgrpunifont). For arbitrary user-supplied strings
//  ("토요일" -> '토' is in korean2, others in korean1) we walk the
//  UTF-8 stream codepoint-by-codepoint, asking korean1 for the
//  glyph width first and falling back to korean2 if the glyph isn't
//  there. ASCII is rendered with korean1 (it inherits Latin1).
// =====================================================================

// Decode one UTF-8 codepoint at &s[pos], advancing pos. Returns
// 0xFFFD for malformed bytes (and advances by 1 byte to make
// progress).
static uint32_t decodeUtf8(const String& s, int& pos, int& byteLen) {
  int len = s.length();
  if (pos >= len) { byteLen = 0; return 0; }
  uint8_t b0 = (uint8_t)s[pos];
  if ((b0 & 0x80) == 0) {                 // 1-byte ASCII
    byteLen = 1; pos += 1;
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0 && pos + 1 < len) {     // 2-byte
    uint32_t cp = ((b0 & 0x1F) << 6) | (s[pos + 1] & 0x3F);
    byteLen = 2; pos += 2;
    return cp;
  }
  if ((b0 & 0xF0) == 0xE0 && pos + 2 < len) {     // 3-byte (covers Hangul)
    uint32_t cp = ((b0 & 0x0F) << 12)
                | ((s[pos + 1] & 0x3F) << 6)
                |  (s[pos + 2] & 0x3F);
    byteLen = 3; pos += 3;
    return cp;
  }
  if ((b0 & 0xF8) == 0xF0 && pos + 3 < len) {     // 4-byte
    uint32_t cp = ((b0 & 0x07) << 18)
                | ((s[pos + 1] & 0x3F) << 12)
                | ((s[pos + 2] & 0x3F) << 6)
                |  (s[pos + 3] & 0x3F);
    byteLen = 4; pos += 4;
    return cp;
  }
  byteLen = 1; pos += 1;
  return 0xFFFD;
}

int DisplayUI::drawKoreanUTF8(int x, int baselineY, const String& s) {
  int pos = 0, startX = x;
  // Cache current font so we don't call setFont() unnecessarily.
  // -1 = unknown, 1 = korean1, 2 = korean2
  int curFont = -1;
  while (pos < (int)s.length()) {
    int prevPos = pos, byteLen = 0;
    uint32_t cp = decodeUtf8(s, pos, byteLen);
    if (byteLen <= 0) break;

    // Extract the original UTF-8 bytes (null-terminated 5-byte buf).
    char buf[5] = {0};
    for (int i = 0; i < byteLen && i < 4; ++i) buf[i] = s[prevPos + i];

    bool isHangul = (cp >= 0xAC00 && cp <= 0xD7A3);

    // Try korean1 first. If the glyph is missing the width returns 0.
    if (curFont != 1) {
      _u8g2.setFont(u8g2_font_unifont_t_korean1);
      curFont = 1;
    }
    int w = _u8g2.getUTF8Width(buf);

    if (w == 0 && isHangul) {
      _u8g2.setFont(u8g2_font_unifont_t_korean2);
      curFont = 2;
      w = _u8g2.getUTF8Width(buf);
    }

    if (w == 0) {
      // Glyph in neither font -- draw an empty slot to keep alignment.
      x += 8;
      continue;
    }

    _u8g2.setCursor(x, baselineY);
    _u8g2.print(buf);
    x += w;
  }
  return x - startX;
}

int DisplayUI::measureKoreanUTF8(const String& s) {
  int pos = 0, total = 0;
  int curFont = -1;
  while (pos < (int)s.length()) {
    int prevPos = pos, byteLen = 0;
    uint32_t cp = decodeUtf8(s, pos, byteLen);
    if (byteLen <= 0) break;

    char buf[5] = {0};
    for (int i = 0; i < byteLen && i < 4; ++i) buf[i] = s[prevPos + i];

    bool isHangul = (cp >= 0xAC00 && cp <= 0xD7A3);

    if (curFont != 1) { _u8g2.setFont(u8g2_font_unifont_t_korean1); curFont = 1; }
    int w = _u8g2.getUTF8Width(buf);
    if (w == 0 && isHangul) {
      _u8g2.setFont(u8g2_font_unifont_t_korean2); curFont = 2;
      w = _u8g2.getUTF8Width(buf);
    }
    total += (w > 0) ? w : 8;
  }
  return total;
}

// =====================================================================
//  Large-font (D2Coding 24 px) helpers
//
//  D2Coding is bundled inline via src/fonts/d2coding24.h. It covers
//  ASCII (U+0020..U+007E, half-width) and all 11172 Hangul syllables
//  (U+AC00..U+D7A3, full-width). For codepoints outside that coverage
//  -- Japanese kana, CJK ideographs, less common symbols -- we fall
//  back to the smaller unifont fonts so we never render an empty box
//  on a real station/song name.
//
//  This uses the same per-codepoint-fallback strategy as the unifont
//  helpers above. Font switching across codepoints is fine; setFont()
//  is just a pointer assignment for U8g2_for_Adafruit_GFX.
// =====================================================================

// Which font is currently selected? Internal codes so we can cache
// setFont() calls (the actual font pointers are passed to u8g2).
//   -1 unknown, 0 D2Coding24, 1 unifont_korean1, 2 unifont_korean2
static inline void useFontSlot(U8G2_FOR_ADAFRUIT_GFX& u, int& cur, int want) {
  if (cur == want) return;
  cur = want;
  switch (want) {
    case 0: u.setFont(d2coding24);                  break;
    case 1: u.setFont(u8g2_font_unifont_t_korean1); break;
    case 2: u.setFont(u8g2_font_unifont_t_korean2); break;
  }
}

int DisplayUI::drawLargeUTF8(int x, int baselineY, const String& s) {
  int pos = 0, startX = x, curFont = -1;
  while (pos < (int)s.length()) {
    int prevPos = pos, byteLen = 0;
    uint32_t cp = decodeUtf8(s, pos, byteLen);
    if (byteLen <= 0) break;

    char buf[5] = {0};
    for (int i = 0; i < byteLen && i < 4; ++i) buf[i] = s[prevPos + i];

    // 1) Try the large D2Coding font first.
    useFontSlot(_u8g2, curFont, 0);
    int w = _u8g2.getUTF8Width(buf);

    // 2) Not in D2Coding (e.g. Japanese hiragana) -> unifont_korean1.
    if (w == 0) {
      useFontSlot(_u8g2, curFont, 1);
      w = _u8g2.getUTF8Width(buf);
    }
    // 3) Still missing -> unifont_korean2 (only worth trying for Hangul).
    if (w == 0 && cp >= 0xAC00 && cp <= 0xD7A3) {
      useFontSlot(_u8g2, curFont, 2);
      w = _u8g2.getUTF8Width(buf);
    }

    if (w == 0) {
      // Truly unknown -- skip a full-width slot for alignment.
      x += 12;
      continue;
    }

    _u8g2.setCursor(x, baselineY);
    _u8g2.print(buf);
    x += w;
  }
  return x - startX;
}

int DisplayUI::measureLargeUTF8(const String& s) {
  int pos = 0, total = 0, curFont = -1;
  while (pos < (int)s.length()) {
    int prevPos = pos, byteLen = 0;
    uint32_t cp = decodeUtf8(s, pos, byteLen);
    if (byteLen <= 0) break;

    char buf[5] = {0};
    for (int i = 0; i < byteLen && i < 4; ++i) buf[i] = s[prevPos + i];

    useFontSlot(_u8g2, curFont, 0);
    int w = _u8g2.getUTF8Width(buf);
    if (w == 0) {
      useFontSlot(_u8g2, curFont, 1);
      w = _u8g2.getUTF8Width(buf);
    }
    if (w == 0 && cp >= 0xAC00 && cp <= 0xD7A3) {
      useFontSlot(_u8g2, curFont, 2);
      w = _u8g2.getUTF8Width(buf);
    }
    total += (w > 0) ? w : 12;
  }
  return total;
}

void DisplayUI::setRotation(uint8_t r) {
  _tft.setRotation(r);
  invalidateAll();
}

void DisplayUI::invalidateAll() {
  _firstDraw   = true;
  _lastMode    = "";
  _lastStation = "";
  _lastSong    = "";
  _lastTime    = "";
  _lastWifi    = -999;
  _lastWifiBars = -1;
  _lastVol     = -1;
  _lastIdx     = -1;
  _clockMode   = false;     // leaving clock screen on next radio-UI draw
}

String DisplayUI::getCurrentTime() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return "--:--";
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

// =====================================================================
//  Boot / soft-off screens
// =====================================================================
void DisplayUI::showBoot(const char* msg) {
  _isOff = false;
  // (backlight is hardwired ON, no software control)
  _tft.fillScreen(COL_BG);
  _tft.setTextWrap(false);

  _tft.setTextColor(COL_ACCENT, COL_BG);
  _tft.setTextSize(3);
  const char* title = "ESP32-S3 Radio";
  int16_t x1, y1; uint16_t w, h;
  _tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 80);
  _tft.print(title);

  _tft.setTextColor(COL_TEXT, COL_BG);
  _tft.setTextSize(2);
  _tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 140);
  _tft.print(msg);

  invalidateAll();
}

void DisplayUI::showOff() {
  _isOff = true;
  _clockMode = false;
  _tft.fillScreen(COL_BG);
  _tft.setTextWrap(false);
  _tft.setTextColor(COL_DIM, COL_BG);
  _tft.setTextSize(2);
  const char* msg = "OFF";
  int16_t x1, y1; uint16_t w, h;
  _tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, (TFT_H - (int)h) / 2);
  _tft.print(msg);
  // Dim the backlight but keep a hint visible
  // (backlight is hardwired ON, no software control)
}

// ---------------------------------------------------------------------
//  Big digital clock screen
//
//  Layout (320x240 landscape):
//   +---------------------------------------+
//   |                                       |
//   |    PM   09:34                         |  AM/PM size 3 + time size 10
//   |                                       |
//   |                                       |
//   |     2026년 5월 16일 (토)               |  u8g2 unifont (Korean)
//   +---------------------------------------+
//
//  Time digits use Adafruit GFX built-in font scaled large (size 10
//  -> 50x70 per glyph). Korean date uses U8g2 unifont_t_korean1 via
//  _u8g2 (UTF-8). Both AM/PM and time are drawn with setTextColor(fg,
//  bg) so re-stamping every second is flicker-free; the date area is
//  blanked once per minute when it actually changes.
// ---------------------------------------------------------------------
void DisplayUI::showClock(int hour24, int minute,
                          const String& dateText, bool colonOn) {
  // BUG FIX: _isOff must NOT be set true here.
  // _isOff=true signals showRadioUI() that the screen was in "off/blank" state
  // and needs a full repaint. Clock mode is a valid display state, NOT "off".
  // Setting it true here caused showRadioUI() to do an extra fillScreen() after
  // invalidateAll() when returning from clock -> radio, and also confused the
  // legacy comment removed (no backlight control).
  // The first-draw clear is handled by `entering` / _clockMode below.
  _isOff = false;
  _tft.setTextWrap(false);

  // First entry: clear once. Subsequent calls just re-stamp glyphs.
  // We also note this transition (`entering`) so the date redraw below
  // happens even if the date string is byte-for-byte identical to the
  // last time the clock was shown -- otherwise the second time the
  // user enters clock mode the screen would be cleared but the date
  // text would NOT be redrawn (it was cached in `lastDate`).
  bool entering = !_clockMode;
  if (entering) {
    _tft.fillScreen(COL_BG);
    _clockMode = true;
  }

  // 24h -> 12h + AM/PM
  bool isPM   = (hour24 >= 12);
  int  hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;
  const char* ampm = isPM ? "PM" : "AM";

  // "HH:MM" or "HH MM" (space when colon blinks off, same width)
  char timeBuf[8];
  snprintf(timeBuf, sizeof(timeBuf), "%02d%c%02d",
           hour12, colonOn ? ':' : ' ', minute);

  // ---- Measure ----------------------------------------------------
  int16_t x1, y1;
  uint16_t wTime, hTime, wAmpm, hAmpm;
  _tft.setTextSize(8);
  _tft.getTextBounds(timeBuf, 0, 0, &x1, &y1, &wTime, &hTime);   // ~200x56
  _tft.setTextSize(3);
  _tft.getTextBounds(ampm, 0, 0, &x1, &y1, &wAmpm, &hAmpm);      //  ~30x21

  const int gap     = 14;
  const int totalW  = (int)wAmpm + gap + (int)wTime;
  const int xAmpm   = (TFT_W - totalW) / 2;
  const int xTime   = xAmpm + (int)wAmpm + gap;
  const int yTime   = 70;                                        // 70..126
  const int yAmpm   = yTime + ((int)hTime - (int)hAmpm) / 2;     // v-center

  // ---- AM/PM ------------------------------------------------------
  _tft.setTextColor(COL_DIM, COL_BG);
  _tft.setTextSize(3);
  _tft.setCursor(xAmpm, yAmpm);
  _tft.print(ampm);

  // ---- HH:MM (big) ------------------------------------------------
  _tft.setTextColor(COL_ACCENT, COL_BG);
  _tft.setTextSize(8);
  _tft.setCursor(xTime, yTime);
  _tft.print(timeBuf);

  // ---- Korean date (unifont 16x16, korean1+korean2 fallback) -----
  // Library limitation: U8g2_for_Adafruit_GFX only ships unifont_t_
  // korean1 / _korean2 (both 16x16). There is no larger Korean font
  // bundled, so we keep the date at this size for now. Color is
  // boosted to COL_TEXT (instead of COL_DIM) for better readability
  // against the small glyphs.
  const int dateBaselineY = 195;
  const int dateClearTop  = 175;
  const int dateClearH    = 26;

  static String lastDate;
  if (entering || dateText != lastDate) {
    _tft.fillRect(0, dateClearTop, TFT_W, dateClearH, COL_BG);
    lastDate = dateText;

    _u8g2.setFontMode(1);
    _u8g2.setForegroundColor(COL_TEXT);

    // Measure first so we can center.
    int wDate = measureKoreanUTF8(dateText);
    int xDate = (TFT_W - wDate) / 2;
    drawKoreanUTF8(xDate, dateBaselineY, dateText);
  }
}

// ---------------------------------------------------------------------
//  Setup-mode screen (AP / first-boot guide). Tells the user exactly
//  which WiFi to join and what URL to open. Layout (320x240 landscape):
//
//   +-------------------------------+
//   |        Setup Mode             |   accent, size 3
//   |                               |
//   |   Join this WiFi network:     |   text, size 2
//   |   ClaudeRadio-Setup-A1B2      |   accent, size 2
//   |                               |
//   |   Then open in browser:       |   text, size 2
//   |   http://192.168.4.1          |   accent, size 2
//   |                               |
//   |   (no password)               |   dim, size 1
//   +-------------------------------+
// ---------------------------------------------------------------------
void DisplayUI::showSetupMode(const char* apSsid, const char* apIp) {
  _isOff = false;
  // (backlight is hardwired ON, no software control)
  _tft.fillScreen(COL_BG);
  _tft.setTextWrap(false);

  int16_t x1, y1; uint16_t w, h;

  // Title
  _tft.setTextColor(COL_ACCENT, COL_BG);
  _tft.setTextSize(3);
  const char* title = "Setup Mode";
  _tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 20);
  _tft.print(title);

  // Line 1: prompt
  _tft.setTextColor(COL_TEXT, COL_BG);
  _tft.setTextSize(2);
  const char* l1 = "Join this WiFi:";
  _tft.getTextBounds(l1, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 75);
  _tft.print(l1);

  // SSID
  _tft.setTextColor(COL_ACCENT, COL_BG);
  _tft.setTextSize(2);
  _tft.getTextBounds(apSsid, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 100);
  _tft.print(apSsid);

  // Line 2: URL prompt
  _tft.setTextColor(COL_TEXT, COL_BG);
  _tft.setTextSize(2);
  const char* l2 = "Then open:";
  _tft.getTextBounds(l2, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 145);
  _tft.print(l2);

  // URL
  _tft.setTextColor(COL_ACCENT, COL_BG);
  _tft.setTextSize(2);
  String url = String("http://") + apIp;
  _tft.getTextBounds(url.c_str(), 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 170);
  _tft.print(url);

  // Footer hint
  _tft.setTextColor(COL_DIM, COL_BG);
  _tft.setTextSize(1);
  const char* hint = "(open network - no password)";
  _tft.getTextBounds(hint, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((TFT_W - (int)w) / 2, 210);
  _tft.print(hint);

  invalidateAll();
}

// =====================================================================
//  Main radio UI (partial redraw)
// =====================================================================
void DisplayUI::showRadioUI(const char* mode,
                            const char* station,
                            const char* song,
                            const char* time_,
                            int wifiDbm,
                            uint8_t volume,
                            uint8_t volMax,
                            uint8_t stationIdx,
                            uint8_t stationCnt) {
  if (_isOff) {
    _tft.fillScreen(COL_BG);
    // (backlight is hardwired ON, no software control)
    _isOff = false;
    invalidateAll();
  }

  if (_firstDraw) {
    _tft.fillScreen(COL_BG);
    // Static dividers
    _tft.drawFastHLine(0, H_HEADER + 2,  TFT_W, COL_DIM);
    _tft.drawFastHLine(0, Y_FOOTER - 4,  TFT_W, COL_DIM);
  }

  // Force time refresh if the underlying string changed.
  if (_firstDraw || _lastTime != time_ || _lastMode != mode ||
      wifiToBars(wifiDbm) != _lastWifiBars) {
    drawHeader(mode, time_, wifiDbm);
    _lastTime = time_;
    _lastMode = mode;
    _lastWifi = wifiDbm;
    _lastWifiBars = wifiToBars(wifiDbm);
  }

  if (_firstDraw || _lastStation != station) {
    drawStation(station);
    _lastStation = station;
  }

  if (_firstDraw || _lastSong != song) {
    drawSong(song);
    _lastSong = song;
  }

  if (_firstDraw || _lastVol != (int)volume || _lastIdx != (int)stationIdx) {
    drawFooter(volume, volMax, stationIdx, stationCnt);
    _lastVol = volume;
    _lastIdx = stationIdx;
  }

  _firstDraw = false;
}

// =====================================================================
//  Sections
// =====================================================================
void DisplayUI::drawHeader(const char* mode, const char* time_, int wifiDbm) {
  // Wipe the header
  _tft.fillRect(0, 0, TFT_W, H_HEADER, COL_BG);

  _tft.setTextWrap(false);
  _tft.setTextColor(COL_ACCENT, COL_BG);
  _tft.setTextSize(2);
  _tft.setCursor(8, 6);
  _tft.print(mode);

  // Time (top right, just left of the wifi bars)
  _tft.setTextColor(COL_TEXT, COL_BG);
  _tft.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  _tft.getTextBounds(time_, 0, 0, &x1, &y1, &w, &h);
  int timeX = TFT_W - 60 - (int)w;
  _tft.setCursor(timeX, 6);
  _tft.print(time_);

  drawWifiBars(wifiDbm, TFT_W - 48, 4);
}

void DisplayUI::drawStation(const char* station) {
  _tft.fillRect(0, Y_STATION, TFT_W, H_STATION, COL_BG);
  if (station == nullptr || station[0] == '\0') return;

  _u8g2.setFontMode(1);
  _u8g2.setForegroundColor(COL_TEXT);

  // The user picks between two sizes from the web UI. Anything other
  // than 16 falls back to the bigger 24 px font.
  const bool big = (config.stationFontSize >= 24);

  auto measure = [&](const String& s) {
    return big ? measureLargeUTF8(s) : measureKoreanUTF8(s);
  };
  auto draw = [&](int x, int y, const String& s) {
    return big ? drawLargeUTF8(x, y, s) : drawKoreanUTF8(x, y, s);
  };

  String s = station;

  // UTF-8 safe "drop last codepoint"
  auto popOneChar = [](String& in) {
    int n = in.length();
    if (n == 0) return;
    int rm = 1;
    while (n - rm > 0 && (((unsigned char)in[n - rm]) & 0xC0) == 0x80) rm++;
    in = in.substring(0, n - rm);
  };

  const int maxW = TFT_W - 16;
  const int lineH      = big ? 28 : 20;          // line spacing
  const int singleY    = big ? Y_STATION + (H_STATION + 24) / 2 - 2
                             : Y_STATION + H_STATION / 2 + 6;
  const int twoLineY1  = big ? Y_STATION + 24
                             : Y_STATION + 18;
  const int twoLineY2  = twoLineY1 + lineH;

  // ---- Single-line layout if it fits ------------------------------
  int wAll = measure(s);
  if (wAll <= maxW) {
    int x = (TFT_W - wAll) / 2;
    draw(x, singleY, s);
    return;
  }

  // ---- Two-line layout -------------------------------------------
  String line1 = s;
  while (measure(line1) > maxW && line1.length() > 0) popOneChar(line1);

  String line2;
  int sp = line1.lastIndexOf(' ');
  if (sp > (int)line1.length() / 2) {
    line2 = s.substring(sp + 1);
    line1 = line1.substring(0, sp);
  } else {
    line2 = s.substring(line1.length());
  }
  line2.trim();
  while (measure(line2) > maxW && line2.length() > 0) popOneChar(line2);

  int w1 = measure(line1);
  int w2 = measure(line2);
  draw((TFT_W - w1) / 2, twoLineY1, line1);
  draw((TFT_W - w2) / 2, twoLineY2, line2);
}

void DisplayUI::drawSong(const char* song) {
  _tft.fillRect(0, Y_SONG, TFT_W, H_SONG, COL_BG);

  // "NOW PLAYING" label still uses the small built-in GFX font.
  _tft.setTextWrap(false);
  _tft.setTextColor(COL_DIM, COL_BG);
  _tft.setTextSize(1);
  _tft.setCursor(12, Y_SONG + 4);
  _tft.print("NOW PLAYING");

  if (song == nullptr || song[0] == '\0') {
    _tft.setTextColor(COL_DIM, COL_BG);
    _tft.setTextSize(2);
    _tft.setCursor(12, Y_SONG + 30);
    _tft.print("--");
    return;
  }

  // Song titles can contain Korean. We use the same per-glyph
  // korean1/korean2 fallback helper that the clock screen uses.
  _u8g2.setFontMode(1);
  _u8g2.setForegroundColor(COL_TEXT);

  const int maxW = TFT_W - 24;        // 12 px margin each side
  String s = song;

  // UTF-8 safe "remove last codepoint" -- handles 3-byte Korean.
  auto popOneChar = [](String& in) {
    int n = in.length();
    if (n == 0) return;
    int rm = 1;
    while (n - rm > 0 && (((unsigned char)in[n - rm]) & 0xC0) == 0x80) rm++;
    in = in.substring(0, n - rm);
  };

  // Lambda: shrink `in` from the end until it fits maxW pixels wide.
  // Uses our fallback-aware measurement so width is accurate even
  // when characters live in korean2.
  auto fitWidth = [&](String& in) {
    while (measureKoreanUTF8(in) > maxW && in.length() > 0) {
      popOneChar(in);
    }
  };

  // ---- Line 1: as much of s as fits -------------------------------
  String line1 = s;
  fitWidth(line1);

  // ---- Line 2 (only if line 1 had to be clipped) -----------------
  String line2;
  if (line1.length() < s.length()) {
    int sp = line1.lastIndexOf(' ');
    if (sp > (int)line1.length() / 2) {
      line2 = s.substring(sp + 1);
      line1 = line1.substring(0, sp);
    } else {
      line2 = s.substring(line1.length());
    }
    line2.trim();
    fitWidth(line2);

    if (line1.length() + 1 + line2.length() < s.length()) {
      String withDots = line2 + "...";
      while (measureKoreanUTF8(withDots) > maxW && line2.length() > 0) {
        popOneChar(line2);
        withDots = line2 + "...";
      }
      line2 = withDots;
    }
  }

  // ---- Draw with korean1/korean2 fallback -------------------------
  const int by1 = Y_SONG + 28;
  const int by2 = Y_SONG + 52;
  drawKoreanUTF8(12, by1, line1);
  if (line2.length() > 0) {
    drawKoreanUTF8(12, by2, line2);
  }
}

void DisplayUI::drawFooter(uint8_t volume, uint8_t volMax,
                           uint8_t stationIdx, uint8_t stationCnt) {
  _tft.fillRect(0, Y_FOOTER, TFT_W, H_FOOTER, COL_BG);

  // Volume bar
  _tft.setTextWrap(false);
  _tft.setTextColor(COL_DIM, COL_BG);
  _tft.setTextSize(1);
  _tft.setCursor(8, Y_FOOTER + 4);
  _tft.print("VOL");

  // Bar geometry
  const int barX     = 36;
  const int barY     = Y_FOOTER + 6;
  const int slotGap  = 1;
  const int totalW   = 200;
  const int slotW    = (totalW - (volMax - 1) * slotGap) / volMax;
  const int barH     = 16;
  for (uint8_t i = 0; i < volMax; ++i) {
    int x = barX + i * (slotW + slotGap);
    uint16_t col;
    if (i < volume) {
      if      (i < volMax / 2)     col = COL_OK;
      else if (i < (volMax * 3) / 4) col = COL_WARN;
      else                          col = COL_BAD;
    } else {
      col = COL_DIM2;
    }
    _tft.fillRect(x, barY, slotW, barH, col);
  }

  // Numeric volume after the bar
  char num[8];
  snprintf(num, sizeof(num), "%u", (unsigned)volume);
  _tft.setTextColor(COL_TEXT, COL_BG);
  _tft.setTextSize(2);
  _tft.setCursor(barX + totalW + 6, Y_FOOTER + 6);
  _tft.print(num);

  // Station index, bottom right
  char idx[12];
  snprintf(idx, sizeof(idx), "[%u/%u]", stationIdx + 1, stationCnt);
  int16_t x1, y1; uint16_t w, h;
  _tft.getTextBounds(idx, 0, 0, &x1, &y1, &w, &h);
  _tft.setTextColor(COL_ACCENT, COL_BG);
  _tft.setCursor(TFT_W - (int)w - 8, Y_FOOTER + 22);
  _tft.print(idx);
}

// =====================================================================
//  Helpers
// =====================================================================
int DisplayUI::wifiToBars(int dbm) {
  if (dbm >= -55) return 4;
  if (dbm >= -65) return 3;
  if (dbm >= -75) return 2;
  if (dbm >= -85) return 1;
  return 0;
}

void DisplayUI::drawWifiBars(int wifiDbm, int x, int y) {
  // 4 bars increasing in height, 5x18 area
  const int bw = 4, gap = 2;
  int bars = wifiToBars(wifiDbm);
  for (int i = 0; i < 4; ++i) {
    int h = 4 + i * 4;
    int xi = x + i * (bw + gap);
    int yi = y + 18 - h;
    uint16_t col = (i < bars) ? COL_OK : COL_DIM;
    if (bars == 0) col = COL_BAD;
    // Wipe column first to handle the case where bars decreased
    _tft.fillRect(xi, y, bw, 20, COL_BG);
    _tft.fillRect(xi, yi, bw, h, col);
  }
}

void DisplayUI::drawCenteredText(const char* str, int x, int y, int w, int h,
                                 uint8_t sizePref, uint16_t color) {
  _tft.setTextWrap(false);
  _tft.setTextColor(color, COL_BG);

  // Shrink the font if the text doesn't fit.
  uint8_t sz = sizePref;
  int16_t bx, by; uint16_t bw, bh;
  for (; sz >= 1; --sz) {
    _tft.setTextSize(sz);
    _tft.getTextBounds(str, 0, 0, &bx, &by, &bw, &bh);
    if ((int)bw <= w - 8) break;
  }

  int tx = x + (w - (int)bw) / 2 - bx;
  int ty = y + (h - (int)bh) / 2 - by;
  _tft.setCursor(tx, ty);
  _tft.print(str);
}
