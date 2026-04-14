#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <time.h>
#include "config.h"

// ── State ──
AsyncWebServer server(80);
bool sprayActive = false;
unsigned long sprayStartMs = 0;
unsigned long sprayDurationMs = DEFAULT_SPRAY_SECONDS * 1000UL;
int configuredSpraySeconds = DEFAULT_SPRAY_SECONDS;

// ── Device identity ──
String deviceId;       // last 4 hex of MAC, e.g. "a1b2"
String hostname;       // "mosquitto-death-a1b2"
String friendlyName;   // user-editable, e.g. "Back Patio"

// ── PIN protection ──
String pin;            // 4-digit PIN, empty = no protection

// ── Schedule ──
// Each schedule entry: enabled, days bitmask (bit0=Sun..bit6=Sat), hour, minute
struct ScheduleEntry {
    bool enabled;
    uint8_t days;   // bitmask: bit0=Sun, bit1=Mon, ..., bit6=Sat
    uint8_t hour;
    uint8_t minute;
};
#define MAX_SCHEDULES 8
ScheduleEntry schedules[MAX_SCHEDULES];
int scheduleCount = 0;
bool lastScheduleCheck[MAX_SCHEDULES] = {};  // debounce: did we fire this minute already?

// ── Button debounce ──
bool lastButtonState = LOW;
unsigned long lastDebounceMs = 0;
#define DEBOUNCE_MS 50

// ── Peer discovery ──
WiFiUDP udp;
#define BEACON_PORT 5555
#define BEACON_INTERVAL_MS 30000
#define PEER_TIMEOUT_MS 90000
#define MAX_PEERS 16
IPAddress beaconMulticast(239, 77, 68, 1);  // "MD" = mosquitto death

struct Peer {
    String deviceId;
    String hostname;
    String friendlyName;
    String ip;
    bool active;       // spray active
    unsigned long lastSeen;
};
Peer peers[MAX_PEERS];
int peerCount = 0;
unsigned long lastBeaconMs = 0;

// ── Forward declarations ──
void startSpray();
void stopSpray();
void loadConfig();
void saveConfig();
void setupRoutes();
String getStatusJson();
String getDevicesJson();
void checkSchedules();
void checkButton();
void sendBeacon();
void receiveBeacons();
bool checkPin(AsyncWebServerRequest *req, const JsonDocument &doc);
bool checkPinHeader(AsyncWebServerRequest *req);

// ════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════

void setup() {
    Serial.begin(115200);

    // Derive unique device ID from MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char idBuf[5];
    snprintf(idBuf, sizeof(idBuf), "%02x%02x", mac[4], mac[5]);
    deviceId = String(idBuf);
    hostname = String(MDNS_HOSTNAME) + "-" + deviceId;

    Serial.printf("\n[mosquitto-death] Device %s starting...\n", hostname.c_str());

    // GPIO
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLDOWN);
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

    // Filesystem
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed");
    }

    loadConfig();

    // WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] Connecting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        digitalWrite(LED_PIN, HIGH);
    } else {
        Serial.println("\n[WiFi] Connection failed — restarting in 10s");
        delay(10000);
        ESP.restart();
    }

    // NTP
    configTime(TZ_OFFSET, TZ_DST, NTP_SERVER);
    Serial.println("[NTP] Time sync requested");

    // mDNS — unique per device
    if (MDNS.begin(hostname.c_str())) {
        Serial.printf("[mDNS] http://%s.local\n", hostname.c_str());
        MDNS.addService("http", "tcp", 80);
    }

    // UDP peer discovery
    udp.beginMulticast(beaconMulticast, BEACON_PORT);
    sendBeacon();  // announce immediately
    Serial.println("[Discovery] Beacon started");

    // Web server
    setupRoutes();
    server.begin();
    Serial.println("[HTTP] Server started on port 80");
}

// ════════════════════════════════════════════
// Loop
// ════════════════════════════════════════════

void loop() {
    // Auto-stop spray after duration
    if (sprayActive && (millis() - sprayStartMs >= sprayDurationMs)) {
        stopSpray();
    }

    checkButton();
    checkSchedules();
    receiveBeacons();

    // Send beacon periodically
    if (millis() - lastBeaconMs >= BEACON_INTERVAL_MS) {
        sendBeacon();
    }

    delay(100);
}

// ════════════════════════════════════════════
// Spray control
// ════════════════════════════════════════════

void startSpray() {
    if (sprayActive) return;
    sprayActive = true;
    sprayStartMs = millis();
    sprayDurationMs = configuredSpraySeconds * 1000UL;
    digitalWrite(RELAY_PIN, HIGH);
    Serial.printf("[Spray] ON for %d seconds\n", configuredSpraySeconds);
}

void stopSpray() {
    sprayActive = false;
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("[Spray] OFF");
}

// ════════════════════════════════════════════
// Button
// ════════════════════════════════════════════

void checkButton() {
    bool reading = digitalRead(BUTTON_PIN);
    if (reading != lastButtonState) {
        lastDebounceMs = millis();
    }
    if ((millis() - lastDebounceMs) > DEBOUNCE_MS) {
        if (reading == HIGH && lastButtonState == LOW) {
            // Button pressed — toggle spray
            if (sprayActive) {
                stopSpray();
            } else {
                startSpray();
            }
        }
    }
    lastButtonState = reading;
}

