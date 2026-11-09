#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>

#include "AppConfig.h"
#include "Storage.h"
#include "WebPages.h"
#include "PrinterDiscovery.h"
#include "PrinterMqtt.h"
#include "LedAnimations.h"
#include "AnimConfig.h"
#include "StripConfig.h"
#include "NightMode.h"
#include "Log.h"

static AsyncWebServer server(HTTP_PORT);
static DNSServer dnsServer;
static AppState state;
static bool apMode = false;

static volatile bool restartRequested = false;
static unsigned long restartAtMs = 0;

// WiFi credentials confirmed working during the setup wizard's step 1, held in
// RAM until step 2 (printer info) is saved together with them.
static String pendingWifiSsid;
static String pendingWifiPass;
static bool staJoinedDuringSetup = false;

// Step 1 runs in the background: /connect-wifi only queues the join and
// answers at once, loop() drives it (serviceSetupWifi), and the page polls
// /wifi-status. Joining the home network moves the setup AP to the router's
// channel, which briefly drops the phone off the AP — a request held open for
// the whole join lost its response right there ("Request failed" although the
// join worked). Polling simply retries through that gap.
enum class SetupWifiState : uint8_t { Idle, Connecting, Connected, Failed };
static volatile SetupWifiState setupWifiState = SetupWifiState::Idle;
static portMUX_TYPE connectMux = portMUX_INITIALIZER_UNLOCKED;
static bool connectRequested = false;     // guarded by connectMux
static char requestedSsid[33];            // guarded by connectMux (fixed buffers: no malloc inside the lock)
static char requestedPass[65];
static String connectingSsid;             // loop() task only
static String connectingPass;
static unsigned long connectStartMs = 0;

// ---------------- helpers ----------------

static String deviceName() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[24];
    snprintf(buf, sizeof(buf), "%s-%04X", DEVICE_NAME_PREFIX, (uint16_t)(mac >> 32));
    return String(buf);
}

static String maskSecret(const String &s) {
    if (s.length() <= 2) return "**";
    String out = s.substring(0, 1);
    for (size_t i = 1; i + 1 < s.length(); i++) out += "*";
    out += s.substring(s.length() - 1);
    return out;
}

static void requestRestart(unsigned long delayMs) {
    restartRequested = true;
    restartAtMs = millis() + delayMs;
}

// Off when the LED strip's data pin is the status LED's pin (e.g. GPIO8 on a
// C3): the strip owns that pin then, and toggling it would garble the data.
static bool statusLedEnabled = true;

static void setStatusLed(bool on) {
    if (!statusLedEnabled) return;
    digitalWrite(STATUS_LED_PIN, on == STATUS_LED_ACTIVE_HIGH ? HIGH : LOW);
}

static String colorToHex(RgbColor c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return String(buf);
}

static RgbColor hexToColor(const String &hex) {
    RgbColor c{0, 0, 0};
    if (hex.length() >= 7 && hex[0] == '#') {
        c.r = (uint8_t)strtol(hex.substring(1, 3).c_str(), nullptr, 16);
        c.g = (uint8_t)strtol(hex.substring(3, 5).c_str(), nullptr, 16);
        c.b = (uint8_t)strtol(hex.substring(5, 7).c_str(), nullptr, 16);
    }
    return c;
}

static void stateAnimToJson(JsonObject o, const StateAnimConfig &c) {
    o["effect"] = ledEffectName(c.effect);
    o["color1"] = colorToHex(c.color1);
    o["color2"] = colorToHex(c.color2);
}

// Note: `effect` is intentionally not read here — each state has a fixed,
// hand-picked animation (see AnimConfigStore::defaults()), not a user-editable
// one, so c.effect stays whatever it already was (loaded from storage).
static void jsonToStateAnim(JsonObject o, StateAnimConfig &c) {
    if (o.isNull()) return;
    c.color1 = hexToColor(o["color1"] | "#000000");
    c.color2 = hexToColor(o["color2"] | "#000000");
}

// ---------------- AP mode (setup portal) ----------------

