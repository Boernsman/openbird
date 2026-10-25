#pragma once

#include <Arduino.h>

// Favorites and schedules (DoorBird favorites.cgi / schedule.cgi), persisted in NVS.
// Schedule outputs of type "http" call the referenced HTTP favorite, "relay"
// pulses a relay; "notify" and "sip" are accepted but ignored.

using RelayHandler = void (*)(int relay);

void notifyBegin(RelayHandler relayHandler);
void notifyReset(); // erase stored favorites and schedules

// Fire the schedule outputs for an input event ("doorbell" or "motion")
void notifyTrigger(const char *input);

// favorites.cgi, the functions return the HTTP status code
String favoritesJson();
int favoritesSave(const String &type, const String &title, const String &value, const String &id, int &savedId);
int favoritesRemove(const String &type, const String &id);
int favoritesCount();

// schedule.cgi
int schedulesJson(const String &input, String &json);
int scheduleSave(const String &body);
int scheduleRemove(const String &input, const String &param);
int schedulesCount();
