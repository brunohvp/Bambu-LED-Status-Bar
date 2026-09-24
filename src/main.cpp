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

static void setStatusLed(bool on) {
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
static void handleConnectWifi(AsyncWebServerRequest *request, JsonVariant &json) {
    JsonObject obj = json.as<JsonObject>();
    String ssid = obj["ssid"] | "";
    String pass = obj["pass"] | "";

    if (ssid.isEmpty()) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
        return;
    }

    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Connecting to \"%s\"", ssid.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        Serial.print(".");
        delay(150);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        pendingWifiSsid = ssid;
        pendingWifiPass = pass;
        staJoinedDuringSetup = true;
        JsonDocument doc;
        doc["ok"] = true;
        doc["ip"] = WiFi.localIP().toString();
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    } else {
        staJoinedDuringSetup = false;
        WiFi.disconnect();
        request->send(200, "application/json", "{\"ok\":false,\"error\":\"could not connect\"}");
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
    if (pendingWifiSsid.isEmpty()) {
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
    server.begin();

    Serial.println("=====================================");
    Serial.printf("Connected! IP=%s  http://%s.local\n", WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
    Serial.println("=====================================");
    Log::add("[WiFi] connected, IP=%s", WiFi.localIP().toString().c_str());

    LedAnimations::begin();
    // Pinned to core 1 (same as loop()/Arduino default), not core 0 — that's
    // where the WiFi stack runs, and it's already documented as sensitive to
    // timing disruption (see WiFi.setSleep(false) above). FreeRTOS still
    // preemptively time-slices this against loop() on the same core, which is
    // all that's needed to survive a blocking MQTT reconnect without stutter.
    xTaskCreatePinnedToCore(ledTask, "led", 3072, nullptr, 1, nullptr, 1);

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
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&twdtConfig); // Arduino already inits the TWDT; this just widens its timeout

    Serial.begin(115200);
    delay(300);
    Serial.printf("\n%s v%s booting\n", PROJECT_NAME, FIRMWARE_VERSION);

    pinMode(STATUS_LED_PIN, OUTPUT);
    setStatusLed(false);

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
