#include "PrinterMqtt.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "Log.h"

// Bambu Lab's local MQTT protocol (LAN Mode / Developer Mode), documented by the
// community at https://github.com/Doridian/OpenBambuAPI/blob/main/mqtt.md
//   host: the printer's IP, port 8883, TLS
//   user: "bblp"   password: the LAN Mode Access Code
//   subscribes: device/<serial>/report
//
// TLS/MQTT stability history (see README > Phase 2/4 for the full story):
//   1. PubSubClient w/ a large *compile-time* buffer -> corrupted the TLS session
//      on big messages.
//   2. 256dpi/MQTT -> still unstable; its remaining-length parsing seems to
//      misread message sizes over WiFiClientSecure regardless of buffer size.
//   3. Back to PubSubClient with a small buffer -> mostly silent message loss,
//      not a fix either.
//   4. This version: modeled after github.com/Keralots/BambuHelper (a much more
//      mature project doing the same job) — a generous *runtime* buffer via
//      setBufferSize(), and critically, fully destroying and recreating the
//      WiFiClientSecure + PubSubClient objects on every reconnect instead of
//      reusing them. Reusing them after a TLS error seems to carry over stale
//      mbedTLS session state that corrupts everything read afterwards.
//   5. Tried bypassing WiFiClientSecure entirely with ESP-IDF's native
//      esp_mqtt_client + esp-tls — hit a hard wall: this prebuilt Arduino SDK
//      doesn't have CONFIG_ESP_TLS_INSECURE compiled in, so esp-tls refuses to
//      connect at all without a real CA cert ("No server verification option
//      set"). Fixing that means switching to framework = espidf, arduino with a
//      custom sdkconfig — a much bigger, riskier build-system change. Reverted
//      to this version, which at least works end-to-end with self-healing
//      reconnects.
//   6. Also tried auto-publishing Bambu Studio/Handy's "resume" command to
//      clear a stuck gcode_state=FAILED. Requires Developer Mode enabled on
//      the printer (else every third-party command is rejected with
//      HMS-0500-0500-0001-0007 "MQTT Command verification failed") — even
//      with that on, it still didn't clear it on real hardware once the job
//      had actually terminated. Not worth the security trade-off of leaving
//      Developer Mode on for a feature that doesn't reliably work; a printer
//      reboot remains the only fix.
static const uint16_t MQTT_PORT = 8883;
static const unsigned long RECONNECT_INTERVAL_MS = 5000;
static const uint16_t MQTT_BUFFER_SIZE = 24576; // comfortably above the 20000 a P2S owner confirmed fixed this exact issue

static WiFiClientSecure *secureClient = nullptr;
static PubSubClient *mqtt = nullptr;

static String g_ip, g_serial, g_accessCode;
static String g_reportTopic;
static PrinterStatus g_status;
static unsigned long g_lastReconnectAttempt = 0;

// The printer sends a report roughly once a second. If PubSubClient still
// thinks it's connected() but nothing has actually arrived in a while, the
// underlying TLS stream has likely gone silently corrupt (a "zombie"
// connection) — treat that the same as a real disconnect and force a fresh
// reconnect instead of waiting forever.
static const unsigned long MESSAGE_TIMEOUT_MS = 30000;
static unsigned long g_lastMessageMs = 0;

static PrinterState lastLoggedState = PrinterState::Unknown;
static int lastLoggedPercent = -1;
static int lastLoggedStage = -999;
static String lastLoggedHms;
static long lastLoggedPrintError = 0;
static uint32_t g_messageCount = 0;

static const char *mqttStateName(int state) {
    switch (state) {
        case MQTT_CONNECTION_TIMEOUT: return "CONNECTION_TIMEOUT";
        case MQTT_CONNECTION_LOST: return "CONNECTION_LOST";
        case MQTT_CONNECT_FAILED: return "CONNECT_FAILED";
        case MQTT_DISCONNECTED: return "DISCONNECTED";
        case MQTT_CONNECTED: return "CONNECTED";
        case MQTT_CONNECT_BAD_PROTOCOL: return "BAD_PROTOCOL";
        case MQTT_CONNECT_BAD_CLIENT_ID: return "BAD_CLIENT_ID";
        case MQTT_CONNECT_UNAVAILABLE: return "SERVER_UNAVAILABLE";
        case MQTT_CONNECT_BAD_CREDENTIALS: return "BAD_CREDENTIALS";
        case MQTT_CONNECT_UNAUTHORIZED: return "UNAUTHORIZED";
        default: return "UNKNOWN";
    }
}

