// OpenBird - DoorBird LAN API compatible door station for the Olimex ESP32-PoE-ISO.
//
// Implements the subset of the DoorBird LAN API that the nymea DoorBird plugin uses:
//   mDNS:  _axis-video._tcp, host "bha-<mac>", TXT "macaddress=<MAC>"
//   HTTP:  /bha-api/getsession.cgi, info.cgi, open-door.cgi, light-on.cgi,
//          monitor.cgi (multipart event stream), favorites.cgi, schedule.cgi, restart.cgi

#include <Arduino.h>
#include <ETH.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_mac.h>

#include "config.h"

static WiFiServer server(80);
static bool ethConnected = false;
static bool mdnsStarted = false;

static String macAddress;    // "1CCAE3AABBCC" style, upper case, no colons
static String sessionId;
static String expectedAuth;  // "Basic <base64(user:pass)>"

// ---- Outputs ----------------------------------------------------------------

struct Output {
    int pin;
    bool activeHigh;
    uint32_t offAt; // millis() when to switch off, 0 = off
};

static Output relays[2] = {
    {RELAY1_PIN, RELAY_ACTIVE_HIGH, 0},
    {RELAY2_PIN, RELAY_ACTIVE_HIGH, 0},
};
static Output light = {LIGHT_PIN, true, 0};

static void outputWrite(const Output &o, bool on)
{
    if (o.pin >= 0)
        digitalWrite(o.pin, on == o.activeHigh ? HIGH : LOW);
}

static void outputInit(Output &o)
{
    if (o.pin < 0)
        return;
    outputWrite(o, false);
    pinMode(o.pin, OUTPUT);
}

static void outputPulse(Output &o, uint32_t durationMs)
{
    outputWrite(o, true);
    o.offAt = millis() + durationMs;
    if (o.offAt == 0)
        o.offAt = 1;
}

static void outputLoop(Output &o)
{
    if (o.offAt && (int32_t)(millis() - o.offAt) >= 0) {
        outputWrite(o, false);
        o.offAt = 0;
    }
}

// ---- Inputs -----------------------------------------------------------------

struct Input {
    const char *name; // event name in monitor.cgi
    int pin;
    bool activeLow;
    bool state;       // debounced, true = active
    bool raw;
    uint32_t changedAt;
};

static Input doorbell = {"doorbell", DOORBELL_PIN, DOORBELL_ACTIVE_LOW, false, false, 0};
static Input motion = {"motionsensor", MOTION_PIN, MOTION_ACTIVE_LOW, false, false, 0};

static void inputInit(Input &in, bool pullup)
{
    if (in.pin < 0)
        return;
    pinMode(in.pin, pullup ? INPUT_PULLUP : INPUT);
}

// Returns true when the debounced state changed.
static bool inputLoop(Input &in)
{
    if (in.pin < 0)
        return false;
    bool raw = (digitalRead(in.pin) == HIGH) != in.activeLow;
    if (raw != in.raw) {
        in.raw = raw;
        in.changedAt = millis();
        return false;
    }
    if (raw != in.state && millis() - in.changedAt >= DEBOUNCE_MS) {
        in.state = raw;
        return true;
    }
    return false;
}

// ---- Event monitor (monitor.cgi) ---------------------------------------------

struct MonitorClient {
    WiFiClient client;
    bool doorbell;
    bool motion;
};

static MonitorClient monitors[MAX_MONITOR_CLIENTS];
static uint32_t lastHeartbeat = 0;

static bool monitorSend(MonitorClient &m, const Input &in)
{
    String part = "--ioboundary\r\nContent-Type: text/plain\r\n\r\n";
    part += in.name;
    part += in.state ? ":H" : ":L";
    part += "\r\n\r\n";
    if (m.client.write((const uint8_t *)part.c_str(), part.length()) != part.length()) {
        m.client.stop();
        return false;
    }
    return true;
}

static void monitorBroadcast(const Input &in)
{
    for (MonitorClient &m : monitors) {
        if (!m.client || !m.client.connected())
            continue;
        if ((&in == &doorbell && m.doorbell) || (&in == &motion && m.motion))
            monitorSend(m, in);
    }
}