static void handleScan(AsyncWebServerRequest *request) {
    int n = WiFi.scanNetworks();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    String out;
    serializeJson(doc, out);
    WiFi.scanDelete();
    request->send(200, "application/json", out);
}

// Step 1: join the home WiFi network while keeping the setup AP alive (WIFI_AP_STA),
// so the browser stays connected to the portal while STA associates in the background.
// Only queues the join (see SetupWifiState above); the page then polls /wifi-status.
static void handleConnectWifi(AsyncWebServerRequest *request, JsonVariant &json) {
    JsonObject obj = json.as<JsonObject>();
    String ssid = obj["ssid"] | "";
    String pass = obj["pass"] | "";

    if (ssid.isEmpty() || ssid.length() >= sizeof(requestedSsid) || pass.length() >= sizeof(requestedPass)) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid ssid or password\"}");
        return;
    }

    // Already on that network (e.g. the page was reloaded after a lost
    // response): re-joining would only drop the AP off-channel again.
    if (setupWifiState == SetupWifiState::Connected && WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
        request->send(200, "application/json", "{\"ok\":true}");
        return;
    }

    portENTER_CRITICAL(&connectMux);
    strlcpy(requestedSsid, ssid.c_str(), sizeof(requestedSsid));
    strlcpy(requestedPass, pass.c_str(), sizeof(requestedPass));
    connectRequested = true;
    portEXIT_CRITICAL(&connectMux);
    request->send(200, "application/json", "{\"ok\":true}");
}

