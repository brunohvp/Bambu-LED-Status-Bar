#pragma once
#include <Arduino.h>

struct AppState {
    // WiFi
    String wifiSsid;
    String wifiPass;

    // Bambu Lab printer (LAN Mode)
    String printerIp;
    String printerSerial;
    String printerAccessCode;

    bool hasWifi() const { return wifiSsid.length() > 0; }
    bool hasPrinter() const {
        return printerIp.length() > 0 && printerSerial.length() > 0 && printerAccessCode.length() > 0;
    }
};

namespace Storage {
    // Loads whatever is saved in NVS. Missing fields come back as an empty string.
    AppState load();

    // Saves WiFi + printer together (the setup wizard's flow).
    void saveAll(const String &ssid, const String &pass,
                 const String &printerIp, const String &printerSerial, const String &printerAccessCode);

    // Erases only the WiFi credentials (keeps the printer config).
    void clearWifi();

    // Erases everything (factory reset).
    void clearAll();
}