// Heartbeats skip active inputs, a repeated "doorbell:H" would count as another ring
static void monitorSendAll(MonitorClient &m, bool heartbeat = false)
{
    if (m.doorbell && doorbell.pin >= 0 && !(heartbeat && doorbell.state) && !monitorSend(m, doorbell))
        return;
    if (m.motion && motion.pin >= 0 && !(heartbeat && motion.state))
        monitorSend(m, motion);
}

static void monitorLoop()
{
    for (MonitorClient &m : monitors) {
        if (m.client && !m.client.connected())
            m.client.stop();
        // Drain anything the client sends so the socket stays healthy
        while (m.client && m.client.available())
            m.client.read();
    }
    if (millis() - lastHeartbeat >= MONITOR_HEARTBEAT_MS) {
        lastHeartbeat = millis();
        for (MonitorClient &m : monitors) {
            if (m.client && m.client.connected())
                monitorSendAll(m, true);
        }
    }
}

// ---- HTTP helpers --------------------------------------------------------------

struct Request {
    String method;
    String path;
    String query;
    String authorization;
};

static String urlDecode(const String &s)
{
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.length()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

static String queryParam(const String &query, const char *key, bool *found = nullptr)
{
    String k = String(key) + "=";
    int pos = 0;
    while (pos <= (int)query.length()) {
        int end = query.indexOf('&', pos);
        if (end < 0)
            end = query.length();
        String item = query.substring(pos, end);
        if (item.startsWith(k)) {
            if (found)
                *found = true;
            return urlDecode(item.substring(k.length()));
        }
        if (item == key) {
            if (found)
                *found = true;
            return String();
        }
        pos = end + 1;
    }
    if (found)
        *found = false;
    return String();
}

static bool readRequest(WiFiClient &client, Request &req)
{
    client.setTimeout(2);
    String line = client.readStringUntil('\n');
    line.trim();
    int sp1 = line.indexOf(' ');
    int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0)
        return false;
    req.method = line.substring(0, sp1);
    String target = line.substring(sp1 + 1, sp2);
    int q = target.indexOf('?');
    req.path = q < 0 ? target : target.substring(0, q);
    req.query = q < 0 ? String() : target.substring(q + 1);

    // Headers
    while (client.connected()) {
        String h = client.readStringUntil('\n');
        h.trim();
        if (h.isEmpty())
            break;
        if (h.length() > 14 && h.substring(0, 14).equalsIgnoreCase("Authorization:")) {
            req.authorization = h.substring(14);
            req.authorization.trim();
        }
    }
    return true;
}

static void sendResponse(WiFiClient &client, int code, const char *status,
                         const char *contentType, const String &body, const char *extraHeaders = "")
{
    String head = "HTTP/1.1 " + String(code) + " " + status + "\r\n";
    head += "Content-Type: " + String(contentType) + "\r\n";
    head += "Content-Length: " + String(body.length()) + "\r\n";
    head += extraHeaders;
    head += "Connection: close\r\n\r\n";
    client.print(head);
    client.print(body);
}

static void sendJson(WiFiClient &client, const String &json)
{
    sendResponse(client, 200, "OK", "application/json", json);
}

static void sendOk(WiFiClient &client)
{
    sendJson(client, "{\"BHA\":{\"RETURNCODE\":\"1\"}}");
}

static bool isAuthorized(const Request &req)
{
    if (req.authorization == expectedAuth)
        return true;
    // DoorBird also accepts credentials or a session id as query parameters
    if (queryParam(req.query, "http-user") == OPENBIRD_USER &&
        queryParam(req.query, "http-password") == OPENBIRD_PASSWORD)
        return true;
    bool hasSession = false;
    String sid = queryParam(req.query, "sessionid", &hasSession);
    return hasSession && sid == sessionId;
}

// ---- API handlers ---------------------------------------------------------------