// ════════════════════════════════════════════
// Schedule
// ════════════════════════════════════════════

void checkSchedules() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return;  // no time yet

    for (int i = 0; i < scheduleCount; i++) {
        if (!schedules[i].enabled) {
            lastScheduleCheck[i] = false;
            continue;
        }

        bool dayMatch = schedules[i].days & (1 << timeinfo.tm_wday);
        bool timeMatch = (timeinfo.tm_hour == schedules[i].hour &&
                          timeinfo.tm_min == schedules[i].minute);

        if (dayMatch && timeMatch) {
            if (!lastScheduleCheck[i]) {
                lastScheduleCheck[i] = true;
                Serial.printf("[Schedule] Firing entry %d\n", i);
                startSpray();
            }
        } else {
            lastScheduleCheck[i] = false;
        }
    }
}

// ════════════════════════════════════════════
// Config persistence
// ════════════════════════════════════════════

void loadConfig() {
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
        Serial.println("[Config] No config file, using defaults");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[Config] Parse error: %s\n", err.c_str());
        return;
    }

    configuredSpraySeconds = doc["spray_seconds"] | DEFAULT_SPRAY_SECONDS;
    if (configuredSpraySeconds < 1) configuredSpraySeconds = 1;
    if (configuredSpraySeconds > MAX_SPRAY_SECONDS) configuredSpraySeconds = MAX_SPRAY_SECONDS;

    friendlyName = doc["friendly_name"] | "";
    pin = doc["pin"] | "";

    JsonArray arr = doc["schedules"].as<JsonArray>();
    scheduleCount = 0;
    for (JsonObject obj : arr) {
        if (scheduleCount >= MAX_SCHEDULES) break;
        schedules[scheduleCount].enabled = obj["enabled"] | true;
        schedules[scheduleCount].days = obj["days"] | 0;
        schedules[scheduleCount].hour = obj["hour"] | 0;
        schedules[scheduleCount].minute = obj["minute"] | 0;
        scheduleCount++;
    }

    Serial.printf("[Config] Loaded: %ds spray, %d schedules\n",
                  configuredSpraySeconds, scheduleCount);
}

void saveConfig() {
    JsonDocument doc;
    doc["spray_seconds"] = configuredSpraySeconds;
    doc["friendly_name"] = friendlyName;
    doc["pin"] = pin;

    JsonArray arr = doc["schedules"].to<JsonArray>();
    for (int i = 0; i < scheduleCount; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["enabled"] = schedules[i].enabled;
        obj["days"] = schedules[i].days;
        obj["hour"] = schedules[i].hour;
        obj["minute"] = schedules[i].minute;
    }

    File f = LittleFS.open("/config.json", "w");
    if (!f) {
        Serial.println("[Config] Failed to open for writing");
        return;
    }
    serializeJson(doc, f);
    f.close();
    Serial.println("[Config] Saved");
}

// ════════════════════════════════════════════
// JSON status
// ════════════════════════════════════════════

String getStatusJson() {
    JsonDocument doc;
    doc["active"] = sprayActive;
    doc["spray_seconds"] = configuredSpraySeconds;

    if (sprayActive) {
        unsigned long elapsed = (millis() - sprayStartMs) / 1000;
        unsigned long remaining = configuredSpraySeconds - min((unsigned long)configuredSpraySeconds, elapsed);
        doc["remaining"] = remaining;
    } else {
        doc["remaining"] = 0;
    }

    // Current time
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        doc["time"] = buf;
        doc["weekday"] = timeinfo.tm_wday;
    }

    doc["device_id"] = deviceId;
    doc["hostname"] = hostname;
    doc["friendly_name"] = friendlyName;
    doc["pin_enabled"] = pin.length() > 0;
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime"] = millis() / 1000;
    doc["wifi_rssi"] = WiFi.RSSI();

    JsonArray arr = doc["schedules"].to<JsonArray>();
    for (int i = 0; i < scheduleCount; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["enabled"] = schedules[i].enabled;
        obj["days"] = schedules[i].days;
        obj["hour"] = schedules[i].hour;
        obj["minute"] = schedules[i].minute;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

// ════════════════════════════════════════════
// Peer discovery
// ════════════════════════════════════════════

void sendBeacon() {
    JsonDocument doc;
    doc["id"] = deviceId;
    doc["host"] = hostname;
    doc["name"] = friendlyName;
    doc["ip"] = WiFi.localIP().toString();
    doc["active"] = sprayActive;

    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));

    udp.beginMulticastPacket();
    udp.write((uint8_t*)buf, len);
    udp.endPacket();

    lastBeaconMs = millis();
}

