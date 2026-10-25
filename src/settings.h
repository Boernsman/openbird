#pragma once

#include <Arduino.h>

// Runtime settings, persisted in NVS. The values from config.h are the defaults.
struct Settings {
    String user;
    String password;
    uint32_t relayPulseMs;
};

extern Settings settings;

void settingsLoad();
void settingsSave();
void settingsReset(); // erase NVS, defaults apply after the next load