static void handleWifiStatus(AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&connectMux);
    bool queued = connectRequested;
    portEXIT_CRITICAL(&connectMux);

    SetupWifiState s = queued ? SetupWifiState::Connecting : (SetupWifiState)setupWifiState;
    // Joined but the link dropped since: the STA reconnects on its own, so report it as in progress.
    if (s == SetupWifiState::Connected && WiFi.status() != WL_CONNECTED) s = SetupWifiState::Connecting;

    JsonDocument doc;
    switch (s) {
        case SetupWifiState::Idle: doc["state"] = "idle"; break;
        case SetupWifiState::Connecting: doc["state"] = "connecting"; break;
        case SetupWifiState::Connected:
            doc["state"] = "connected";
            doc["ssid"] = WiFi.SSID();
            doc["ip"] = WiFi.localIP().toString();
            break;
        case SetupWifiState::Failed: doc["state"] = "failed"; break;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// Runs from loop() in setup mode: starts a queued join and watches how it ends.
static void serviceSetupWifi() {
    char ssid[sizeof(requestedSsid)];
    char pass[sizeof(requestedPass)];
    bool start = false;
    portENTER_CRITICAL(&connectMux);
    if (connectRequested) {
        memcpy(ssid, requestedSsid, sizeof(ssid));
        memcpy(pass, requestedPass, sizeof(pass));
        connectRequested = false;
        start = true;
    }
    portEXIT_CRITICAL(&connectMux);

    if (start) {
        staJoinedDuringSetup = false;
        connectingSsid = ssid;
        connectingPass = pass;
        connectStartMs = millis();
        setupWifiState = SetupWifiState::Connecting;
        Log::add("[Setup] joining \"%s\"...", ssid);
        WiFi.begin(ssid, pass);
        return;
    }

    if (setupWifiState != SetupWifiState::Connecting) return;

    if (WiFi.status() == WL_CONNECTED) {
        pendingWifiSsid = connectingSsid;
        pendingWifiPass = connectingPass;
        staJoinedDuringSetup = true;
        setupWifiState = SetupWifiState::Connected; // set last: /save-printer reads pendingWifi* only after this
        Log::add("[Setup] joined \"%s\", IP=%s", connectingSsid.c_str(), WiFi.localIP().toString().c_str());
    } else if (millis() - connectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
        WiFi.disconnect();
        setupWifiState = SetupWifiState::Failed;
        Log::add("[Setup] could not join \"%s\" (status %d)", connectingSsid.c_str(), (int)WiFi.status());
    }
}

// Step 2: now that STA is joined to the home LAN, listen for the printer's SSDP broadcast.
static void handleScanPrinter(AsyncWebServerRequest *request) {
    if (!staJoinedDuringSetup || WiFi.status() != WL_CONNECTED) {
        request->send(409, "application/json", "{\"ok\":false,\"error\":\"wifi not connected yet\"}");
        return;
    }

    auto results = PrinterDiscovery::scan(4000);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (auto &p : results) {
        JsonObject o = arr.add<JsonObject>();
        o["ip"] = p.ip;
        o["serial"] = p.serial;
        o["model"] = p.model;
        o["name"] = p.name;
        o["lan_mode"] = p.lanMode;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// Step 2 (finish): persist WiFi (confirmed in step 1) + printer info together, then reboot.
static void handleSavePrinter(AsyncWebServerRequest *request, JsonVariant &json) {
    if (setupWifiState != SetupWifiState::Connected || pendingWifiSsid.isEmpty()) {
        request->send(409, "application/json", "{\"ok\":false,\"error\":\"connect to wifi first\"}");
        return;
    }

    JsonObject obj = json.as<JsonObject>();
    String pip = obj["printer_ip"] | "";
    String pserial = obj["printer_serial"] | "";
    String pcode = obj["printer_access_code"] | "";

    if (pip.isEmpty() || pserial.isEmpty() || pcode.isEmpty()) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"missing printer fields\"}");
        return;
    }

    Storage::saveAll(pendingWifiSsid, pendingWifiPass, pip, pserial, pcode);
    request->send(200, "application/json", "{\"ok\":true}");
    requestRestart(1500);
}

static void startApPortal() {
    apMode = true;
    WiFi.mode(WIFI_AP_STA); // AP_STA so we can scan for networks while the AP is up
    WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY, AP_SUBNET);

    String apName = deviceName();
    if (strlen(AP_PASSWORD) >= 8) {
        WiFi.softAP(apName.c_str(), AP_PASSWORD);
    } else {
        WiFi.softAP(apName.c_str());
    }

    dnsServer.start(DNS_PORT, "*", AP_LOCAL_IP);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", SETUP_PAGE_HTML);
    });
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/scan-printer", HTTP_GET, handleScanPrinter);
    server.on("/wifi-status", HTTP_GET, handleWifiStatus);

    auto *connectHandler = new AsyncCallbackJsonWebHandler("/connect-wifi", handleConnectWifi);
    server.addHandler(connectHandler);
    auto *savePrinterHandler = new AsyncCallbackJsonWebHandler("/save-printer", handleSavePrinter);
    server.addHandler(savePrinterHandler);

    // Captive portal: any other URL falls back to the setup page (makes the OS pop the browser open on its own)
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(200, "text/html", SETUP_PAGE_HTML);
    });

    server.begin();

    Serial.println("=====================================");
    Serial.printf("SETUP mode. Connect to WiFi \"%s\"\n", apName.c_str());
    Serial.println("and open http://192.168.4.1 (or wait for the captive portal to pop up on its own)");
    Serial.println("=====================================");
}

// ---------------- station mode (connected) ----------------

