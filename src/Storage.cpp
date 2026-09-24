#include "Storage.h"
#include <Preferences.h>

static const char *NS = "bambuled";

AppState Storage::load() {
    Preferences prefs;
    AppState s;
    prefs.begin(NS, true); // read-only
    s.wifiSsid          = prefs.getString("wifi_ssid", "");
    s.wifiPass           = prefs.getString("wifi_pass", "");
    s.printerIp          = prefs.getString("p_ip", "");
    s.printerSerial       = prefs.getString("p_serial", "");
    s.printerAccessCode   = prefs.getString("p_code", "");
    prefs.end();
    return s;
}

void Storage::saveAll(const String &ssid, const String &pass,
                       const String &printerIp, const String &printerSerial, const String &printerAccessCode) {
    Preferences prefs;
    prefs.begin(NS, false);
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);
    prefs.putString("p_ip", printerIp);
    prefs.putString("p_serial", printerSerial);
    prefs.putString("p_code", printerAccessCode);
    prefs.end();
}

void Storage::clearWifi() {
    Preferences prefs;
    prefs.begin(NS, false);
    prefs.remove("wifi_ssid");
    prefs.remove("wifi_pass");
    prefs.end();
}

void Storage::clearAll() {
    Preferences prefs;
    prefs.begin(NS, false);
    prefs.clear();
    prefs.end();
}
