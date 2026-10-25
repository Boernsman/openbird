#pragma once

// Put your overrides (credentials, pins) into src/config_local.h.
// That file is git-ignored and included first, so its #defines win.
#if __has_include("config_local.h")
#include "config_local.h"
#endif

// ---- Default credentials (HTTP Basic auth, same as a DoorBird user) ---------
// Used until changed on the settings page; also the ArduinoOTA password.
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
#define RELAY_PULSE_MS 3000 // default, adjustable on the settings page
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

// ---- Factory reset ---------------------------------------------------------------
// Hold the on-board BUT1 (GPIO34) for RESET_HOLD_MS to restore the defaults.
// Disabled automatically when BUT1 is used as the doorbell.
#ifndef RESET_PIN
#define RESET_PIN 34
#endif
#ifndef RESET_HOLD_MS
#define RESET_HOLD_MS 10000
#endif

// ---- Watchdog ------------------------------------------------------------------------
#ifndef WATCHDOG_TIMEOUT_S
#define WATCHDOG_TIMEOUT_S 30
#endif

// ---- Notifications ---------------------------------------------------------------
#ifndef NOTIFY_TIMEOUT_MS
#define NOTIFY_TIMEOUT_MS 5000 // per HTTP favorite request
#endif
// Schedules are evaluated in UTC
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

// ---- Reported device info ----------------------------------------------------
#ifndef DEVICE_TYPE
#define DEVICE_TYPE "OpenBird ESP32-PoE-ISO"
#endif
// Favorites and schedules require at least 000110 on a real DoorBird, clients check this
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "000130"
#endif
#ifndef BUILD_NUMBER
#define BUILD_NUMBER "1"
#endif