static void handleApiStatus(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["wifi_ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["hostname"] = String(MDNS_HOSTNAME) + ".local";
    doc["printer_ip"] = state.printerIp;
    doc["printer_serial"] = state.printerSerial;
    doc["printer_access_code_masked"] = maskSecret(state.printerAccessCode);
    doc["uptime_s"] = millis() / 1000;

    doc["mqtt_connected"] = PrinterMqtt::isConnected();
    PrinterStatus ps = PrinterMqtt::getStatus();
    doc["printer_state"] = printerStateName(ps.state);
    doc["printer_gcode_state"] = ps.gcodeState;
    doc["printer_percent"] = ps.percent;
    doc["printer_remaining_min"] = ps.remainingMinutes;
    doc["printer_nozzle_temp"] = ps.nozzleTemp;
    doc["printer_bed_temp"] = ps.bedTemp;
    doc["printer_subtask"] = ps.subtaskName;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

static void handleApiInfo(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["project_name"] = PROJECT_NAME;
    doc["makerworld_url"] = PROJECT_MAKERWORLD_URL;
    doc["github_url"] = PROJECT_GITHUB_URL;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

static void handleGetLogs(AsyncWebServerRequest *request) {
    request->send(200, "text/plain", Log::recent());
}

// ---- animation settings API ----

static void handleGetAnim(AsyncWebServerRequest *request) {
    AnimConfigSet cfg = AnimConfigStore::load();
    JsonDocument doc;
    stateAnimToJson(doc["idle"].to<JsonObject>(), cfg.idle);
    stateAnimToJson(doc["calibrating"].to<JsonObject>(), cfg.calibrating);
    stateAnimToJson(doc["printing"].to<JsonObject>(), cfg.printing);
    stateAnimToJson(doc["paused"].to<JsonObject>(), cfg.paused);
    stateAnimToJson(doc["finished"].to<JsonObject>(), cfg.finished);
    stateAnimToJson(doc["error"].to<JsonObject>(), cfg.error);
    doc["brightness"] = cfg.brightness;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

static void handleSaveAnim(AsyncWebServerRequest *request, JsonVariant &json) {
    JsonObject obj = json.as<JsonObject>();
    AnimConfigSet cfg = AnimConfigStore::load(); // start from what's saved so a partial body doesn't wipe the rest
    jsonToStateAnim(obj["idle"], cfg.idle);
    jsonToStateAnim(obj["calibrating"], cfg.calibrating);
    jsonToStateAnim(obj["printing"], cfg.printing);
    jsonToStateAnim(obj["paused"], cfg.paused);
    jsonToStateAnim(obj["finished"], cfg.finished);
    jsonToStateAnim(obj["error"], cfg.error);
    if (!obj["brightness"].isNull()) cfg.brightness = obj["brightness"].as<int>();

    AnimConfigStore::save(cfg);
    LedAnimations::reloadConfig(); // applies immediately, no reboot needed
    request->send(200, "application/json", "{\"ok\":true}");
}

static void handleResetAnim(AsyncWebServerRequest *request) {
    AnimConfigStore::save(AnimConfigStore::defaults());
    LedAnimations::reloadConfig();
    request->send(200, "application/json", "{\"ok\":true}");
}

// ---- LED strip hardware API (count / chipset / color order) ----

static void handleGetStrip(AsyncWebServerRequest *request) {
    StripConfig cfg = StripConfigStore::load();
    JsonDocument doc;
    doc["count"] = cfg.count;
    doc["chipset"] = ledChipsetId(cfg.chipset);
    doc["order"] = ledColorOrderName(cfg.order);
    doc["max_count"] = LED_MAX_COUNT;
    doc["pin"] = cfg.pin;
    JsonArray pins = doc["pins"].to<JsonArray>();
    size_t pinCount;
    const LedPinInfo *allowed = ledAllowedPins(pinCount);
    for (size_t i = 0; i < pinCount; i++) {
        JsonObject o = pins.add<JsonObject>();
        o["pin"] = allowed[i].pin;
        if (allowed[i].warning) o["warning"] = allowed[i].warning;
    }
    JsonArray chipsets = doc["chipsets"].to<JsonArray>();
    for (uint8_t i = 0; i < (uint8_t)LedChipset::Count_; i++) {
        JsonObject o = chipsets.add<JsonObject>();
        o["id"] = ledChipsetId((LedChipset)i);
        o["label"] = ledChipsetLabel((LedChipset)i);
        o["default_order"] = ledColorOrderName(ledChipsetDefaultOrder((LedChipset)i));
    }
    JsonArray orders = doc["orders"].to<JsonArray>();
    for (uint8_t i = 0; i < (uint8_t)LedColorOrder::Count_; i++) {
        orders.add(ledColorOrderName((LedColorOrder)i));
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// Saves the new strip setup and reboots to apply it (FastLED fixes the
// chipset when the controller is created at boot).
static void handleSaveStrip(AsyncWebServerRequest *request, JsonVariant &json) {
    JsonObject obj = json.as<JsonObject>();
    StripConfig cfg = StripConfigStore::load(); // start from what's saved so a partial body keeps the rest

    if (!obj["count"].isNull()) {
        int count = obj["count"].as<int>();
        if (count < 1 || count > LED_MAX_COUNT) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"count out of range\"}");
            return;
        }
        cfg.count = (uint16_t)count;
    }
    if (!obj["chipset"].isNull() && !parseLedChipset(obj["chipset"].as<const char *>(), cfg.chipset)) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"unknown chipset\"}");
        return;
    }
    if (!obj["order"].isNull() && !parseLedColorOrder(obj["order"].as<const char *>(), cfg.order)) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"unknown color order\"}");
        return;
    }
    if (!obj["pin"].isNull()) {
        int pin = obj["pin"].as<int>();
        if (!ledPinAllowed(pin)) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"pin not allowed\"}");
            return;
        }
        cfg.pin = (uint8_t)pin;
    }

    StripConfigStore::save(cfg);
    Log::add("[LED] strip set to %u x %s (%s) on GPIO%u, restarting", cfg.count, ledChipsetId(cfg.chipset),
             ledColorOrderName(cfg.order), cfg.pin);
    LedAnimations::blackout();
    request->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    requestRestart(1500);
}

