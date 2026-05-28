#pragma once
// =====================================================================
//  Pin assignments for ESP32-S3 N16R8 Web Radio
//  - Compatible with the Claude Butler board layout (shareable hardware)
//  - Avoid GPIO 33-37 (Octal PSRAM), GPIO 0 (BOOT), GPIO 19/20 (USB JTAG)
// =====================================================================

// ---- Diagnostic / verbose-serial mode (FIRST-BOOT DEFAULT only) -----
// Toggle this from the web UI (http://<radio-ip>/) for the live device.
// The value here is just the very-first-boot default before anything
// is saved to NVS, plus the station-list switch used in stations.h
// the first time the device runs.
//   1 : verbose Serial diagnostics, single test station, no auto-
//       restart on WiFi or heap problems.
//   0 : production. Concise logging, full station list, automatic
//       recovery (restart after 3 WiFi failures, 24 h scheduled reboot).
#define DIAG_MODE 0

// ---- MAX98357A I2S amplifier ----------------------------------------
#define I2S_BCLK    4
#define I2S_LRC     5
#define I2S_DOUT    6
// MAX98357A SD pin -> tie directly to 3.3V (always-on, RIGHT channel)

// ---- ST7789V TFT (SPI, hardware SPI on HSPI/FSPI bus) ---------------
#define TFT_CS     10
#define TFT_MOSI   11
#define TFT_SCLK   12
#define TFT_DC     18
#define TFT_RST    38
// NOTE: This TFT module has no BL pin -- backlight is hardwired to VCC
// on the PCB. Software cannot dim it; all backlight code was removed.

// ---- Buttons (INPUT_PULLUP, active LOW) -----------------------------
// LEFT  : short = volume down, long = previous station
// RIGHT : short = volume up,   long = next station
// LEFT + RIGHT held 1 s = system ON/OFF toggle (soft power)
#define BTN_LEFT    1
#define BTN_RIGHT   2

// ---- Status LED (optional, Claude Butler compatible) ----------------
// Green LED, 220 ohm series resistor to GND
#define LED_STATUS 41

// ---- TFT orientation -------------------------------------------------
// 1 = landscape, USB on the right
// 3 = landscape, USB on the left
#define TFT_ROTATION 1
#define TFT_W       320
#define TFT_H       240
