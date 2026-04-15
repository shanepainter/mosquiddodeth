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

// ── Device identity ──
String deviceId;
String hostname;
String friendlyName;

// ── PIN protection ──
String pin;

// ── Zone ──
struct ScheduleEntry {
    bool enabled;
    uint8_t days;   // bitmask: bit0=Sun, bit1=Mon, ..., bit6=Sat
    uint8_t hour;
    uint8_t minute;
};

#define MAX_SCHEDULES_PER_ZONE 4

struct Zone {
    int relayPin;
    String name;
    int spraySeconds;
    bool active;
    unsigned long startMs;
    ScheduleEntry schedules[MAX_SCHEDULES_PER_ZONE];
    int scheduleCount;
    bool lastScheduleFired[MAX_SCHEDULES_PER_ZONE];
};

Zone zones[NUM_ZONES];
int activeZone = -1;  // index of currently running zone, -1 = none

// ── Queue ──
#define MAX_QUEUE 16
int zoneQueue[MAX_QUEUE];
int queueHead = 0;
int queueTail = 0;

// ── Button ──
bool lastButtonState = LOW;
unsigned long lastDebounceMs = 0;
#define DEBOUNCE_MS 50
int buttonZoneIndex = 0;  // which zone the button activates next

// ── Peer discovery ──
WiFiUDP udp;
#define BEACON_PORT 5555
#define BEACON_INTERVAL_MS 30000
#define PEER_TIMEOUT_MS 90000
#define MAX_PEERS 16
IPAddress beaconMulticast(239, 77, 68, 1);

struct Peer {
    String deviceId;
    String hostname;
    String friendlyName;
    String ip;
    bool active;
    unsigned long lastSeen;
};
Peer peers[MAX_PEERS];
int peerCount = 0;
unsigned long lastBeaconMs = 0;

// ── Forward declarations ──
void startZone(int z);
void stopZone(int z);
void stopAll();
void enqueueZone(int z);
void processQueue();
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
// Queue operations
// ════════════════════════════════════════════

int queueSize() {
    return (queueTail - queueHead + MAX_QUEUE) % MAX_QUEUE;
}

void enqueueZone(int z) {
    // Don't enqueue if already active or already in queue
    if (zones[z].active) return;
    for (int i = queueHead; i != queueTail; i = (i + 1) % MAX_QUEUE) {
        if (zoneQueue[i] == z) return;
    }
    if (queueSize() >= MAX_QUEUE - 1) return;
    zoneQueue[queueTail] = z;
    queueTail = (queueTail + 1) % MAX_QUEUE;
}

int dequeueZone() {
    if (queueHead == queueTail) return -1;
    int z = zoneQueue[queueHead];
    queueHead = (queueHead + 1) % MAX_QUEUE;
    return z;
}

void processQueue() {
    if (activeZone >= 0) return;  // something is running
    int z = dequeueZone();
    if (z >= 0) startZone(z);
}

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

    Serial.printf("\n[MosquittoDeth] Device %s starting...\n", hostname.c_str());

    // GPIO — init all zone relay pins
    for (int i = 0; i < NUM_ZONES; i++) {
        zones[i].relayPin = ZONE_PINS[i];
        zones[i].name = ZONE_DEFAULT_NAMES[i];
        zones[i].spraySeconds = DEFAULT_SPRAY_SECONDS;
        zones[i].active = false;
        zones[i].scheduleCount = 0;
        pinMode(ZONE_PINS[i], OUTPUT);
        digitalWrite(ZONE_PINS[i], LOW);
    }
    pinMode(LED_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLDOWN);
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

    // mDNS
    if (MDNS.begin(hostname.c_str())) {
        Serial.printf("[mDNS] http://%s.local\n", hostname.c_str());
        MDNS.addService("http", "tcp", 80);
    }

    // UDP peer discovery
    udp.beginMulticast(beaconMulticast, BEACON_PORT);
    sendBeacon();
    Serial.println("[Discovery] Beacon started");

    // Web server
    setupRoutes();
    server.begin();
    Serial.printf("[HTTP] Server started — %d zones configured\n", NUM_ZONES);
}

// ════════════════════════════════════════════
// Loop
// ════════════════════════════════════════════