// ---- night mode (time-of-day brightness) API ----

static String minutesToHhmm(uint16_t m) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", m / 60, m % 60);
    return String(buf);
}

// "HH:MM" -> minutes after midnight; false if malformed.
static bool hhmmToMinutes(const char *s, uint16_t &out) {
    if (!s) return false;
    unsigned h, m;
    char extra;
    if (sscanf(s, "%u:%u%c", &h, &m, &extra) != 2 || h > 23 || m > 59) return false;
    out = (uint16_t)(h * 60 + m);
    return true;
}

static void handleGetNight(AsyncWebServerRequest *request) {
    NightModeConfig cfg = NightModeStore::load();
    JsonDocument doc;
    doc["enabled"] = cfg.enabled;
    doc["start"] = minutesToHhmm(cfg.startMin);
    doc["end"] = minutesToHhmm(cfg.endMin);
    doc["brightness"] = cfg.brightness;
    doc["tz"] = cfg.tz;
    JsonArray zones = doc["zones"].to<JsonArray>();
    size_t zoneCount;
    const TimeZoneInfo *tz = timeZones(zoneCount);
    for (size_t i = 0; i < zoneCount; i++) zones.add(tz[i].label);
    doc["time_synced"] = Clock::synced();
    doc["local_time"] = Clock::localTimeString();
    doc["night_active"] = LedAnimations::nightActive();
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

static void handleSaveNight(AsyncWebServerRequest *request, JsonVariant &json) {
    JsonObject obj = json.as<JsonObject>();
    NightModeConfig cfg = NightModeStore::load(); // start from what's saved so a partial body keeps the rest
    uint8_t oldTz = cfg.tz;

    if (!obj["enabled"].isNull()) cfg.enabled = obj["enabled"].as<bool>();
    if (!obj["start"].isNull() && !hhmmToMinutes(obj["start"].as<const char *>(), cfg.startMin)) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad start time\"}");
        return;
    }
    if (!obj["end"].isNull() && !hhmmToMinutes(obj["end"].as<const char *>(), cfg.endMin)) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad end time\"}");
        return;
    }
    if (!obj["brightness"].isNull()) cfg.brightness = (uint8_t)constrain(obj["brightness"].as<int>(), 0, 255);
    if (!obj["tz"].isNull()) {
        size_t zoneCount;
        timeZones(zoneCount);
        int tz = obj["tz"].as<int>();
        if (tz < 0 || (size_t)tz >= zoneCount) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"unknown time zone\"}");
            return;
        }
        cfg.tz = (uint8_t)tz;
    }

    NightModeStore::save(cfg);
    if (cfg.tz != oldTz) Clock::setZone(cfg.tz);
    LedAnimations::reloadConfig(); // applies immediately, no reboot needed
    Log::add("[LED] night mode %s, %s-%s at %u", cfg.enabled ? "enabled" : "disabled",
             minutesToHhmm(cfg.startMin).c_str(), minutesToHhmm(cfg.endMin).c_str(), cfg.brightness);
    request->send(200, "application/json", "{\"ok\":true}");
}

