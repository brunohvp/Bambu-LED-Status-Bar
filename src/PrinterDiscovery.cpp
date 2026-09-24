#include "PrinterDiscovery.h"
#include <WiFiUdp.h>

// Bambu Lab printers broadcast a non-standard SSDP NOTIFY roughly every 5s to
// the multicast group 239.255.255.250, with the destination UDP port
// alternating between 1990 and 2021. Example payload:
//
//   NOTIFY * HTTP/1.1
//   HOST: 239.255.255.250:1900
//   Server: UPnP/1.0
//   Location: 192.168.3.8
//   NT: urn:bambulab-com:device:3dprinter:1
//   USN: <serial>
//   DevModel.bambu.com: C12
//   DevName.bambu.com: My Printer
//   DevConnect.bambu.com: lan
//
// Reference: https://gist.github.com/Alex-Schaefer/72a9e2491a42da2ef99fb87601955cc3
static const IPAddress BAMBU_MCAST_ADDR(239, 255, 255, 250);
static const uint16_t BAMBU_SSDP_PORTS[2] = {1990, 2021};

static String extractHeader(const String &msg, const char *header) {
    String key = String(header) + ":";
    int idx = msg.indexOf(key);
    if (idx < 0) return "";
    int start = idx + key.length();
    int end = msg.indexOf('\r', start);
    if (end < 0) end = msg.indexOf('\n', start);
    if (end < 0) end = msg.length();
    String val = msg.substring(start, end);
    val.trim();
    return val;
}

std::vector<DiscoveredPrinter> PrinterDiscovery::scan(unsigned long durationMs) {
    std::vector<DiscoveredPrinter> found;

    WiFiUDP sockets[2];
    bool open[2];
    for (int i = 0; i < 2; i++) {
        open[i] = sockets[i].beginMulticast(BAMBU_MCAST_ADDR, BAMBU_SSDP_PORTS[i]);
    }

    char buf[1200];
    unsigned long start = millis();
    while (millis() - start < durationMs) {
        for (int i = 0; i < 2; i++) {
            if (!open[i]) continue;
            int sz = sockets[i].parsePacket();
            if (sz <= 0) continue;
            int len = sockets[i].read(buf, sizeof(buf) - 1);
            if (len <= 0) continue;
            buf[len] = 0;
            String msg(buf);
            if (msg.indexOf("bambulab") < 0 && msg.indexOf("NOTIFY") < 0) continue;

            DiscoveredPrinter p;
            p.ip = extractHeader(msg, "Location");
            p.serial = extractHeader(msg, "USN");
            p.model = extractHeader(msg, "DevModel.bambu.com");
            p.name = extractHeader(msg, "DevName.bambu.com");
            String conn = extractHeader(msg, "DevConnect.bambu.com");
            p.lanMode = conn.indexOf("lan") >= 0;

            if (p.ip.isEmpty() || p.serial.isEmpty()) continue;

            bool dup = false;
            for (auto &e : found) {
                if (e.serial == p.serial) { dup = true; break; }
            }
            if (!dup) found.push_back(p);
        }
        delay(20);
    }

    for (int i = 0; i < 2; i++) {
        if (open[i]) sockets[i].stop();
    }
    return found;
}
