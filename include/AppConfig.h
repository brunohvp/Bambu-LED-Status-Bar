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

// Pins can be overridden per board from platformio.ini (build_flags -D...),
// see the esp32c3 env there; the values below are the ESP32 DevKit defaults.

// ---- Onboard LED (visual feedback that doesn't depend on the strip) ----
// Most ESP32-WROOM-32 DevKits have a blue LED on GPIO2. Adjust if yours differs.
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN         2
#endif
#ifndef STATUS_LED_ACTIVE_HIGH
#define STATUS_LED_ACTIVE_HIGH true
#endif

// ---- LED strip (status bar) ----
// Data pin, LED count, chipset and color order are set from the web UI
// (Settings > LED strip) and stored in NVS — the values below are only the
// first-boot defaults (see StripConfig.h). They match the original hardcoded
// strip (10 x WS2811, RGB, GPIO16), so existing builds keep working after an update.
#ifndef LED_DATA_PIN
#define LED_DATA_PIN            16
#endif
#define LED_DEFAULT_COUNT        10   // 10 segments = one per 10% of print progress
#define LED_DEFAULT_CHIPSET     LedChipset::WS2811
#define LED_DEFAULT_COLOR_ORDER LedColorOrder::RGB
#define LED_MAX_COUNT           300   // upper limit accepted from the web UI
#define LED_MAX_BRIGHTNESS      255   // global FastLED brightness ceiling (0-255)

// How long (ms) idle before the strip dims into "sleep" mode
#define LED_IDLE_TO_SLEEP_MS   (30UL * 60UL * 1000UL)

// ---- Web server ----
#define HTTP_PORT               80