// How long to keep trusting the last known state (e.g. still showing red
// after a cancelled print) once reports stop arriving, before giving up and
// showing "disconnected" instead. Long enough to ride out a normal TLS
// reconnect blip without flickering; short enough that turning the printer
// off for real doesn't leave the LED lying about its state indefinitely.
static const unsigned long STATE_STALE_MS = 30UL * 1000UL;

// Runs on its own FreeRTOS task (pinned to the core opposite loop()) so a
// blocking MQTT reconnect — which happens periodically due to the TLS
// instability documented in the README — never stalls the animation. Most
// effects (a full-strip fade/wave) hid that stall well; Loading's single
// moving pixel made it obvious as a visible stutter.
static void ledTask(void *) {
    for (;;) {
        PrinterStatus ps = PrinterMqtt::getStatus();
        // Trust the last known state through brief reconnects (the printer
        // re-asserts it every ~1s anyway) — snapping to Unknown on every
        // disconnect made the strip flicker gray during the residual TLS
        // hiccups instead of just holding steady. But don't trust it forever:
        // once nothing's arrived in STATE_STALE_MS, the printer is more
        // likely actually off/unreachable than mid-blip.
        bool stale = ps.everConnected && (millis() - ps.lastUpdateMs > STATE_STALE_MS);
        PrinterState ledState = (state.hasPrinter() && ps.everConnected && !stale) ? ps.state : PrinterState::Unknown;
        LedAnimations::update(ledState, ps.percent, ps.chamberLightOn);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void startStaMode() {
    apMode = false;
    WiFi.mode(WIFI_STA);

    // WiFi modem sleep (power saving) inserts periodic gaps in the radio's
    // timing that are a well-documented cause of random TLS/TCP corruption on
    // ESP32 (mbedTLS "MAC verification failed" / "invalid SSL record" errors),
    // and can also fight with FastLED's RMT-driven LED output. This device is
    // always mains-powered, so there's no reason to keep it on.
    WiFi.setSleep(false);

    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", HTTP_PORT);
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", STATUS_PAGE_HTML);
    });
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", SETTINGS_PAGE_HTML);
    });
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/info", HTTP_GET, handleApiInfo);
    server.on("/api/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
        Storage::clearWifi();
        request->send(200, "application/json", "{\"ok\":true}");
        requestRestart(1000);
    });
    server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        Storage::clearAll();
        request->send(200, "application/json", "{\"ok\":true}");
        requestRestart(1000);
    });

    server.on("/api/logs", HTTP_GET, handleGetLogs);

    server.on("/api/anim", HTTP_GET, handleGetAnim);
    auto *saveAnimHandler = new AsyncCallbackJsonWebHandler("/api/anim", handleSaveAnim);
    server.addHandler(saveAnimHandler);
    server.on("/api/anim/reset", HTTP_POST, handleResetAnim);

    server.on("/api/strip", HTTP_GET, handleGetStrip);
    auto *saveStripHandler = new AsyncCallbackJsonWebHandler("/api/strip", handleSaveStrip);
    server.addHandler(saveStripHandler);

    server.on("/api/night", HTTP_GET, handleGetNight);
    auto *saveNightHandler = new AsyncCallbackJsonWebHandler("/api/night", handleSaveNight);
    server.addHandler(saveNightHandler);
    server.begin();

    // Wall-clock time for the optional night mode (NTP runs in the background).
    Clock::begin(NightModeStore::load().tz);

    Serial.println("=====================================");
    Serial.printf("Connected! IP=%s  http://%s.local\n", WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
    Serial.println("=====================================");
    Log::add("[WiFi] connected, IP=%s", WiFi.localIP().toString().c_str());

    LedAnimations::begin();
    // Pinned to the same core as loop() (ARDUINO_RUNNING_CORE: core 1 on the
    // dual-core ESP32, core 0 on single-core chips like the ESP32-C3), not the
    // WiFi core on dual-core chips — WiFi is already documented as sensitive to
    // timing disruption (see WiFi.setSleep(false) above). FreeRTOS still
    // preemptively time-slices this against loop() on the same core, which is
    // all that's needed to survive a blocking MQTT reconnect without stutter.
    xTaskCreatePinnedToCore(ledTask, "led", 3072, nullptr, 1, nullptr, ARDUINO_RUNNING_CORE);

    if (state.hasPrinter()) {
        PrinterMqtt::begin(state.printerIp, state.printerSerial, state.printerAccessCode);
    } else {
        Serial.println("No printer configured yet (reconfigure via the setup portal).");
    }
}

