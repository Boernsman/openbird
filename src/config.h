#pragma once

// Put your overrides (credentials, pins) into src/config_local.h.
// That file is git-ignored and included first, so its #defines win.
#if __has_include("config_local.h")
#include "config_local.h"
#endif

// ---- Credentials (HTTP Basic auth, same as a DoorBird user) ----------------
#ifndef OPENBIRD_USER
#define OPENBIRD_USER "openbird"
#endif
#ifndef OPENBIRD_PASSWORD
#define OPENBIRD_PASSWORD "changeme"
#endif

// ---- Doorbell button ------------------------------------------------------
// Default GPIO33 (EXT header), wired between pin and GND, internal pull-up.
// For a quick bench test use GPIO34 (on-board BUT1, has external pull-up).
#ifndef DOORBELL_PIN
#define DOORBELL_PIN 33
#endif
#ifndef DOORBELL_ACTIVE_LOW
#define DOORBELL_ACTIVE_LOW 1
#endif
#ifndef DOORBELL_PULLUP
#define DOORBELL_PULLUP 1 // GPIO34..39 have no internal pull-ups, set to 0 there
#endif
#ifndef DEBOUNCE_MS
#define DEBOUNCE_MS 50
#endif

// ---- Optional motion sensor input (PIR), -1 = not fitted --------------------
#ifndef MOTION_PIN
#define MOTION_PIN -1
#endif
#ifndef MOTION_ACTIVE_LOW
#define MOTION_ACTIVE_LOW 0
#endif

// ---- Door opener relay(s) --------------------------------------------------
// Relay 1 is "open-door.cgi?r=1", relay 2 is "?r=2" (-1 = not fitted).
#ifndef RELAY1_PIN
#define RELAY1_PIN 32
#endif
#ifndef RELAY2_PIN
#define RELAY2_PIN -1
#endif
#ifndef RELAY_ACTIVE_HIGH
#define RELAY_ACTIVE_HIGH 1 // many cheap relay modules are active low -> 0
#endif
#ifndef RELAY_PULSE_MS
#define RELAY_PULSE_MS 3000
#endif

// ---- Optional light output for "light-on.cgi", -1 = not fitted --------------
#ifndef LIGHT_PIN
#define LIGHT_PIN -1
#endif
#ifndef LIGHT_ON_MS
#define LIGHT_ON_MS 180000 // DoorBird keeps the IR light on for 3 minutes
#endif

// ---- Event monitor ----------------------------------------------------------
#ifndef MAX_MONITOR_CLIENTS
#define MAX_MONITOR_CLIENTS 4
#endif
#ifndef MONITOR_HEARTBEAT_MS
#define MONITOR_HEARTBEAT_MS 30000
#endif

// ---- Reported device info ----------------------------------------------------
#ifndef DEVICE_TYPE
#define DEVICE_TYPE "OpenBird ESP32-PoE-ISO"
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "000100"
#endif
