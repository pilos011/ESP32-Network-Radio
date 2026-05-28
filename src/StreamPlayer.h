#pragma once
#include <Arduino.h>
#include "Audio.h"

class StreamPlayer {
public:
  StreamPlayer(int bclk, int lrc, int dout);

  void begin();
  bool play(const char* url);   // returns true on successful connect
  void stop();
  void loop();

  void setVolume(uint8_t volume);   // 0..21
  uint8_t getVolume() const { return _volume; }

  // Input ringbuffer diagnostics (PSRAM). Valid after the first
  // connecttohost(); returns 0 before the buffer is initialized.
  // NOTE: inBufferSize()/inBufferFilled() exist in ESP32-audioI2S 3.0.x.
  // If a future library bump removes them, only these two wrappers and
  // the buffer fields in main.cpp's healthSnapshot() need touching.
  uint32_t bufferSize();     // total bytes
  uint32_t bufferFilled();   // bytes currently queued

  Audio& getAudio() { return audio; }

private:
  Audio audio;
  int _bclk, _lrc, _dout;
  uint8_t _volume = 12;
  static constexpr uint8_t MIN_VOL = 0;
  static constexpr uint8_t MAX_VOL = 21;

  // PSRAM input buffer size. ESP32-audioI2S defaults to ~300 KB even
  // when 8 MB PSRAM is present. The N16R8 has plenty, so we give it
  // 1 MB -> ~64 s of 128 kbps audio, which absorbs weak-WiFi jitter
  // and eliminates the "slow stream / dropouts" reconnect loop.
  static constexpr int PSRAM_BUF_BYTES = 1024 * 1024;
};
