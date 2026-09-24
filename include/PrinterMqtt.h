#pragma once
#include <Arduino.h>
#include "PrinterStatus.h"

namespace PrinterMqtt {
    // Connects (MQTTS, port 8883, user "bblp") and subscribes to device/<serial>/report.
    // Call once, after already joined to the printer's network (station mode).
    void begin(const String &ip, const String &serial, const String &accessCode);

    // Pumps the MQTT client (PubSubClient) and reconnects on its own every 5s
    // if it drops. Call this on every loop().
    void loop();

    bool isConnected();
    PrinterStatus getStatus();
}
