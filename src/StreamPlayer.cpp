#include "StreamPlayer.h"

StreamPlayer::StreamPlayer(int bclk, int lrc, int dout)
  : _bclk(bclk), _lrc(lrc), _dout(dout) {}

void StreamPlayer::begin() {
  // Enlarge the input ringbuffer in PSRAM BEFORE the first
  // connecttohost(). Must be called before the buffer is initialized
  // or the library rejects it. -1 keeps the default SRAM size; only
  // the PSRAM buffer is bumped (the N16R8 has 8 MB to spare).
  audio.setBufsize(-1, PSRAM_BUF_BYTES);

  audio.setPinout(_bclk, _lrc, _dout);
  audio.setVolume(_volume);
}

bool StreamPlayer::play(const char* url) {
  Serial.printf("[STREAM] connect: %s\n", url);
  bool ok = audio.connecttohost(url);
  Serial.println(ok ? "[STREAM] connected" : "[STREAM] connect FAILED");
  return ok;
}

void StreamPlayer::stop() {
  Serial.println("[STREAM] stop");
  audio.stopSong();
}

void StreamPlayer::loop() {
  audio.loop();
}

void StreamPlayer::setVolume(uint8_t volume) {
  _volume = constrain(volume, MIN_VOL, MAX_VOL);
  audio.setVolume(_volume);
  Serial.printf("[STREAM] volume = %u\n", _volume);
}

uint32_t StreamPlayer::bufferSize() {
  return audio.inBufferSize();
}

uint32_t StreamPlayer::bufferFilled() {
  return audio.inBufferFilled();
}
