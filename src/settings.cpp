#include "settings.h"

#include <Preferences.h>

#include "config.h"

Settings settings;

static const char *NAMESPACE = "openbird";

void settingsLoad()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false); // read-write: creates the namespace on first boot
    // isKey() first: Preferences logs an error for every missing key
    settings.user = prefs.isKey("user") ? prefs.getString("user") : String(OPENBIRD_USER);
    settings.password = prefs.isKey("password") ? prefs.getString("password") : String(OPENBIRD_PASSWORD);
    settings.relayPulseMs = prefs.isKey("relayPulse") ? prefs.getUInt("relayPulse") : RELAY_PULSE_MS;
    settings.volume = prefs.isKey("volume") ? prefs.getUChar("volume") : CHIME_VOLUME;
    prefs.end();
}

void settingsSave()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    prefs.putString("user", settings.user);
    prefs.putString("password", settings.password);
    prefs.putUInt("relayPulse", settings.relayPulseMs);
    prefs.putUChar("volume", settings.volume);
    prefs.end();
}

void settingsReset()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    prefs.clear();
    prefs.end();
}
