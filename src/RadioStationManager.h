#pragma once
#include <Arduino.h>

// Plain station record. Used both by Config and by anyone who needs
// to display or play a station.
struct RadioStation {
  String name;
  String url;
};

// Forward declaration so we don't drag Config.h into this header.
class Config;

// Walks through Config::stations[] with a remembered current index.
// Wraps around at both ends.
class RadioStationManager {
public:
  explicit RadioStationManager(Config* cfg);

  RadioStation getCurrentStation() const;
  RadioStation getStation(uint8_t index) const;
  uint8_t      getCurrentIndex()   const { return currentIndex; }
  uint8_t      getCount()          const;

  void nextStation();
  void previousStation();
  void setStation(uint8_t index);

private:
  Config* _cfg;
  uint8_t currentIndex = 0;
};
