#pragma once
#include <Arduino.h>
#include <WebServer.h>

class Config;

// =====================================================================
//  Minimal HTTP setup server (port 80). Renders a form pre-filled with
//  the current Config values, accepts a POST, persists to NVS and
//  reboots so the new config takes effect cleanly.
//
//  Endpoints:
//    GET  /            -> setup form (HTML)
//    POST /save        -> persist + restart
//    GET  /api/status  -> small JSON for the page header
//
//  Captive portal mode (begin(cfg, true)):
//    Replies to common OS connectivity-check URLs by redirecting to /,
//    so the phone/laptop pops the setup page automatically when it
//    joins the AP. Pairs with a DNSServer in main.cpp that resolves
//    every name to the ESP's IP.
//
//  Designed to coexist with audio playback - keeps each request short.
//  Caller must call handle() from loop().
// =====================================================================
class WebUI {
public:
  void begin(Config* cfg, bool captivePortal = false);
  void handle();

  // Set by main.cpp (e.g. from healthSnapshot) so the status pill on
  // the page can show what's playing without WebUI needing to know
  // about the player.
  void setStatusText(const String& s) { _status = s; }

private:
  WebServer _server{80};
  Config*   _cfg = nullptr;
  String    _status;
  bool      _captive = false;

  void handleRoot();
  void handleSave();
  void handleStatus();
  void handleNotFound();
  void handleCaptiveRedirect();   // 302 -> http://<ap-ip>/
  void handleFactoryReset();      // wipe NVS and restart into AP mode
};