void receiveBeacons() {
    int packetSize = udp.parsePacket();
    if (packetSize == 0) return;

    char buf[256];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len <= 0) return;
    buf[len] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf)) return;

    String peerId = doc["id"] | "";
    if (peerId.isEmpty() || peerId == deviceId) return;  // ignore self

    // Update existing or add new
    int slot = -1;
    for (int i = 0; i < peerCount; i++) {
        if (peers[i].deviceId == peerId) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (peerCount >= MAX_PEERS) return;
        slot = peerCount++;
    }

    peers[slot].deviceId = peerId;
    peers[slot].hostname = doc["host"] | "";
    peers[slot].friendlyName = doc["name"] | "";
    peers[slot].ip = doc["ip"] | "";
    peers[slot].active = doc["active"] | false;
    peers[slot].lastSeen = millis();
}

String getDevicesJson() {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    unsigned long now = millis();

    // Add self first
    JsonObject self = arr.add<JsonObject>();
    self["id"] = deviceId;
    self["hostname"] = hostname;
    self["name"] = friendlyName;
    self["ip"] = WiFi.localIP().toString();
    self["active"] = sprayActive;
    self["self"] = true;

    // Add peers (skip stale ones)
    for (int i = 0; i < peerCount; i++) {
        if (now - peers[i].lastSeen > PEER_TIMEOUT_MS) continue;
        JsonObject p = arr.add<JsonObject>();
        p["id"] = peers[i].deviceId;
        p["hostname"] = peers[i].hostname;
        p["name"] = peers[i].friendlyName;
        p["ip"] = peers[i].ip;
        p["active"] = peers[i].active;
        p["self"] = false;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

// ════════════════════════════════════════════
// PIN authentication
// ════════════════════════════════════════════

// Check PIN from X-Pin header (for spray start/stop which have no body)
bool checkPinHeader(AsyncWebServerRequest *req) {
    if (pin.isEmpty()) return true;  // no PIN set
    if (req->hasHeader("X-Pin") && req->header("X-Pin") == pin) return true;
    req->send(403, "application/json", "{\"error\":\"pin_required\"}");
    return false;
}

// Check PIN from JSON body "pin" field
bool checkPin(AsyncWebServerRequest *req, const JsonDocument &doc) {
    if (pin.isEmpty()) return true;  // no PIN set
    String submitted = doc["pin"] | "";
    if (submitted == pin) return true;
    req->send(403, "application/json", "{\"error\":\"pin_required\"}");
    return false;
}

// ════════════════════════════════════════════
// Web routes
// ════════════════════════════════════════════

void setupRoutes() {
    // Serve the web UI and static assets
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(LittleFS, "/index.html", "text/html");
    });
    server.on("/apple-touch-icon.png", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(LittleFS, "/apple-touch-icon.png", "image/png");
    });
    server.on("/icon-512.png", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(LittleFS, "/icon-512.png", "image/png");
    });

    // API: status
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", getStatusJson());
    });

    // API: all devices on network
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", getDevicesJson());
    });

    // API: start spray (PIN protected)
    server.on("/api/spray/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!checkPinHeader(req)) return;
        startSpray();
        req->send(200, "application/json", getStatusJson());
    });

    // API: stop spray (PIN protected)
    server.on("/api/spray/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!checkPinHeader(req)) return;
        stopSpray();
        req->send(200, "application/json", getStatusJson());
    });

    // API: set spray duration
    server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *req) {
        // Handled by body handler below
    },
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        if (doc["spray_seconds"].is<int>()) {
            int s = doc["spray_seconds"];
            if (s >= 1 && s <= MAX_SPRAY_SECONDS) {
                configuredSpraySeconds = s;
            }
        }
        if (doc["friendly_name"].is<const char*>()) {
            friendlyName = doc["friendly_name"].as<String>();
        }
        // PIN change: send "new_pin":"1234" to set, "new_pin":"" to clear
        if (doc["new_pin"].is<const char*>()) {
            String newPin = doc["new_pin"].as<String>();
            if (newPin.isEmpty() || newPin.length() == 4) {
                pin = newPin;
            }
        }
        saveConfig();
        req->send(200, "application/json", getStatusJson());
    });

    // API: update schedules
    server.on("/api/schedules", HTTP_POST, [](AsyncWebServerRequest *req) {
        // Handled by body handler
    },
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;

        JsonArray arr = doc["schedules"].as<JsonArray>();
        scheduleCount = 0;
        for (JsonObject obj : arr) {
            if (scheduleCount >= MAX_SCHEDULES) break;
            schedules[scheduleCount].enabled = obj["enabled"] | true;
            schedules[scheduleCount].days = obj["days"] | 0;
            schedules[scheduleCount].hour = obj["hour"] | 0;
            schedules[scheduleCount].minute = obj["minute"] | 0;
            scheduleCount++;
        }

        saveConfig();
        req->send(200, "application/json", getStatusJson());
    });

    // API: delete a schedule
    server.on("/api/schedules/delete", HTTP_POST, [](AsyncWebServerRequest *req) {
        // Handled by body handler
    },
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        int idx = doc["index"] | -1;
        if (idx >= 0 && idx < scheduleCount) {
            for (int i = idx; i < scheduleCount - 1; i++) {
                schedules[i] = schedules[i + 1];
            }
            scheduleCount--;
            saveConfig();
        }
        req->send(200, "application/json", getStatusJson());
    });
}
