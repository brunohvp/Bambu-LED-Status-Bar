#pragma once
#include <Arduino.h>
#include <vector>

struct DiscoveredPrinter {
    String ip;
    String serial;
    String model;
    String name;
    bool lanMode;
};

namespace PrinterDiscovery {
    // Listens for Bambu Lab's SSDP-like broadcast for `durationMs` and returns
    // whatever printers answered. Only works once the device is joined to the
    // same LAN as the printer (i.e. after a successful WiFi connection) —
    // scanning from the setup AP's own subnet will never find anything.
    std::vector<DiscoveredPrinter> scan(unsigned long durationMs = 4000);
}