void loop() {
    // Auto-stop active zone after its duration
    if (activeZone >= 0) {
        Zone &z = zones[activeZone];
        if (z.active && (millis() - z.startMs >= (unsigned long)z.spraySeconds * 1000UL)) {
            stopZone(activeZone);
        }
    }

    processQueue();
    checkButton();
    checkSchedules();
    receiveBeacons();

    if (millis() - lastBeaconMs >= BEACON_INTERVAL_MS) {
        sendBeacon();
    }

    delay(100);
}

// ════════════════════════════════════════════
// Zone control
// ════════════════════════════════════════════

void startZone(int z) {
    if (z < 0 || z >= NUM_ZONES) return;
    if (zones[z].active) return;

    // If another zone is running, queue this one instead
    if (activeZone >= 0 && activeZone != z) {
        enqueueZone(z);
        Serial.printf("[Zone %d] Queued (zone %d still running)\n", z, activeZone);
        return;
    }

    zones[z].active = true;
    zones[z].startMs = millis();
    digitalWrite(zones[z].relayPin, HIGH);
    activeZone = z;
    Serial.printf("[Zone %d] %s ON for %ds\n", z, zones[z].name.c_str(), zones[z].spraySeconds);
}

void stopZone(int z) {
    if (z < 0 || z >= NUM_ZONES) return;
    zones[z].active = false;
    digitalWrite(zones[z].relayPin, LOW);
    if (activeZone == z) activeZone = -1;
    Serial.printf("[Zone %d] %s OFF\n", z, zones[z].name.c_str());
}

void stopAll() {
    for (int i = 0; i < NUM_ZONES; i++) {
        zones[i].active = false;
        digitalWrite(zones[i].relayPin, LOW);
    }
    activeZone = -1;
    queueHead = queueTail = 0;  // clear queue
    Serial.println("[Zones] All stopped, queue cleared");
}

// ════════════════════════════════════════════
// Button — cycles through zones on each press
// ════════════════════════════════════════════

void checkButton() {
    bool reading = digitalRead(BUTTON_PIN);
    if (reading != lastButtonState) {
        lastDebounceMs = millis();
    }
    if ((millis() - lastDebounceMs) > DEBOUNCE_MS) {
        if (reading == HIGH && lastButtonState == LOW) {
            if (activeZone >= 0) {
                // Something running — stop everything
                stopAll();
            } else {
                // Start the next zone in rotation
                startZone(buttonZoneIndex);
                buttonZoneIndex = (buttonZoneIndex + 1) % NUM_ZONES;
            }
        }
    }
    lastButtonState = reading;
}

// ════════════════════════════════════════════
// Schedules — per zone
// ════════════════════════════════════════════

