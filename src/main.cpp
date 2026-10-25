// OpenBird - DoorBird LAN API compatible door station for the Olimex ESP32-PoE-ISO.
//
// Implements the subset of the DoorBird LAN API that the nymea DoorBird plugin uses:
//   mDNS:  _axis-video._tcp, host "bha-<mac>", TXT "macaddress=<MAC>"
//   HTTP:  /bha-api/getsession.cgi, info.cgi, open-door.cgi, light-on.cgi,
//          monitor.cgi (multipart event stream), favorites.cgi, schedule.cgi, restart.cgi
// Favorites and schedules trigger HTTP notifications and relays on ring/motion.
// Plus a settings page on "/", ArduinoOTA updates and factory reset via BUT1.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ETH.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <lwip/sockets.h>

#include "config.h"
#include "notify.h"
#include "settings.h"
#include "webpage.h"

static int listenFd = -1;
static volatile bool ethConnected = false;
static bool mdnsStarted = false;
static bool otaStarted = false;
static bool ntpStarted = false;

static String macAddress;    // "1CCAE3AABBCC" style, upper case, no colons
static String mdnsHost;      // "bha-1ccae3aabbcc"
static String sessionId;
static String notificationKey;
static String expectedAuth;  // "Basic <base64(user:pass)>"
static String formToken;     // guards the settings form against cross-site posts

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
    uint32_t connectedAt;
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

static int monitorCount()
{
    int count = 0;
    for (MonitorClient &m : monitors) {
        if (m.client && m.client.connected())
            count++;
    }
    return count;
}

static void monitorDisconnectAll()
{
    for (MonitorClient &m : monitors)
        m.client.stop();
}

// TCP keepalive drops clients that vanished without closing the connection,
// so their slot frees up and writes never pile up in the send buffer.
static void enableKeepalive(WiFiClient &client)
{
    int fd = client.fd();
    if (fd < 0)
        return;
    int on = 1, idle = 30, interval = 5, count = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
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

// ---- HTTP server socket -----------------------------------------------------------
// WiFiServer only listens on IPv4. With IPv6 enabled, mDNS also announces the
// link-local IPv6 address and clients may connect over it, so listen dual-stack:
// lwIP accepts IPv4 on an IPv6 socket bound to the "any" address.

static bool httpListen(uint16_t port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port); // sin6_addr all zero = any address, IPv4 and IPv6
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    listenFd = fd;
    return true;
}

static WiFiClient httpAccept()
{
    if (listenFd < 0)
        return WiFiClient();
    int fd = accept(listenFd, nullptr, nullptr);
    if (fd < 0)
        return WiFiClient();
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    return WiFiClient(fd);
}

// WiFiClient::remoteIP() only understands IPv4 socket addresses
static String peerName(WiFiClient &client)
{
    struct sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (getpeername(client.fd(), (struct sockaddr *)&addr, &len) < 0)
        return "?";
    char buf[INET6_ADDRSTRLEN] = "?";
    if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, buf, sizeof(buf));
    } else if (addr.ss_family == AF_INET6) {
        struct in6_addr *a = &((struct sockaddr_in6 *)&addr)->sin6_addr;
        // IPv4 clients on the dual-stack socket appear as ::ffff:a.b.c.d
        static const uint8_t v4mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(a->s6_addr, v4mapped, sizeof(v4mapped)) == 0)
            inet_ntop(AF_INET, &a->s6_addr[12], buf, sizeof(buf));
        else
            inet_ntop(AF_INET6, a, buf, sizeof(buf));
    }
    return buf;
}

// ---- HTTP helpers --------------------------------------------------------------

struct Request {
    String method;
    String path;
    String query;
    String authorization;
    long contentLength = -1; // -1 = no Content-Length header
    String body;             // only read up to MAX_BODY_SIZE
};

