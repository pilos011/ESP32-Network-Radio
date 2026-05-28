#include "RadioStationManager.h"
#include "Config.h"

RadioStationManager::RadioStationManager(Config* cfg) : _cfg(cfg) {}

uint8_t RadioStationManager::getCount() const {
  return _cfg ? _cfg->stationCount : 0;
}

RadioStation RadioStationManager::getCurrentStation() const {
  if (!_cfg || _cfg->stationCount == 0) return RadioStation{};
  uint8_t idx = currentIndex;
  if (idx >= _cfg->stationCount) idx = 0;
  return _cfg->stations[idx];
}

RadioStation RadioStationManager::getStation(uint8_t index) const {
  if (!_cfg || index >= _cfg->stationCount) return RadioStation{};
  return _cfg->stations[index];
}

void RadioStationManager::nextStation() {
  uint8_t n = getCount();
  if (n == 0) { currentIndex = 0; return; }
  currentIndex = (currentIndex + 1) % n;
}

void RadioStationManager::previousStation() {
  uint8_t n = getCount();
  if (n == 0) { currentIndex = 0; return; }
  currentIndex = (currentIndex == 0) ? (n - 1) : (currentIndex - 1);
}

void RadioStationManager::setStation(uint8_t index) {
  uint8_t n = getCount();
  if (n > 0 && index < n) currentIndex = index;
}