void checkSchedules() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return;

    for (int z = 0; z < NUM_ZONES; z++) {
        for (int s = 0; s < zones[z].scheduleCount; s++) {
            ScheduleEntry &sched = zones[z].schedules[s];
            if (!sched.enabled) {
                zones[z].lastScheduleFired[s] = false;
                continue;
            }

            bool dayMatch = sched.days & (1 << timeinfo.tm_wday);
            bool timeMatch = (timeinfo.tm_hour == sched.hour &&
                              timeinfo.tm_min == sched.minute);

            if (dayMatch && timeMatch) {
                if (!zones[z].lastScheduleFired[s]) {
                    zones[z].lastScheduleFired[s] = true;
                    Serial.printf("[Schedule] Zone %d, entry %d firing\n", z, s);
                    if (activeZone >= 0) {
                        enqueueZone(z);
                    } else {
                        startZone(z);
                    }
                }
            } else {
                zones[z].lastScheduleFired[s] = false;
            }
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

    friendlyName = doc["friendly_name"] | "";
    pin = doc["pin"] | "";

    JsonArray zarr = doc["zones"].as<JsonArray>();
    int i = 0;
    for (JsonObject zobj : zarr) {
        if (i >= NUM_ZONES) break;
        zones[i].name = zobj["name"] | ZONE_DEFAULT_NAMES[i];
        zones[i].spraySeconds = zobj["seconds"] | DEFAULT_SPRAY_SECONDS;
        if (zones[i].spraySeconds < 1) zones[i].spraySeconds = 1;
        if (zones[i].spraySeconds > MAX_SPRAY_SECONDS) zones[i].spraySeconds = MAX_SPRAY_SECONDS;

        JsonArray sarr = zobj["schedules"].as<JsonArray>();
        zones[i].scheduleCount = 0;
        for (JsonObject sobj : sarr) {
            if (zones[i].scheduleCount >= MAX_SCHEDULES_PER_ZONE) break;
            int s = zones[i].scheduleCount;
            zones[i].schedules[s].enabled = sobj["enabled"] | true;
            zones[i].schedules[s].days = sobj["days"] | 0;
            zones[i].schedules[s].hour = sobj["hour"] | 0;
            zones[i].schedules[s].minute = sobj["minute"] | 0;
            zones[i].scheduleCount++;
        }
        i++;
    }

    Serial.printf("[Config] Loaded: %d zones\n", NUM_ZONES);
}

void saveConfig() {
    JsonDocument doc;
    doc["friendly_name"] = friendlyName;
    doc["pin"] = pin;

    JsonArray zarr = doc["zones"].to<JsonArray>();
    for (int i = 0; i < NUM_ZONES; i++) {
        JsonObject zobj = zarr.add<JsonObject>();
        zobj["name"] = zones[i].name;
        zobj["seconds"] = zones[i].spraySeconds;

        JsonArray sarr = zobj["schedules"].to<JsonArray>();
        for (int s = 0; s < zones[i].scheduleCount; s++) {
            JsonObject sobj = sarr.add<JsonObject>();
            sobj["enabled"] = zones[i].schedules[s].enabled;
            sobj["days"] = zones[i].schedules[s].days;
            sobj["hour"] = zones[i].schedules[s].hour;
            sobj["minute"] = zones[i].schedules[s].minute;
        }
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
    doc["device_id"] = deviceId;
    doc["hostname"] = hostname;
    doc["friendly_name"] = friendlyName;
    doc["pin_enabled"] = pin.length() > 0;
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime"] = millis() / 1000;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["active_zone"] = activeZone;
    doc["queue_size"] = queueSize();

    // Current time
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        doc["time"] = buf;
        doc["weekday"] = timeinfo.tm_wday;
    }

    JsonArray zarr = doc["zones"].to<JsonArray>();
    for (int i = 0; i < NUM_ZONES; i++) {
        JsonObject zobj = zarr.add<JsonObject>();
        zobj["name"] = zones[i].name;
        zobj["seconds"] = zones[i].spraySeconds;
        zobj["active"] = zones[i].active;
        zobj["pin"] = zones[i].relayPin;

        if (zones[i].active) {
            unsigned long elapsed = (millis() - zones[i].startMs) / 1000;
            unsigned long remaining = zones[i].spraySeconds - min((unsigned long)zones[i].spraySeconds, elapsed);
            zobj["remaining"] = remaining;
        } else {
            zobj["remaining"] = 0;
        }

        // Check if queued
        bool queued = false;
        for (int q = queueHead; q != queueTail; q = (q + 1) % MAX_QUEUE) {
            if (zoneQueue[q] == i) { queued = true; break; }
        }
        zobj["queued"] = queued;

        JsonArray sarr = zobj["schedules"].to<JsonArray>();
        for (int s = 0; s < zones[i].scheduleCount; s++) {
            JsonObject sobj = sarr.add<JsonObject>();
            sobj["enabled"] = zones[i].schedules[s].enabled;
            sobj["days"] = zones[i].schedules[s].days;
            sobj["hour"] = zones[i].schedules[s].hour;
            sobj["minute"] = zones[i].schedules[s].minute;
        }
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
    doc["active"] = (activeZone >= 0);

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
    if (peerId.isEmpty() || peerId == deviceId) return;

    int slot = -1;
    for (int i = 0; i < peerCount; i++) {
        if (peers[i].deviceId == peerId) { slot = i; break; }
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

    JsonObject self = arr.add<JsonObject>();
    self["id"] = deviceId;
    self["hostname"] = hostname;
    self["name"] = friendlyName;
    self["ip"] = WiFi.localIP().toString();
    self["active"] = (activeZone >= 0);
    self["self"] = true;

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

bool checkPinHeader(AsyncWebServerRequest *req) {
    if (pin.isEmpty()) return true;
    if (req->hasHeader("X-Pin") && req->header("X-Pin") == pin) return true;
    req->send(403, "application/json", "{\"error\":\"pin_required\"}");
    return false;
}

bool checkPin(AsyncWebServerRequest *req, const JsonDocument &doc) {
    if (pin.isEmpty()) return true;
    String submitted = doc["pin"] | "";
    if (submitted == pin) return true;
    req->send(403, "application/json", "{\"error\":\"pin_required\"}");
    return false;
}

// ════════════════════════════════════════════
// Web routes
// ════════════════════════════════════════════

void setupRoutes() {
    // Static assets
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(LittleFS, "/index.html", "text/html");
    });
    server.on("/apple-touch-icon.png", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(LittleFS, "/apple-touch-icon.png", "image/png");
    });
    server.on("/icon-512.png", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(LittleFS, "/icon-512.png", "image/png");
    });
    server.on("/logo.png", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(LittleFS, "/logo.png", "image/png");
    });

    // API: status (read-only, no PIN)
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", getStatusJson());
    });

    // API: all devices (read-only, no PIN)
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", getDevicesJson());
    });

    // API: start a zone (PIN protected)
    // POST /api/zone/start with body {"zone": 0}
    server.on("/api/zone/start", HTTP_POST, [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        int z = doc["zone"] | -1;
        if (z < 0 || z >= NUM_ZONES) {
            req->send(400, "application/json", "{\"error\":\"invalid zone\"}");
            return;
        }
        if (activeZone >= 0) {
            enqueueZone(z);
        } else {
            startZone(z);
        }
        req->send(200, "application/json", getStatusJson());
    });

    // API: stop a zone or all (PIN protected)
    // POST /api/zone/stop with body {"zone": 0} or {"all": true}
    server.on("/api/zone/stop", HTTP_POST, [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        if (doc["all"] | false) {
            stopAll();
        } else {
            int z = doc["zone"] | -1;
            if (z >= 0 && z < NUM_ZONES) stopZone(z);
        }
        req->send(200, "application/json", getStatusJson());
    });

    // API: run all zones sequentially (PIN protected)
    server.on("/api/zone/run-all", HTTP_POST, [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        for (int i = 0; i < NUM_ZONES; i++) {
            if (activeZone < 0 && !zones[i].active) {
                startZone(i);
            } else {
                enqueueZone(i);
            }
        }
        req->send(200, "application/json", getStatusJson());
    });

    // API: update device config (PIN protected)
    server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        if (doc["friendly_name"].is<const char*>()) {
            friendlyName = doc["friendly_name"].as<String>();
        }
        if (doc["new_pin"].is<const char*>()) {
            String newPin = doc["new_pin"].as<String>();
            if (newPin.isEmpty() || newPin.length() == 4) {
                pin = newPin;
            }
        }
        saveConfig();
        req->send(200, "application/json", getStatusJson());
    });

    // API: update a single zone's config (PIN protected)
    // POST /api/zone/config with body {"zone": 0, "name": "Front Yard", "seconds": 120}
    server.on("/api/zone/config", HTTP_POST, [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        int z = doc["zone"] | -1;
        if (z < 0 || z >= NUM_ZONES) {
            req->send(400, "application/json", "{\"error\":\"invalid zone\"}");
            return;
        }
        if (doc["name"].is<const char*>()) {
            zones[z].name = doc["name"].as<String>();
        }
        if (doc["seconds"].is<int>()) {
            int s = doc["seconds"];
            if (s >= 1 && s <= MAX_SPRAY_SECONDS) zones[z].spraySeconds = s;
        }
        saveConfig();
        req->send(200, "application/json", getStatusJson());
    });

    // API: update a zone's schedules (PIN protected)
    server.on("/api/zone/schedules", HTTP_POST, [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            req->send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        if (!checkPin(req, doc)) return;
        int z = doc["zone"] | -1;
        if (z < 0 || z >= NUM_ZONES) {
            req->send(400, "application/json", "{\"error\":\"invalid zone\"}");
            return;
        }

        JsonArray sarr = doc["schedules"].as<JsonArray>();
        zones[z].scheduleCount = 0;
        for (JsonObject sobj : sarr) {
            if (zones[z].scheduleCount >= MAX_SCHEDULES_PER_ZONE) break;
            int s = zones[z].scheduleCount;
            zones[z].schedules[s].enabled = sobj["enabled"] | true;
            zones[z].schedules[s].days = sobj["days"] | 0;
            zones[z].schedules[s].hour = sobj["hour"] | 0;
            zones[z].schedules[s].minute = sobj["minute"] | 0;
            zones[z].scheduleCount++;
        }

        saveConfig();
        req->send(200, "application/json", getStatusJson());
    });
}
