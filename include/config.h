#pragma once

// ── WiFi ──
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"

// ── Hardware ──
#define RELAY_PIN 26        // GPIO for relay module (active HIGH)
#define BUTTON_PIN 27       // GPIO for manual pushbutton (pulled LOW, press = HIGH)
#define LED_PIN 2           // Onboard LED for status

// ── Spray defaults ──
#define DEFAULT_SPRAY_SECONDS 90
#define MAX_SPRAY_SECONDS 600

// ── NTP ──
#define NTP_SERVER "pool.ntp.org"
#define TZ_OFFSET -18000    // UTC-5 (CDT) in seconds — adjust for your timezone
#define TZ_DST 3600         // DST offset in seconds (0 if no DST)

// ── mDNS ──
#define MDNS_HOSTNAME "mosquitto-death"
// Each device appends last 4 hex of MAC: http://mosquitto-death-a1b2.local
