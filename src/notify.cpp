#include "notify.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "config.h"

// Same namespace as the settings, so a factory reset clears everything
static const char *NAMESPACE = "openbird";
static const size_t MAX_STORED_SIZE = 3900; // NVS strings are limited to 4000 bytes

static JsonDocument favorites; // {"sip":{"<id>":{"title":..,"value":..}},"http":{..}}
static JsonDocument schedules; // [ {"input":..,"param":..,"output":[..]} ]
static RelayHandler relayCallback = nullptr;
static QueueHandle_t requestQueue = nullptr;

// ---- Persistence ----------------------------------------------------------------

static void load(const char *key, JsonDocument &doc)
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false); // read-write: creates the namespace on first boot
    String json = prefs.isKey(key) ? prefs.getString(key) : String();
    prefs.end();
    if (json.isEmpty() || deserializeJson(doc, json))
        doc.clear();
}

static bool store(const char *key, const JsonDocument &doc)
{
    String json;
    serializeJson(doc, json);
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    bool ok = prefs.putString(key, json) == json.length();
    prefs.end();
    return ok;
}

static void ensureStructure()
{
    if (!favorites["sip"].is<JsonObject>())
        favorites["sip"].to<JsonObject>();
    if (!favorites["http"].is<JsonObject>())
        favorites["http"].to<JsonObject>();
    if (!schedules.is<JsonArray>())
        schedules.to<JsonArray>();
}

// ---- HTTP notification task ------------------------------------------------------

static void requestTask(void *)
{
    char *url;
    while (true) {
        if (xQueueReceive(requestQueue, &url, portMAX_DELAY) != pdTRUE)
            continue;
        HTTPClient http;
        http.setTimeout(NOTIFY_TIMEOUT_MS);
        http.setConnectTimeout(NOTIFY_TIMEOUT_MS);
        bool ok;
        if (strncmp(url, "https://", 8) == 0) {
            // Like DoorBird: encrypted, but certificates of LAN hosts are not checked
            WiFiClientSecure client;
            client.setInsecure();
            ok = http.begin(client, url);
            if (ok)
                Serial.printf("Notify %s -> %d\n", url, http.GET());
        } else {
            ok = http.begin(url);
            if (ok)
                Serial.printf("Notify %s -> %d\n", url, http.GET());
        }
        if (!ok)
            Serial.printf("Notify: invalid URL %s\n", url);
        http.end();
        free(url);
    }
}

static void queueRequest(const char *url)
{
    char *copy = strdup(url);
    if (!copy || xQueueSend(requestQueue, &copy, 0) != pdTRUE) {
        Serial.println("Notify queue full, dropping request");
        free(copy);
    }
}

// ---- Schedule evaluation -----------------------------------------------------------

static bool timeValid()
{
    return time(nullptr) > 1700000000; // set by NTP
}

static bool inRange(long t, long from, long to)
{
    // DoorBird's "always" example is from 82800 to 82799: windows may wrap around
    return from <= to ? (t >= from && t <= to) : (t >= from || t <= to);
}

static bool scheduleActive(JsonObjectConst schedule)
{
    if (!timeValid()) {
        // Without a clock, rather notify too often than miss a ring
        return true;
    }
    time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);
    long weekSecond = utc.tm_wday * 86400L + utc.tm_hour * 3600L + utc.tm_min * 60L + utc.tm_sec;

    for (JsonObjectConst range : schedule["weekdays"].as<JsonArrayConst>()) {
        if (inRange(weekSecond, atol(range["from"] | "0"), atol(range["to"] | "0")))
            return true;
    }
    for (JsonObjectConst range : schedule["from-to"].as<JsonArrayConst>()) {
        if (inRange((long)now, atol(range["from"] | "0"), atol(range["to"] | "0")))
            return true;
    }
    return false;
}

void notifyTrigger(const char *input)
{
    for (JsonObjectConst entry : schedules.as<JsonArrayConst>()) {
        if (strcmp(entry["input"] | "", input) != 0)
            continue;
        for (JsonObjectConst output : entry["output"].as<JsonArrayConst>()) {
            if (strcmp(output["enabled"] | "1", "0") == 0 || !scheduleActive(output["schedule"]))
                continue;
            const char *event = output["event"] | "";
            const char *param = output["param"] | "";
            if (strcmp(event, "http") == 0) {
                const char *url = favorites["http"][param]["value"] | "";
                if (*url)
                    queueRequest(url);
            } else if (strcmp(event, "relay") == 0) {
                if (relayCallback)
                    relayCallback(atoi(param));
            }
        }
    }
}

// ---- favorites.cgi -----------------------------------------------------------------

