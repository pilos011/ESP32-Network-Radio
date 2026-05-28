#pragma once
#include <Arduino.h>

// =====================================================================
//  Two-button controller with debounce, two-threshold long-press,
//  and simultaneous-hold detection.
//
//  Press duration classification (per button, on release):
//    < MEDIUM_PRESS_MS  -> SHORT   (volume / no-op)
//    < LONG_PRESS_MS    -> MEDIUM  (station change)
//    >= LONG_PRESS_MS   -> LONG    (mode change / AP mode)
//
//  [Clock mode]
//    RIGHT SHORT        : (none)
//    RIGHT LONG (2 s)   : switch to radio mode
//    LEFT  SHORT        : (none)
//    LEFT  LONG (2 s)   : switch clock↔radio (same as RIGHT)
//
//  [Radio mode]
//    RIGHT SHORT        : volume +1
//    RIGHT MEDIUM (1 s) : next station
//    RIGHT LONG (2 s)   : switch to clock mode
//    LEFT  SHORT        : volume -1
//    LEFT  MEDIUM (1 s) : previous station
//    LEFT  LONG (2 s)   : switch clock↔radio (same as RIGHT)
//
//  [Any mode]
//    BOTH held 10 s     : force AP setup mode
// =====================================================================

class ButtonControl {
public:
  enum Event : uint8_t {
    NONE = 0,
    LEFT_SHORT,         // < 1 s
    LEFT_MEDIUM,        // 1 s .. 2 s
    LEFT_LONG,          // >= 2 s
    RIGHT_SHORT,
    RIGHT_MEDIUM,
    RIGHT_LONG,
    BOTH_HOLD_LONG      // both held >= 10 s
  };

  static constexpr unsigned long DEBOUNCE_MS       = 25;
  static constexpr unsigned long MEDIUM_PRESS_MS   = 1000;   // 1 s
  static constexpr unsigned long LONG_PRESS_MS     = 2000;   // 2 s
  static constexpr unsigned long BOTH_HOLD_LONG_MS = 10000;  // 10 s

  ButtonControl(uint8_t leftPin, uint8_t rightPin);
  void  begin();
  Event update();

private:
  uint8_t _pinL, _pinR;

  // Debounce
  bool          _rawL = false,    _rawR = false;
  bool          _stableL = false, _stableR = false;
  bool          _prevL = false,   _prevR = false;
  unsigned long _lastChangeL = 0, _lastChangeR = 0;

  // Per-button state
  enum BtnState : uint8_t { BS_IDLE, BS_DOWN };
  BtnState      _stateL = BS_IDLE, _stateR = BS_IDLE;
  unsigned long _downTimeL = 0,    _downTimeR = 0;

  // Suppress individual releases after a both-hold gesture
  bool _suppressL = false, _suppressR = false;

  // Both-hold tracking
  bool          _bothActive    = false;
  bool          _bothLongFired = false;
  unsigned long _bothDownTime  = 0;
};