// Best-effort mapping — only the well-documented states. Anything else falls
// back to Unknown, and the raw gcode_state stays visible in the log/API to help
// figure it out.
static PrinterState mapGcodeState(const String &s) {
    if (s == "RUNNING") return PrinterState::Printing;
    if (s == "PAUSE" || s == "PAUSED") return PrinterState::Paused;
    if (s == "FINISH" || s == "FINISHED") return PrinterState::Finished;
    if (s == "FAILED") return PrinterState::Error;
    if (s == "PREPARE") return PrinterState::Calibrating;
    if (s == "IDLE") return PrinterState::Idle;
    return PrinterState::Unknown;
}

// gcode_state="PREPARE" just means "getting ready" — stg_cur says what for
// (homing, bed leveling, loading filament, ...). IDs sourced from the
// community-maintained mapping in greghesp/ha-bambulab (pybambu/const.py,
// CURRENT_STAGE_IDS) since Bambu doesn't publish this table anywhere.
// Returns Unknown for "printing"(0)/idle(-1,255)/anything unrecognized,
// meaning "don't override — gcode_state already has this covered".
static PrinterState mapStage(int stg) {
    switch (stg) {
        case 1:  // auto_bed_leveling
        case 2:  // heatbed_preheating
        case 3:  // sweeping_xy_mech_mode
        case 4:  // changing_filament
        case 7:  // heating_hotend
        case 8:  // calibrating_extrusion
        case 9:  // scanning_bed_surface
        case 10: // inspecting_first_layer
        case 11: // identifying_build_plate_type
        case 12: // calibrating_micro_lidar
        case 13: // homing_toolhead
        case 14: // cleaning_nozzle_tip
        case 15: // checking_extruder_temperature
        case 18: // calibrating_micro_lidar (dup id in source table)
        case 19: // calibrating_extrusion_flow
        case 22: // filament_unloading
        case 24: // filament_loading
        case 25: // calibrating_motor_noise
            return PrinterState::Calibrating; // covers heating too — no separate state for it
        case 5:  // m400_pause
        case 6:  // paused_filament_runout
        case 16: // paused_user
        case 17: // paused_front_cover_falling
        case 20: // paused_nozzle_temperature_malfunction
        case 21: // paused_heat_bed_temperature_malfunction
        case 23: // paused_skipped_step
        case 26: // paused_ams_lost
        case 27: // paused_low_fan_speed_heat_break
        case 28: // paused_chamber_temperature_control_error
        case 30: // paused_user_gcode
        case 32: // paused_nozzle_filament_covered_detected
        case 33: // paused_cutter_error
        case 34: // paused_first_layer_error
        case 35: // paused_nozzle_clog
            return PrinterState::Paused;
        default:
            return PrinterState::Unknown;
    }
}

// Bambu's report can carry AMS/HMS/tray data we never read. Filtering tells
// ArduinoJson to only allocate storage for the handful of fields below, so
// parsing stays cheap regardless of what else is in the message.
static void buildStatusFilter(JsonDocument &filter) {
    JsonObject print = filter["print"].to<JsonObject>();
    print["gcode_state"] = true;
    print["mc_percent"] = true;
    print["mc_remaining_time"] = true;
    print["nozzle_temper"] = true;
    print["bed_temper"] = true;
    print["subtask_name"] = true;
    print["print_error"] = true; // 0 = no error, per OpenBambuAPI docs — simplest error signal
    print["hms"] = true; // raw array, logged as-is for now — no public doc on code/severity layout yet
    print["stg_cur"] = true; // sub-stage while PREPARE-ing (homing, leveling, loading filament...)
    print["lights_report"] = true; // [{"node":"chamber_light","mode":"on"/"off"}, ...] — dims the idle animation when off
}