String favoritesJson()
{
    String json;
    serializeJson(favorites, json);
    return json;
}

int favoritesCount()
{
    return favorites["sip"].size() + favorites["http"].size();
}

int favoritesSave(const String &type, const String &title, const String &value, const String &id, int &savedId)
{
    if ((type != "http" && type != "sip") || value.isEmpty())
        return 400;
    const char *other = type == "http" ? "sip" : "http";

    if (id.isEmpty()) {
        // New favorite: next free id over both types
        int next = 0;
        for (const char *t : {"sip", "http"}) {
            for (JsonPairConst kv : favorites[t].as<JsonObjectConst>())
                next = max(next, atoi(kv.key().c_str()) + 1);
        }
        savedId = next;
    } else {
        savedId = id.toInt();
        if (String(savedId) != id || savedId < 0)
            return 400;
        // Switching the type of an existing favorite is not allowed
        if (favorites[other][id].is<JsonObject>())
            return 400;
    }

    JsonDocument backup = favorites;
    JsonObject fav = favorites[type][String(savedId)].to<JsonObject>();
    fav["title"] = title;
    fav["value"] = value;
    if (measureJson(favorites) > MAX_STORED_SIZE) {
        favorites = backup;
        return 507;
    }
    return store("favorites", favorites) ? 200 : 500;
}

int favoritesRemove(const String &type, const String &id)
{
    if ((type != "http" && type != "sip") || id.isEmpty())
        return 400;
    favorites[type].remove(id);

    // Drop schedule outputs that used this favorite, and entries left without outputs
    JsonArray entries = schedules.as<JsonArray>();
    for (size_t i = entries.size(); i-- > 0;) {
        JsonArray outputs = entries[i]["output"];
        for (size_t j = outputs.size(); j-- > 0;) {
            if (type == (outputs[j]["event"] | "") && id == (outputs[j]["param"] | ""))
                outputs.remove(j);
        }
        if (outputs.size() == 0)
            entries.remove(i);
    }
    return store("favorites", favorites) && store("schedules", schedules) ? 200 : 500;
}

// ---- schedule.cgi --------------------------------------------------------------------

int schedulesCount()
{
    return schedules.size();
}

int schedulesJson(const String &input, String &json)
{
    if (input.isEmpty()) {
        serializeJson(schedules, json);
        return 200;
    }
    JsonDocument filtered;
    JsonArray list = filtered.to<JsonArray>();
    for (JsonObjectConst entry : schedules.as<JsonArrayConst>()) {
        if (input == (entry["input"] | ""))
            list.add(entry);
    }
    if (list.size() == 0)
        return 204;
    serializeJson(filtered, json);
    return 200;
}

int scheduleSave(const String &body)
{
    JsonDocument entry;
    if (body.isEmpty() || deserializeJson(entry, body) || !entry.is<JsonObject>() ||
        !entry["input"].is<const char *>() || !entry["output"].is<JsonArray>())
        return 400;
    if (!entry["param"].is<const char *>())
        entry["param"] = "";

    JsonDocument backup = schedules;
    JsonArray entries = schedules.as<JsonArray>();
    bool replaced = false;
    for (JsonObject existing : entries) {
        if (existing["input"] == entry["input"] && existing["param"] == entry["param"]) {
            existing.set(entry.as<JsonObjectConst>());
            replaced = true;
            break;
        }
    }
    if (!replaced)
        entries.add(entry);

    if (measureJson(schedules) > MAX_STORED_SIZE) {
        schedules = backup;
        return 507;
    }
    return store("schedules", schedules) ? 200 : 500;
}

int scheduleRemove(const String &input, const String &param)
{
    if (input.isEmpty())
        return 400;
    JsonArray entries = schedules.as<JsonArray>();
    for (size_t i = entries.size(); i-- > 0;) {
        if (input == (entries[i]["input"] | "") && param == (entries[i]["param"] | ""))
            entries.remove(i);
    }
    return store("schedules", schedules) ? 200 : 500;
}

// ---- Setup ------------------------------------------------------------------------------

void notifyBegin(RelayHandler relayHandler)
{
    relayCallback = relayHandler;
    load("favorites", favorites);
    load("schedules", schedules);
    ensureStructure();
    Serial.printf("Loaded %d favorites, %d schedules\n", favoritesCount(), schedulesCount());

    requestQueue = xQueueCreate(8, sizeof(char *));
    xTaskCreate(requestTask, "notify", 12288, nullptr, 1, nullptr); // TLS handshakes need the stack
}

void notifyReset()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    prefs.remove("favorites");
    prefs.remove("schedules");
    prefs.end();
}