static void handleInfo(WiFiClient &client)
{
    String relayList = "\"1\"";
    if (relays[1].pin >= 0)
        relayList += ",\"2\"";
    String json = "{\"BHA\":{\"RETURNCODE\":\"1\",\"VERSION\":[{";
    json += "\"FIRMWARE\":\"" FIRMWARE_VERSION "\",";
    json += "\"BUILD_NUMBER\":\"" __DATE__ "\",";
    json += "\"PRIMARY_MAC_ADDR\":\"" + macAddress + "\",";
    json += "\"WIFI_MAC_ADDR\":\"" + macAddress + "\",";
    json += "\"RELAYS\":[" + relayList + "],";
    json += "\"DEVICE-TYPE\":\"" DEVICE_TYPE "\"";
    json += "}]}}";
    sendJson(client, json);
}

static void handleOpenDoor(WiFiClient &client, const Request &req)
{
    String r = queryParam(req.query, "r");
    int index = r.isEmpty() ? 1 : r.toInt();
    if (index < 1 || index > 2 || relays[index - 1].pin < 0) {
        sendResponse(client, 400, "Bad Request", "text/plain", "Unknown relay\n");
        return;
    }
    Serial.printf("Opening door, relay %d\n", index);
    outputPulse(relays[index - 1], RELAY_PULSE_MS);
    sendOk(client);
}

static void handleLightOn(WiFiClient &client)
{
    if (light.pin >= 0)
        outputPulse(light, LIGHT_ON_MS);
    sendOk(client);
}

static bool handleMonitor(WiFiClient &client, const Request &req)
{
    String ring = queryParam(req.query, "ring");
    bool wantDoorbell = ring.isEmpty() || ring.indexOf("doorbell") >= 0;
    bool wantMotion = ring.isEmpty() || ring.indexOf("motionsensor") >= 0;

    int slot = -1;
    for (int i = 0; i < MAX_MONITOR_CLIENTS; i++) {
        if (!monitors[i].client || !monitors[i].client.connected()) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // DoorBird answers with 509 when too many monitor connections are open
        sendResponse(client, 509, "Bandwidth Limit Exceeded", "text/plain", "Too many monitor clients\n");
        return false;
    }

    client.print("HTTP/1.1 200 OK\r\n"
                 "Content-Type: multipart/x-mixed-replace; boundary=--ioboundary\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: close\r\n\r\n");
    monitors[slot].client = client;
    monitors[slot].doorbell = wantDoorbell;
    monitors[slot].motion = wantMotion;
    monitorSendAll(monitors[slot]); // report the current state right away
    Serial.printf("Monitor client %s connected (slot %d)\n", client.remoteIP().toString().c_str(), slot);
    return true;
}

// Returns true if the client connection was handed over and must not be closed.
static bool handleRequest(WiFiClient &client, const Request &req)
{
    if (!req.path.startsWith("/bha-api/")) {
        if (req.path == "/") {
            sendResponse(client, 200, "OK", "text/plain",
                         "OpenBird " DEVICE_TYPE "\nMAC " + macAddress + "\nIP " + ETH.localIP().toString() + "\n");
        } else {
            sendResponse(client, 404, "Not Found", "text/plain", "Not found\n");
        }
        return false;
    }

    if (!isAuthorized(req)) {
        sendResponse(client, 401, "Unauthorized", "text/plain", "Unauthorized\n",
                     "WWW-Authenticate: Basic realm=\"DoorBird\"\r\n");
        return false;
    }

    const String cgi = req.path.substring(9);
    if (cgi == "getsession.cgi") {
        if (!queryParam(req.query, "invalidate").isEmpty()) {
            sendOk(client);
        } else {
            sendJson(client, "{\"BHA\":{\"RETURNCODE\":\"1\",\"SESSIONID\":\"" + sessionId +
                                 "\",\"NOTIFICATION_ENCRYPTION_KEY\":\"\"}}");
        }
    } else if (cgi == "info.cgi") {
        handleInfo(client);
    } else if (cgi == "open-door.cgi") {
        handleOpenDoor(client, req);
    } else if (cgi == "light-on.cgi") {
        handleLightOn(client);
    } else if (cgi == "monitor.cgi") {
        return handleMonitor(client, req);
    } else if (cgi == "favorites.cgi") {
        // No notification targets are stored; report an empty list and accept saves
        if (req.query.indexOf("action=") >= 0)
            sendOk(client);
        else
            sendJson(client, "{\"http\":{},\"sip\":{}}");
    } else if (cgi == "schedule.cgi") {
        sendJson(client, "[]");
    } else if (cgi == "restart.cgi") {
        sendOk(client);
        client.flush();
        client.stop();
        delay(200);
        ESP.restart();
    } else if (cgi == "image.cgi" || cgi == "video.cgi" || cgi == "history.cgi" ||
               cgi == "audio-receive.cgi" || cgi == "audio-transmit.cgi") {
        sendResponse(client, 503, "Service Unavailable", "text/plain", "No camera/audio on this device\n");
    } else {
        sendResponse(client, 404, "Not Found", "text/plain", "Not found\n");
    }
    return false;
}