static void onMessage(char *topic, byte *payload, unsigned int length) {
    g_messageCount++;
    g_lastMessageMs = millis(); // any message counts, even if it fails to parse below

    JsonDocument filter;
    buildStatusFilter(filter);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[MQTT] JSON parse error: %s (len=%u) — likely a corrupted read, not a real parse issue\n", err.c_str(), length);
        return;
    }

    JsonObject print = doc["print"];
    if (print.isNull()) {
        Serial.printf("[MQTT] message (len=%u) had no \"print\" object — ignored\n", length);
        return;
    }

    if (!print["gcode_state"].isNull()) {
        g_status.gcodeState = print["gcode_state"].as<String>();
        g_status.state = mapGcodeState(g_status.gcodeState);
    }
    if (!print["mc_percent"].isNull()) g_status.percent = print["mc_percent"].as<int>();
    if (!print["mc_remaining_time"].isNull()) g_status.remainingMinutes = print["mc_remaining_time"].as<int>();
    if (!print["nozzle_temper"].isNull()) g_status.nozzleTemp = print["nozzle_temper"].as<float>();
    if (!print["bed_temper"].isNull()) g_status.bedTemp = print["bed_temper"].as<float>();
    if (!print["subtask_name"].isNull()) g_status.subtaskName = print["subtask_name"].as<String>();

    JsonArray lightsReport = print["lights_report"];
    if (!lightsReport.isNull()) {
        for (JsonObject light : lightsReport) {
            if (light["node"] == "chamber_light") {
                bool on = light["mode"] != "off";
                if (on != g_status.chamberLightOn) {
                    Log::add("[MQTT] chamber_light -> %s", on ? "on" : "off");
                }
                g_status.chamberLightOn = on;
            }
        }
    }

    if (!print["stg_cur"].isNull()) {
        int stage = print["stg_cur"].as<int>();
        if (stage != lastLoggedStage) {
            Serial.printf("[MQTT] stg_cur -> %d\n", stage);
            lastLoggedStage = stage;
        }
        // Refine Calibrating (covers gcode_state=PREPARE already, but stg_cur
        // can also mean the printer's mid-print pause sub-reasons) and,
        // confirmed on real hardware, homing/leveling/filament-change
        // sub-stages that happen *after* gcode_state has already flipped to
        // RUNNING (some print jobs fold the initial homing into the job
        // itself rather than a separate PREPARE phase) — mapStage() returns
        // Unknown for "printing"(0)/idle, which leaves Printing alone as
        // expected once real extrusion starts. FINISH/FAILED/IDLE are
        // already unambiguous, don't second-guess those.
        if (g_status.state == PrinterState::Calibrating || g_status.state == PrinterState::Printing) {
            PrinterState refined = mapStage(stage);
            if (refined != PrinterState::Unknown) g_status.state = refined;
        }
    }

    // gcode_state alone misses errors that happen outside an active print job
    // (e.g. a door/cover sensor tripping while idle) — print_error is Bambu's
    // general-purpose "something's wrong" flag and catches those too.
    // BUT: confirmed on real hardware that print_error stays non-zero (stale,
    // from the last failed/cancelled job) even after gcode_state genuinely
    // returns to IDLE — only a printer reboot cleared it. Trusting it
    // unconditionally kept the LED stuck red forever after any cancel. Treat a
    // fresh IDLE from gcode_state as authoritative and don't second-guess it.
    if (!print["print_error"].isNull()) {
        long printError = print["print_error"].as<long>();
        if (printError != 0 && g_status.gcodeState != "IDLE") {
            g_status.state = PrinterState::Error;
            if (printError != lastLoggedPrintError) {
                Log::add("[MQTT] print_error=%ld -> forcing state=Error", printError);
                lastLoggedPrintError = printError;
            }
        } else {
            lastLoggedPrintError = 0; // cleared — the next real error logs again even if it repeats a code
        }
    }
    // hms (Health Management System) codes carry mixed severities (notice/warning/
    // error/fatal) that aren't documented anywhere public yet, so we log them
    // instead of acting on them until we've seen real codes to classify. The
    // printer re-sends the same active codes on every report, so only log when
    // the set actually changes — otherwise this drowns out everything else.
    // The printer alternates between full and partial reports on the same
    // topic — a partial one simply omits "hms" entirely rather than sending
    // an empty array. isNull() catches both "absent" and "explicit null", so
    // only touch lastLoggedHms when the key actually exists in this message;
    // otherwise we'd flip-flop "cleared" on every other message.
    if (!print["hms"].isNull()) {
        JsonArray hms = print["hms"];
        if (hms.size() > 0) {
            String raw;
            serializeJson(hms, raw);
            if (raw != lastLoggedHms) {
                Log::add("[MQTT] hms -> %s", raw.c_str());
                lastLoggedHms = raw;
            }
        } else if (lastLoggedHms.length() > 0) {
            Log::add("[MQTT] hms -> (cleared)");
            lastLoggedHms = "";
        }
    }

    g_status.lastUpdateMs = millis();
    g_status.everConnected = true;

    if (g_status.state != lastLoggedState) {
        Log::add("[MQTT] state -> %s (gcode_state=\"%s\")", printerStateName(g_status.state), g_status.gcodeState.c_str());
        lastLoggedState = g_status.state;
    }
    if (g_status.state == PrinterState::Printing && g_status.percent != lastLoggedPercent) {
        Serial.printf("[MQTT] progress: %d%%\n", g_status.percent);
        lastLoggedPercent = g_status.percent;
    }
}

