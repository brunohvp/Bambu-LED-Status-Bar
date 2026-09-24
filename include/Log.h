#pragma once
#include <Arduino.h>

// A small in-RAM ring buffer of recent, high-value log lines (mirrored to
// Serial). Lets the Settings page show what the firmware is doing without a
// USB cable — most users debugging a WLED/BIQU-style status bar don't have
// PlatformIO installed. Intentionally selective: only connection lifecycle,
// state changes and errors go through here, not the high-frequency per-report
// spam (that stays Serial-only, see PrinterMqtt.cpp).
namespace Log {
    void add(const char *fmt, ...);

    // Oldest first, one line per '\n'. Cleared on reboot (RAM only).
    String recent();
}
