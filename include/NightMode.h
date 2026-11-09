#pragma once
#include <Arduino.h>

// Optional time-of-day brightness ("night mode"): between startMin and endMin
// (minutes after local midnight; the window may wrap past midnight) the strip
// runs at `brightness` instead of the normal one. Editable from Settings >
// Night Mode, applied live. Needs the clock from NTP — until it's synced the
// strip simply stays at the normal (day) brightness.
struct NightModeConfig {
    bool enabled;
    uint16_t startMin;  // 0-1439
    uint16_t endMin;    // 0-1439
    uint8_t brightness; // 0-255
    uint8_t tz;         // index into timeZones()
};

struct TimeZoneInfo {
    const char *label;
    const char *posix; // POSIX TZ string (handles daylight saving on its own)
};
const TimeZoneInfo *timeZones(size_t &count);

// True if minutesOfDay falls inside the configured window (start inclusive,
// end exclusive). start == end means an empty window.
bool nightWindowContains(const NightModeConfig &cfg, uint16_t minutesOfDay);

namespace NightModeStore {
    NightModeConfig load(); // falls back to defaults() for anything missing/out of range
    void save(const NightModeConfig &cfg);
    NightModeConfig defaults();
}

namespace Clock {
    void begin(uint8_t tzIndex);     // starts NTP and sets the zone — call once WiFi is up
    void setZone(uint8_t tzIndex);
    bool synced();
    bool localMinutes(uint16_t &minutesOfDay); // false until synced
    String localTimeString();                  // "2026-11-09 14:05", or "" until synced
}