static const long MAX_BODY_SIZE = 4096;

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
        } else if (h.length() > 15 && h.substring(0, 15).equalsIgnoreCase("Content-Length:")) {
            req.contentLength = h.substring(15).toInt();
        }
    }

    if (req.contentLength > 0 && req.contentLength <= MAX_BODY_SIZE) {
        std::unique_ptr<char[]> buf(new char[req.contentLength + 1]);
        size_t n = client.readBytes(buf.get(), req.contentLength);
        buf[n] = 0;
        req.body = buf.get();
    }
    return true;
}

static String htmlEscape(const String &s)
{
    String out;
    out.reserve(s.length());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
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
    bool hasUser = false;
    String user = queryParam(req.query, "http-user", &hasUser);
    if (hasUser && user == settings.user && queryParam(req.query, "http-password") == settings.password)
        return true;
    bool hasSession = false;
    String sid = queryParam(req.query, "sessionid", &hasSession);
    return hasSession && sid == sessionId;
}

static void sendUnauthorized(WiFiClient &client)
{
    // Keep the realm constant: Qt caches the credentials from getsession.cgi per
    // host + realm and resends them when later requests get a 401.
    sendResponse(client, 401, "Unauthorized", "text/plain", "Unauthorized\n",
                 "WWW-Authenticate: Basic realm=\"DoorBird\"\r\n");
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

// Called at boot and whenever the credentials change
static void applyCredentials()
{
    expectedAuth = "Basic " + base64::encode(settings.user + ":" + settings.password);
    sessionId = randomHex(32);
    monitorDisconnectAll();
    if (otaStarted) {
        ArduinoOTA.end();
        ArduinoOTA.setPassword(settings.password.c_str());
        ArduinoOTA.begin();
    }
}

// ---- API handlers ---------------------------------------------------------------

static void handleInfo(WiFiClient &client)
{
    String relayList = "\"1\"";
    if (relays[1].pin >= 0)
        relayList += ",\"2\"";
    String json = "{\"BHA\":{\"RETURNCODE\":\"1\",\"VERSION\":[{";
    json += "\"FIRMWARE\":\"" FIRMWARE_VERSION "\",";
    json += "\"BUILD_NUMBER\":\"" BUILD_NUMBER "\",";
    json += "\"PRIMARY_MAC_ADDR\":\"" + macAddress + "\",";
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
    outputPulse(relays[index - 1], settings.relayPulseMs);
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
    // Legacy one-shot state query, still used by some clients: "doorbell=0|1"
    String check = queryParam(req.query, "check");
    if (!check.isEmpty()) {
        const Input *in = check == "doorbell" ? &doorbell : check == "motionsensor" ? &motion : nullptr;
        if (!in) {
            sendResponse(client, 400, "Bad Request", "text/plain", "Invalid check\n");
        } else {
            sendResponse(client, 200, "OK", "text/plain", check + "=" + (in->state ? "1" : "0") + "\r\n");
        }
        return false;
    }

    String ring = queryParam(req.query, "ring");
    bool wantDoorbell = ring.isEmpty() || ring.indexOf("doorbell") >= 0;
    bool wantMotion = ring.isEmpty() || ring.indexOf("motionsensor") >= 0;

    // Take a free slot, or replace the oldest connection: a client that reconnects
    // must not be locked out by its own stale connection.
    int slot = 0;
    for (int i = 0; i < MAX_MONITOR_CLIENTS; i++) {
        if (!monitors[i].client || !monitors[i].client.connected()) {
            slot = i;
            break;
        }
        if ((int32_t)(monitors[i].connectedAt - monitors[slot].connectedAt) < 0)
            slot = i;
    }
    monitors[slot].client.stop();

    client.print("HTTP/1.1 200 OK\r\n"
                 "Content-Type: multipart/x-mixed-replace; boundary=--ioboundary\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: close\r\n\r\n");
    enableKeepalive(client);
    monitors[slot].client = client;
    monitors[slot].doorbell = wantDoorbell;
    monitors[slot].motion = wantMotion;
    monitors[slot].connectedAt = millis();
    monitorSendAll(monitors[slot]); // report the current state right away
    Serial.printf("Monitor client %s connected (slot %d)\n", peerName(client).c_str(), slot);
    return true;
}

// ---- Settings page ----------------------------------------------------------------

static String formatUptime()
{
    uint32_t s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
    return buf;
}

static String formatTime()
{
    time_t now = time(nullptr);
    if (now < 1700000000)
        return "not synchronized";
    struct tm utc;
    gmtime_r(&now, &utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &utc);
    return buf;
}

static void sendSettingsPage(WiFiClient &client, const String &message, bool error)
{
    String page = SETTINGS_PAGE;
    page.replace("%MESSAGE%", message.isEmpty() ? String()
                                                : String("<p class=\"msg") + (error ? " err" : "") + "\">" +
                                                      htmlEscape(message) + "</p>");
    page.replace("%DEVICE%", DEVICE_TYPE);
    page.replace("%FIRMWARE%", FIRMWARE_VERSION " (" __DATE__ ")");
    page.replace("%MAC%", macAddress);
    page.replace("%IP%", ETH.localIP().toString());
    page.replace("%HOST%", mdnsHost);
    page.replace("%UPTIME%", formatUptime());
    page.replace("%DOORBELL%", doorbell.pin < 0 ? "not fitted" : doorbell.state ? "pressed" : "idle");
    page.replace("%MONITORS%", String(monitorCount()) + " / " + String(MAX_MONITOR_CLIENTS));
    page.replace("%NOTIFY%", String(favoritesCount()) + " favorites, " + String(schedulesCount()) + " schedules");
    page.replace("%TIME%", formatTime());
    page.replace("%TOKEN%", formToken);
    page.replace("%USER%", htmlEscape(settings.user));
    page.replace("%PULSE%", String(settings.relayPulseMs));
    sendResponse(client, 200, "OK", "text/html; charset=utf-8", page, "Cache-Control: no-store\r\n");
}

static void handleSettingsPost(WiFiClient &client, const Request &req)
{
    if (queryParam(req.body, "token") != formToken) {
        sendSettingsPage(client, "Form expired, please try again.", true);
        return;
    }
    String user = queryParam(req.body, "user");
    String password = queryParam(req.body, "password");
    long pulse = queryParam(req.body, "pulse").toInt();
    user.trim();

    // A colon would break HTTP Basic auth, which splits user and password at the first one
    if (user.isEmpty() || user.length() > 32 || user.indexOf(':') >= 0) {
        sendSettingsPage(client, "Username must be 1-32 characters and must not contain ':'.", true);
        return;
    }
    if (password.length() > 64) {
        sendSettingsPage(client, "Password must be at most 64 characters.", true);
        return;
    }
    if (pulse < 100 || pulse > 30000) {
        sendSettingsPage(client, "Relay pulse must be between 100 and 30000 ms.", true);
        return;
    }

    bool credentialsChanged = user != settings.user || (!password.isEmpty() && password != settings.password);
    settings.user = user;
    if (!password.isEmpty())
        settings.password = password;
    settings.relayPulseMs = pulse;
    settingsSave();
    Serial.println("Settings saved");

    if (credentialsChanged) {
        // The browser still holds the old credentials and will ask for the new ones
        sendResponse(client, 303, "See Other", "text/plain", "Saved\n", "Location: /\r\n");
        client.flush();
        client.stop();
        applyCredentials();
        return;
    }
    sendSettingsPage(client, "Settings saved.", false);
}

// ---- Favorites and schedules ---------------------------------------------------------

static void sendStatus(WiFiClient &client, int code)
{
    switch (code) {
    case 200: sendResponse(client, 200, "OK", "text/plain", ""); break;
    case 204: sendResponse(client, 204, "No Content", "text/plain", ""); break;
    case 400: sendResponse(client, 400, "Bad Request", "text/plain", "Parameter missing or invalid\n"); break;
    case 507: sendResponse(client, 507, "Insufficient Storage", "text/plain", "Size limit exceeded\n"); break;
    default: sendResponse(client, 500, "Internal Server Error", "text/plain", "Internal error\n"); break;
    }
}

static void handleFavorites(WiFiClient &client, const Request &req)
{
    String action = queryParam(req.query, "action");
    String type = queryParam(req.query, "type");
    String id = queryParam(req.query, "id");
    if (action.isEmpty()) {
        sendJson(client, favoritesJson());
    } else if (action == "save") {
        int savedId = -1;
        int code = favoritesSave(type, queryParam(req.query, "title"), queryParam(req.query, "value"), id, savedId);
        if (code == 200) {
            // The id of the saved favorite is reported in a response header
            String header = "favoriteid: " + String(savedId) + "\r\n";
            sendResponse(client, 200, "OK", "text/plain", "", header.c_str());
        } else {
            sendStatus(client, code);
        }
    } else if (action == "remove") {
        sendStatus(client, favoritesRemove(type, id));
    } else {
        sendStatus(client, 400);
    }
}

static void handleSchedule(WiFiClient &client, const Request &req)
{
    String action = queryParam(req.query, "action");
    if (req.method == "POST") {
        // DoorBird requires a matching Content-Length, the body must be complete
        if (req.contentLength <= 0 || req.contentLength > MAX_BODY_SIZE || (long)req.body.length() != req.contentLength)
            sendStatus(client, 400);
        else
            sendStatus(client, scheduleSave(req.body));
    } else if (action == "remove") {
        sendStatus(client, scheduleRemove(queryParam(req.query, "input"), queryParam(req.query, "param")));
    } else if (action.isEmpty()) {
        String json;
        int code = schedulesJson(queryParam(req.query, "input"), json);
        if (code == 200)
            sendJson(client, json);
        else
            sendStatus(client, code);
    } else {
        sendStatus(client, 400);
    }
}

// Returns true if the client connection was handed over and must not be closed.
static bool handleRequest(WiFiClient &client, const Request &req)
{
    if (req.path == "/" || req.path == "/settings") {
        if (req.authorization != expectedAuth) {
            sendUnauthorized(client);
        } else if (req.path == "/settings" && req.method == "POST") {
            handleSettingsPost(client, req);
        } else {
            sendSettingsPage(client, String(), false);
        }
        return false;
    }

    if (!req.path.startsWith("/bha-api/")) {
        sendResponse(client, 404, "Not Found", "text/plain", "Not found\n");
        return false;
    }

    if (!isAuthorized(req)) {
        sendUnauthorized(client);
        return false;
    }

    const String cgi = req.path.substring(9);
    if (cgi == "getsession.cgi") {
        bool invalidate = false;
        String oldSession = queryParam(req.query, "invalidate", &invalidate);
        if (invalidate) {
            if (oldSession == sessionId)
                sessionId = randomHex(32);
            sendJson(client, "{\"BHA\":{\"RETURNCODE\":\"1\",\"SESSIONID\":\"\"}}");
        } else {
            // The key is only used for UDP broadcasts, which OpenBird does not send
            sendJson(client, "{\"BHA\":{\"RETURNCODE\":\"1\",\"SESSIONID\":\"" + sessionId +
                                 "\",\"NOTIFICATION_ENCRYPTION_KEY\":\"" + notificationKey + "\"}}");
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
        handleFavorites(client, req);
    } else if (cgi == "schedule.cgi") {
        handleSchedule(client, req);
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
    WiFiClient client = httpAccept();
    if (!client)
        return;
    Request req;
    if (!readRequest(client, req)) {
        client.stop();
        return;
    }
    Serial.printf("%s %s %s\n", peerName(client).c_str(), req.method.c_str(), req.path.c_str());
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
    if (!MDNS.begin(mdnsHost.c_str())) {
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
    Serial.printf("mDNS: %s.local\n", mdnsHost.c_str());
}

// Time for schedule windows (UTC), started once an IP address is assigned
static void startNtp()
{
    if (ntpStarted)
        return;
    configTime(0, 0, NTP_SERVER);
    ntpStarted = true;
}

static void startOta()
{
    if (otaStarted)
        return;
    ArduinoOTA.setHostname(mdnsHost.c_str());
    ArduinoOTA.setPassword(settings.password.c_str());
    ArduinoOTA.setMdnsEnabled(false); // mDNS is managed in startMdns()
    ArduinoOTA.onStart([]() {
        Serial.println("OTA update started");
        // Flashing blocks loop() for longer than the watchdog timeout
        esp_task_wdt_delete(NULL);
        monitorDisconnectAll();
        outputWrite(relays[0], false);
        outputWrite(relays[1], false);
    });
    ArduinoOTA.onEnd([]() { Serial.println("OTA update done, restarting"); });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA update failed: %u\n", error);
        esp_task_wdt_add(NULL);
    });
    ArduinoOTA.begin();
    otaStarted = true;
}

static void onNetworkEvent(WiFiEvent_t event)
{
    switch (event) {
    case ARDUINO_EVENT_ETH_START: {
        String host = "openbird-" + macAddress.substring(6);
        host.toLowerCase();
        ETH.setHostname(host.c_str()); // DHCP host name
        Serial.println("ETH started");
        break;
    }
    case ARDUINO_EVENT_ETH_CONNECTED:
        Serial.println("ETH link up");
        // Without an IPv6 address mDNS leaves AAAA queries unanswered, and clients
        // that ask for both (browsers, curl) wait for the timeout before using IPv4
        ETH.enableIpV6();
        break;
    case ARDUINO_EVENT_ETH_GOT_IP6:
        Serial.printf("ETH IPv6 %s\n", ETH.localIPv6().toString().c_str());
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

// ---- Factory reset ------------------------------------------------------------------

#if RESET_PIN >= 0 && RESET_PIN != DOORBELL_PIN
static void resetButtonLoop()
{
    static uint32_t pressedSince = 0;
    if (digitalRead(RESET_PIN) == LOW) {
        if (!pressedSince) {
            pressedSince = millis() | 1;
        } else if (millis() - pressedSince >= RESET_HOLD_MS) {
            Serial.println("Factory reset");
            settingsReset();
            notifyReset();
            delay(100);
            ESP.restart();
        }
    } else {
        pressedSince = 0;
    }
}
#else
static void resetButtonLoop() {}
#endif

// ---- Main ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    Serial.println("\nOpenBird starting");

    esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    outputInit(relays[0]);
    outputInit(relays[1]);
    outputInit(light);
    inputInit(doorbell, DOORBELL_PULLUP);
    inputInit(motion, false);
#if RESET_PIN >= 0 && RESET_PIN != DOORBELL_PIN
    pinMode(RESET_PIN, INPUT); // BUT1 has an external pull-up
#endif

    settingsLoad();
    formToken = randomHex(16);
    notificationKey = randomHex(32);
    notifyBegin([](int relay) {
        if (relay >= 1 && relay <= 2 && relays[relay - 1].pin >= 0)
            outputPulse(relays[relay - 1], settings.relayPulseMs);
    });

    // Read the MAC before ETH.begin(), the ETH_START event already needs it
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    char macStr[13];
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    macAddress = macStr;
    mdnsHost = "bha-" + macAddress;
    mdnsHost.toLowerCase();

    applyCredentials();

    WiFi.onEvent(onNetworkEvent);
    // LAN8720 pin mapping comes from the esp32-poe-iso board variant
    if (!ETH.begin())
        Serial.println("ETH init failed");

    if (!httpListen(80))
        Serial.println("HTTP server start failed");
}

void loop()
{
    esp_task_wdt_reset();

    if (inputLoop(doorbell)) {
        Serial.printf("Doorbell %s\n", doorbell.state ? "pressed" : "released");
        monitorBroadcast(doorbell);
        if (doorbell.state)
            notifyTrigger("doorbell");
    }
    if (inputLoop(motion)) {
        Serial.printf("Motion %s\n", motion.state ? "detected" : "cleared");
        monitorBroadcast(motion);
        if (motion.state)
            notifyTrigger("motion");
    }
    resetButtonLoop();

    outputLoop(relays[0]);
    outputLoop(relays[1]);
    outputLoop(light);

    if (ethConnected) {
        startNtp();
        startOta();
        ArduinoOTA.handle();
        httpLoop();
    }
    monitorLoop();

    delay(1);
}
