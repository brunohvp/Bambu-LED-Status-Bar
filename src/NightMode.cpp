#include "NightMode.h"
#include <Preferences.h>
#include <time.h>

static const char *NS = "bambuled"; // same namespace as Storage, so Factory reset clears it too
static const char *KEY_ENABLED = "nm_on";
static const char *KEY_START = "nm_start";
static const char *KEY_END = "nm_end";
static const char *KEY_BRIGHTNESS = "nm_bri";
static const char *KEY_TZ = "nm_tz";

// Index 0 is the default. Append new zones at the end — the saved setting is an index.
static const TimeZoneInfo ZONES[] = {
    {"Central Europe (Budapest, Berlin, Paris)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"UK, Ireland, Portugal (London, Lisbon)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Eastern Europe (Athens, Bucharest, Helsinki)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow, Istanbul", "MSK-3"},
    {"UTC", "UTC0"},
    {"US Eastern (New York)", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central (Chicago)", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain (Denver)", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Pacific (Los Angeles)", "PST8PDT,M3.2.0,M11.1.0"},
    {"Japan (Tokyo)", "JST-9"},
    {"Australia East (Sydney)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};
static const size_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

const TimeZoneInfo *timeZones(size_t &count) {
    count = ZONE_COUNT;
    return ZONES;
}

bool nightWindowContains(const NightModeConfig &cfg, uint16_t m) {
    if (cfg.startMin == cfg.endMin) return false;
    if (cfg.startMin < cfg.endMin) return m >= cfg.startMin && m < cfg.endMin;
    return m >= cfg.startMin || m < cfg.endMin; // wraps past midnight, e.g. 22:00-07:00
}

NightModeConfig NightModeStore::defaults() {
    return NightModeConfig{false, 22 * 60, 7 * 60, 20, 0}; // off; 22:00-07:00 at ~8%
}

NightModeConfig NightModeStore::load() {
    NightModeConfig d = defaults();
    Preferences prefs;
    prefs.begin(NS, true);
    NightModeConfig c;
    c.enabled = prefs.getBool(KEY_ENABLED, d.enabled);
    c.startMin = prefs.getUShort(KEY_START, d.startMin);
    c.endMin = prefs.getUShort(KEY_END, d.endMin);
    c.brightness = prefs.getUChar(KEY_BRIGHTNESS, d.brightness);
    c.tz = prefs.getUChar(KEY_TZ, d.tz);
    prefs.end();

    if (c.startMin >= 24 * 60) c.startMin = d.startMin;
    if (c.endMin >= 24 * 60) c.endMin = d.endMin;
    if (c.tz >= ZONE_COUNT) c.tz = d.tz;
    return c;
}

void NightModeStore::save(const NightModeConfig &c) {
    Preferences prefs;
    prefs.begin(NS, false);
    prefs.putBool(KEY_ENABLED, c.enabled);
    prefs.putUShort(KEY_START, c.startMin);
    prefs.putUShort(KEY_END, c.endMin);
    prefs.putUChar(KEY_BRIGHTNESS, c.brightness);
    prefs.putUChar(KEY_TZ, c.tz);
    prefs.end();
}

void Clock::begin(uint8_t tzIndex) {
    if (tzIndex >= ZONE_COUNT) tzIndex = 0;
    configTzTime(ZONES[tzIndex].posix, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

void Clock::setZone(uint8_t tzIndex) {
    if (tzIndex >= ZONE_COUNT) tzIndex = 0;
    setenv("TZ", ZONES[tzIndex].posix, 1);
    tzset();
}

bool Clock::synced() {
    return time(nullptr) > 1700000000; // anything before Nov 2023 means NTP hasn't answered yet
}

bool Clock::localMinutes(uint16_t &minutesOfDay) {
    if (!synced()) return false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    minutesOfDay = (uint16_t)(t.tm_hour * 60 + t.tm_min);
    return true;
}

String Clock::localTimeString() {
    if (!synced()) return String();
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &t);
    return String(buf);
}
