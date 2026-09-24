#pragma once

// ---- Project / firmware identity ----
#define FIRMWARE_VERSION        "0.1.0"
#define PROJECT_NAME             "Bambu LED Status"
#define PROJECT_MAKERWORLD_URL   "https://makerworld.com/en/models/2172105-bambu-lab-p2s-led-progress-status-bar"
#define PROJECT_GITHUB_URL       "https://github.com/brunohvp/Bambu-LED-Status-Bar"

// ---- Device identity ----
#define DEVICE_NAME_PREFIX   "BambuLED"   // becomes "BambuLED-A1B2" (suffix = last 2 bytes of the MAC)
#define MDNS_HOSTNAME         "bambuled"  // http://bambuled.local once connected to WiFi

// ---- Setup access point ----
#define AP_PASSWORD           ""          // "" = open network. Set a password (min 8 chars) if you prefer.
#define AP_LOCAL_IP           IPAddress(192, 168, 4, 1)
#define AP_GATEWAY            IPAddress(192, 168, 4, 1)
#define AP_SUBNET              IPAddress(255, 255, 255, 0)
#define DNS_PORT               53

// ---- WiFi connection (station mode) ----
#define WIFI_CONNECT_TIMEOUT_MS   20000   // max time trying the saved network before falling back to the AP

// ---- Onboard LED (visual feedback that doesn't depend on the strip) ----
// Most ESP32-WROOM-32 DevKits have a blue LED on GPIO2. Adjust if yours differs.
#define STATUS_LED_PIN         2
#define STATUS_LED_ACTIVE_HIGH true

// ---- WS2811 strip (status bar) ----
// Same pin/count as the original WLED project. Adjust to match your wiring.
#define LED_DATA_PIN            16
#define LED_COUNT                10   // 10 segments = one per 10% of print progress
#define LED_COLOR_ORDER         RGB   // switch to GRB if colors come out swapped (see README)
#define LED_MAX_BRIGHTNESS      255   // global FastLED brightness ceiling (0-255)

// How long (ms) idle before the strip dims into "sleep" mode
#define LED_IDLE_TO_SLEEP_MS   (30UL * 60UL * 1000UL)

// ---- Web server ----
#define HTTP_PORT               80
