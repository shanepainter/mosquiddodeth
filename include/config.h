#pragma once

// ── Hardware ──
#define BUTTON_PIN 27       // GPIO for manual pushbutton (cycles through zones)
#define LED_PIN 2           // Onboard LED for status
#define SETUP_HOLD_MS 5000  // Hold button 5s to enter setup mode

// ── Zones ──
// Each zone drives one relay for one spray line.
// Add/remove entries to match your wiring. Max 8 zones.
#define NUM_ZONES 4
const int ZONE_PINS[NUM_ZONES] = { 26, 25, 33, 32 };
const char* ZONE_DEFAULT_NAMES[NUM_ZONES] = {
    "Zone 1", "Zone 2", "Zone 3", "Zone 4"
};

// ── Spray defaults ──
#define DEFAULT_SPRAY_SECONDS 90
#define MAX_SPRAY_SECONDS 600

// ── NTP ──
#define NTP_SERVER "pool.ntp.org"
#define TZ_OFFSET -18000    // UTC-5 (CDT) in seconds — adjust for your timezone
#define TZ_DST 3600         // DST offset in seconds (0 if no DST)

// ── mDNS ──
#define MDNS_HOSTNAME "mosquiddo-deth"
// Each device appends last 4 hex of MAC: http://mosquiddo-deth-a1b2.local

// ── Tank level sensor (JSN-SR04T) ──
#define TANK_TRIG_PIN 14    // GPIO for trigger
#define TANK_ECHO_PIN 12    // GPIO for echo
#define TANK_READ_INTERVAL_MS 60000  // read every 60s
#define TANK_LOW_PERCENT 15          // alert threshold

// ── Setup AP ──
#define SETUP_AP_PREFIX "MosquiddoDeth-Setup"
// AP name will be: MosquiddoDeth-Setup-a1b2