// Fully tears down the TLS + MQTT client objects (not just their sockets) so no
// stale mbedTLS session state can carry over into the next connection attempt.
static void releaseClients() {
    if (mqtt) { delete mqtt; mqtt = nullptr; }
    if (secureClient) { delete secureClient; secureClient = nullptr; }
}

static bool ensureClients() {
    if (mqtt && secureClient) return true;

    secureClient = new WiFiClientSecure();
    // The printer uses a self-signed certificate in LAN Mode (no public CA to
    // validate against). The connection is local, on your own network —
    // accepting it without validation is standard practice for LAN-mode MQTT
    // clients in this community.
    secureClient->setInsecure();

    mqtt = new PubSubClient(*secureClient);
    // Order matters here: a Reddit user with the exact same printer (P2S) and the
    // same "connects fine, most messages silently vanish" symptom fixed it by
    // calling setBufferSize() before setServer() (https://www.reddit.com/r/BambuLab/comments/1mi3a4x/).
    // Matching that exact order + a size at least as big as their working value (20000).
    if (!mqtt->setBufferSize(MQTT_BUFFER_SIZE)) {
        Serial.printf("[MQTT] setBufferSize(%u) failed — not enough heap\n", (unsigned)MQTT_BUFFER_SIZE);
        releaseClients();
        return false;
    }
    mqtt->setServer(g_ip.c_str(), MQTT_PORT);
    mqtt->setCallback(onMessage);
    return true;
}

static bool connectMqtt() {
    releaseClients(); // always start from a fresh TLS/MQTT object, never reuse one after a failure
    if (!ensureClients()) return false;

    Log::add("[MQTT] connecting to %s:%u as bblp...", g_ip.c_str(), MQTT_PORT);
    // Unique per attempt: a fixed client ID across rapid reconnects may make the
    // broker think the old session is still alive and never route us anything.
    String clientId = "bambuled-" + g_serial + "-" + String(millis());
    if (mqtt->connect(clientId.c_str(), "bblp", g_accessCode.c_str())) {
        bool subOk = mqtt->subscribe(g_reportTopic.c_str());
        Log::add("[MQTT] connected. subscribe(\"%s\") -> %s", g_reportTopic.c_str(), subOk ? "ok" : "FAILED");
        g_lastMessageMs = millis(); // start the "am I actually receiving anything" clock fresh
        return true;
    }
    Log::add("[MQTT] connect failed: state=%s", mqttStateName(mqtt->state()));
    return false;
}

void PrinterMqtt::begin(const String &ip, const String &serial, const String &accessCode) {
    g_ip = ip;
    g_serial = serial;
    g_accessCode = accessCode;
    g_reportTopic = "device/" + serial + "/report";

    connectMqtt();
}

void PrinterMqtt::loop() {
    if (g_ip.isEmpty()) return; // begin() hasn't been called yet (no printer configured)

    if (mqtt) mqtt->loop();

    // Shows how chatty the printer actually is, independent of whether the
    // state changed (our other logs only print on a change) — answers "does
    // it only send while printing?" empirically instead of guessing.
    static unsigned long lastMsgRateLogMs = 0;
    unsigned long nowRate = millis();
    if (nowRate - lastMsgRateLogMs > 15000) {
        Serial.printf("[MQTT] %u report(s) received in the last ~15s\n", (unsigned)g_messageCount);
        g_messageCount = 0;
        lastMsgRateLogMs = nowRate;
    }

    bool starved = mqtt && mqtt->connected() && (millis() - g_lastMessageMs > MESSAGE_TIMEOUT_MS);

    if (!mqtt || !mqtt->connected() || starved) {
        unsigned long now = millis();
        if (now - g_lastReconnectAttempt > RECONNECT_INTERVAL_MS) {
            g_lastReconnectAttempt = now;
            if (starved) {
                Log::add("[MQTT] no messages in %lus despite connected() == true (zombie connection) — forcing reconnect",
                         (millis() - g_lastMessageMs) / 1000);
            } else if (mqtt) {
                Log::add("[MQTT] disconnected, state=%s — reconnecting", mqttStateName(mqtt->state()));
            }
            connectMqtt();
        }
    }
}

bool PrinterMqtt::isConnected() { return mqtt && mqtt->connected(); }
PrinterStatus PrinterMqtt::getStatus() { return g_status; }
