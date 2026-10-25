#include "settings.h"

#include <Preferences.h>

#include "config.h"

Settings settings;

static const char *NAMESPACE = "openbird";

void settingsLoad()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, true);
    settings.user = prefs.getString("user", OPENBIRD_USER);
    settings.password = prefs.getString("password", OPENBIRD_PASSWORD);
    settings.relayPulseMs = prefs.getUInt("relayPulse", RELAY_PULSE_MS);
    prefs.end();
}

void settingsSave()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    prefs.putString("user", settings.user);
    prefs.putString("password", settings.password);
    prefs.putUInt("relayPulse", settings.relayPulseMs);
    prefs.end();
}

void settingsReset()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    prefs.clear();
    prefs.end();
}