static void httpLoop()
{
    WiFiClient client = server.available();
    if (!client)
        return;
    Request req;
    if (!readRequest(client, req)) {
        client.stop();
        return;
    }
    Serial.printf("%s %s %s\n", client.remoteIP().toString().c_str(), req.method.c_str(), req.path.c_str());
    if (!handleRequest(client, req)) {
        client.flush();
        client.stop();
    }
}

// ---- Network ----------------------------------------------------------------------

static void startMdns()
{
    if (mdnsStarted)
        return;
    String host = "bha-" + macAddress;
    host.toLowerCase();
    if (!MDNS.begin(host.c_str())) {
        Serial.println("mDNS start failed");
        return;
    }
    MDNS.setInstanceName("OpenBird - " + macAddress);
    MDNS.addService("http", "tcp", 80);
    // The nymea plugin browses _axis-video._tcp, requires the host name to start with
    // "bha-" and reads the MAC from the *first* TXT record ("macaddress=...").
    MDNS.addService("axis-video", "tcp", 80);
    MDNS.addServiceTxt("axis-video", "tcp", "macaddress", macAddress);
    mdnsStarted = true;
    Serial.printf("mDNS: %s.local\n", host.c_str());
}

static void onNetworkEvent(WiFiEvent_t event)
{
    switch (event) {
    case ARDUINO_EVENT_ETH_START: {
        String host = "openbird-" + macAddress.substring(6);
        host.toLowerCase();
        ETH.setHostname(host.c_str()); // DHCP host name
        break;
    }
    case ARDUINO_EVENT_ETH_CONNECTED:
        Serial.println("ETH link up");
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
        Serial.printf("ETH IP %s, %s, %dMbps\n", ETH.localIP().toString().c_str(),
                      ETH.fullDuplex() ? "full duplex" : "half duplex", ETH.linkSpeed());
        ethConnected = true;
        startMdns();
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        Serial.println("ETH link down");
        ethConnected = false;
        break;
    case ARDUINO_EVENT_ETH_STOP:
        ethConnected = false;
        break;
    default:
        break;
    }
}

static String randomHex(size_t bytes)
{
    String s;
    for (size_t i = 0; i < bytes; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", (unsigned)(esp_random() & 0xff));
        s += buf;
    }
    return s;
}

// ---- Main ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    Serial.println("\nOpenBird starting");

    outputInit(relays[0]);
    outputInit(relays[1]);
    outputInit(light);
    inputInit(doorbell, DOORBELL_PULLUP);
    inputInit(motion, false);

    expectedAuth = "Basic " + base64::encode(String(OPENBIRD_USER ":" OPENBIRD_PASSWORD));
    sessionId = randomHex(32);

    // Read the MAC before ETH.begin(), the ETH_START event already needs it
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    char macStr[13];
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    macAddress = macStr;

    WiFi.onEvent(onNetworkEvent);
    ETH.begin(); // LAN8720 pin mapping comes from the esp32-poe-iso board variant

    server.begin();
}

void loop()
{
    if (inputLoop(doorbell)) {
        Serial.printf("Doorbell %s\n", doorbell.state ? "pressed" : "released");
        monitorBroadcast(doorbell);
    }
    if (inputLoop(motion)) {
        Serial.printf("Motion %s\n", motion.state ? "detected" : "cleared");
        monitorBroadcast(motion);
    }

    outputLoop(relays[0]);
    outputLoop(relays[1]);
    outputLoop(light);

    if (ethConnected)
        httpLoop();
    monitorLoop();

    delay(1);
}