// tries to connect with the saved credential; returns true on success
static bool tryConnectSTA() {
    String dname = deviceName();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(dname.c_str());
    WiFi.begin(state.wifiSsid.c_str(), state.wifiPass.c_str());

    Serial.printf("Connecting to \"%s\"", state.wifiSsid.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        setStatusLed((millis() / 150) % 2 == 0);
        Serial.print(".");
        delay(50);
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

// ---------------- Arduino entrypoints ----------------

void setup() {
    // ponytail: the setup portal's /scan, /scan-printer and /connect-wifi handlers
    // block synchronously for up to ~20s (real fix: make them async/pollable) —
    // that starves the AsyncTCP task long enough to trip the default 5s task
    // watchdog and hard-crash the board. Widening the watchdog is the corner cut;
    // upgrade path is rewriting those 3 handlers to kick off work and poll for
    // completion instead of blocking inline.
    esp_task_wdt_config_t twdtConfig = {
        .timeout_ms = 60000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // every core's idle task (1 core on the C3)
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&twdtConfig); // Arduino already inits the TWDT; this just widens its timeout

    Serial.begin(115200);
    delay(300);
    Serial.printf("\n%s v%s booting\n", PROJECT_NAME, FIRMWARE_VERSION);

    statusLedEnabled = StripConfigStore::load().pin != STATUS_LED_PIN;
    if (statusLedEnabled) {
        pinMode(STATUS_LED_PIN, OUTPUT);
        setStatusLed(false);
    }

    state = Storage::load();

    if (state.hasWifi() && tryConnectSTA()) {
        setStatusLed(true);
        startStaMode();
    } else {
        if (state.hasWifi()) Serial.println("Could not connect to the saved network. Opening the setup portal.");
        startApPortal();
    }
}

void loop() {
    if (apMode) {
        dnsServer.processNextRequest();
        serviceSetupWifi();
        // slow blink in setup mode
        setStatusLed((millis() / 500) % 2 == 0);
        // dnsServer.processNextRequest() runs in a tight loop with nothing else
        // to do in AP mode — without yielding, this can starve other FreeRTOS
        // tasks (including AsyncTCP) of CPU time long enough to trip the task
        // watchdog. 1ms is enough to let the scheduler breathe without making
        // the captive portal feel slow.
        delay(1);
    } else {
        PrinterMqtt::loop();
        // LED rendering runs on its own task (see ledTask) so a blocking MQTT
        // reconnect never stalls the animation.
    }

    static unsigned long lastHeapLogMs = 0;
    if (millis() - lastHeapLogMs > 30000) {
        lastHeapLogMs = millis();
        // Free vs. largest-contiguous-block: a big gap between them means the
        // heap is fragmented, even if the free total looks healthy.
        Serial.printf("[SYS] free heap: %u bytes (largest block: %u)\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    }

    if (restartRequested && millis() > restartAtMs) {
        Serial.println("Restarting...");
        delay(50);
        ESP.restart();
    }
}
