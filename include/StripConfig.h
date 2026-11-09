#pragma once
#include <Arduino.h>

// Physical LED strip setup: data pin, how many LEDs, which chipset, which wire
// color order. Editable from Settings > LED strip and stored in NVS. Unlike the
// colors/brightness in AnimConfig, changing any of these reboots the device —
// the FastLED channel is created once at boot. LED_DATA_PIN (AppConfig.h /
// platformio.ini) is only the first-boot default pin.

enum class LedChipset : uint8_t {
    WS2812B = 0,
    WS2811,
    WS2811_400,
    WS2813,
    WS2815,
    SK6812,
    APA106,
    Count_ // keep last
};

// Order the strip expects the three channels on the wire.
enum class LedColorOrder : uint8_t {
    RGB = 0,
    RBG,
    GRB,
    GBR,
    BRG,
    BGR,
    Count_ // keep last
};

struct StripConfig {
    uint16_t count;
    LedChipset chipset;
    LedColorOrder order;
    uint8_t pin;
};

// A GPIO offered as the strip's data pin. `warning` is null for plain outputs;
// pins with a catch (e.g. a strapping pin that also drives the onboard LED)
// are offered with a warning the web UI shows before they're picked.
struct LedPinInfo {
    uint8_t pin;
    const char *warning;
};

// Data-pin choices on this chip: no flash/PSRAM, USB, UART0 or input-only
// pins; risky-but-usable pins only with a warning.
const LedPinInfo *ledAllowedPins(size_t &count);
bool ledPinAllowed(int pin);

const char *ledChipsetId(LedChipset c);           // "WS2812B", "WS2811_400", ... (API value)
const char *ledChipsetLabel(LedChipset c);        // human-readable, for the UI
LedColorOrder ledChipsetDefaultOrder(LedChipset c);
const char *ledColorOrderName(LedColorOrder o);   // "RGB", "GRB", ...
bool parseLedChipset(const char *s, LedChipset &out);
bool parseLedColorOrder(const char *s, LedColorOrder &out);

namespace StripConfigStore {
    // Falls back to defaults() if nothing (or something out of range) is stored.
    StripConfig load();
    void save(const StripConfig &cfg);
    StripConfig defaults();
}
