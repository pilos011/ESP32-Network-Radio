#include "ButtonControl.h"

ButtonControl::ButtonControl(uint8_t leftPin, uint8_t rightPin)
  : _pinL(leftPin), _pinR(rightPin) {}

void ButtonControl::begin() {
  pinMode(_pinL, INPUT_PULLUP);
  pinMode(_pinR, INPUT_PULLUP);
  _rawL = _stableL = _prevL = (digitalRead(_pinL) == LOW);
  _rawR = _stableR = _prevR = (digitalRead(_pinR) == LOW);
  _lastChangeL = _lastChangeR = millis();
}

ButtonControl::Event ButtonControl::update() {
  const unsigned long now = millis();

  // ---- 1. Debounce --------------------------------------------------
  const bool rawL = (digitalRead(_pinL) == LOW);
  const bool rawR = (digitalRead(_pinR) == LOW);

  if (rawL != _rawL) { _rawL = rawL; _lastChangeL = now; }
  if (rawR != _rawR) { _rawR = rawR; _lastChangeR = now; }

  if ((now - _lastChangeL) >= DEBOUNCE_MS) _stableL = _rawL;
  if ((now - _lastChangeR) >= DEBOUNCE_MS) _stableR = _rawR;

  Event evt = NONE;

  // ---- 2. Both-hold -------------------------------------------------
  if (_stableL && _stableR) {
    if (!_bothActive) {
      _bothActive    = true;
      _bothDownTime  = now;
      _bothLongFired = false;
      _stateL = BS_IDLE;  _suppressL = true;
      _stateR = BS_IDLE;  _suppressR = true;
    }
    if (!_bothLongFired && (now - _bothDownTime) >= BOTH_HOLD_LONG_MS) {
      _bothLongFired = true;
      evt = BOTH_HOLD_LONG;
    }
  } else {
    _bothActive = false;
  }

  // ---- 3. Edge detection --------------------------------------------
  const bool lFell = (_stableL && !_prevL);
  const bool lRose = (!_stableL && _prevL);
  const bool rFell = (_stableR && !_prevR);
  const bool rRose = (!_stableR && _prevR);

  // ---- 4. Clear suppress on release ---------------------------------
  if (_suppressL && lRose) { _suppressL = false; _stateL = BS_IDLE; }
  if (_suppressR && rRose) { _suppressR = false; _stateR = BS_IDLE; }

  // ---- 5. Left button -----------------------------------------------
  if (evt == NONE && !_suppressL) {
    switch (_stateL) {
      case BS_IDLE:
        if (lFell) { _stateL = BS_DOWN; _downTimeL = now; }
        break;
      case BS_DOWN:
        if (lRose) {
          const unsigned long dur = now - _downTimeL;
          if (dur >= LONG_PRESS_MS)        evt = LEFT_LONG;
          else if (dur >= MEDIUM_PRESS_MS) evt = LEFT_MEDIUM;
          else                             evt = LEFT_SHORT;
          _stateL = BS_IDLE;
        }
        break;
    }
  }

  // ---- 6. Right button ----------------------------------------------
  if (evt == NONE && !_suppressR) {
    switch (_stateR) {
      case BS_IDLE:
        if (rFell) { _stateR = BS_DOWN; _downTimeR = now; }
        break;
      case BS_DOWN:
        if (rRose) {
          const unsigned long dur = now - _downTimeR;
          if (dur >= LONG_PRESS_MS)        evt = RIGHT_LONG;
          else if (dur >= MEDIUM_PRESS_MS) evt = RIGHT_MEDIUM;
          else                             evt = RIGHT_SHORT;
          _stateR = BS_IDLE;
        }
        break;
    }
  }

  _prevL = _stableL;
  _prevR = _stableR;
  return evt;
}
