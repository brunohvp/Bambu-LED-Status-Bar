#include "StripConfig.h"
#include <Preferences.h>
#include "AppConfig.h"

static const char *NS = "bambuled"; // same namespace as Storage, so Factory reset clears it too
static const char *KEY_COUNT = "led_count";
static const char *KEY_CHIPSET = "led_chip";
static const char *KEY_ORDER = "led_order";
static const char *KEY_PIN = "led_pin";

// Data-pin choices per chip (see ledAllowedPins() in StripConfig.h).
#if CONFIG_IDF_TARGET_ESP32C3
// Excluded: 2/9 strapping (9 = BOOT button), 11-17 flash, 18/19 USB
// (flashing + serial), 20/21 UART0 (the ROM boot log on 21 would flicker the
// strip). 8 is offered with a warning.
static const LedPinInfo PIN_CHOICES[] = {
    {0, nullptr}, {1, nullptr}, {3, nullptr}, {4, nullptr}, {5, nullptr}, {6, nullptr}, {7, nullptr},
    {8, "GPIO8 is a boot strapping pin and the onboard LED on most ESP32-C3 boards. "
        "The onboard status LED is switched off while the strip uses it. It works once the board "
        "has booted, but if flashing over USB ever fails, unplug the strip's data wire and try again."},
    {10, nullptr},
};
#elif CONFIG_IDF_TARGET_ESP32
// Excluded: 0/2/12/15 strapping (12 also sets the flash voltage), 1/3 UART0,
// 6-11 flash, 34-39 input-only.
static const LedPinInfo PIN_CHOICES[] = {
    {4, nullptr}, {5, nullptr}, {13, nullptr}, {14, nullptr}, {16, nullptr}, {17, nullptr},
    {18, nullptr}, {19, nullptr}, {21, nullptr}, {22, nullptr}, {23, nullptr}, {25, nullptr},
    {26, nullptr}, {27, nullptr}, {32, nullptr}, {33, nullptr},
};
#else
// Other chips: only the build-time pin until someone vets a list for them.
static const LedPinInfo PIN_CHOICES[] = {{LED_DATA_PIN, nullptr}};
#endif

const LedPinInfo *ledAllowedPins(size_t &count) {
    count = sizeof(PIN_CHOICES) / sizeof(PIN_CHOICES[0]);
    return PIN_CHOICES;
}

bool ledPinAllowed(int pin) {
    for (const LedPinInfo &p : PIN_CHOICES) {
        if (p.pin == pin) return true;
    }
    return false;
}

struct ChipsetInfo {
    const char *id;
    const char *label;
    LedColorOrder defaultOrder;
};

// Indexed by LedChipset.
static const ChipsetInfo CHIPSETS[] = {
    {"WS2812B",    "WS2812B / NeoPixel (5V)",      LedColorOrder::GRB},
    {"WS2811",     "WS2811 (800 kHz)",             LedColorOrder::RGB},
    {"WS2811_400", "WS2811 (400 kHz, older)",      LedColorOrder::RGB},
    {"WS2813",     "WS2813 (5V, backup data line)", LedColorOrder::GRB},
    {"WS2815",     "WS2815 (12V)",                 LedColorOrder::GRB},
    {"SK6812",     "SK6812 (RGB)",                 LedColorOrder::GRB},
    {"APA106",     "APA106 / PL9823",              LedColorOrder::RGB},
};
static_assert(sizeof(CHIPSETS) / sizeof(CHIPSETS[0]) == (size_t)LedChipset::Count_,
              "CHIPSETS table must match the LedChipset enum");

// Indexed by LedColorOrder.
static const char *ORDER_NAMES[] = {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR"};
static_assert(sizeof(ORDER_NAMES) / sizeof(ORDER_NAMES[0]) == (size_t)LedColorOrder::Count_,
              "ORDER_NAMES table must match the LedColorOrder enum");

static bool validChipset(uint8_t v) { return v < (uint8_t)LedChipset::Count_; }
static bool validOrder(uint8_t v) { return v < (uint8_t)LedColorOrder::Count_; }

const char *ledChipsetId(LedChipset c) {
    return validChipset((uint8_t)c) ? CHIPSETS[(uint8_t)c].id : CHIPSETS[0].id;
}

const char *ledChipsetLabel(LedChipset c) {
    return validChipset((uint8_t)c) ? CHIPSETS[(uint8_t)c].label : CHIPSETS[0].label;
}

LedColorOrder ledChipsetDefaultOrder(LedChipset c) {
    return validChipset((uint8_t)c) ? CHIPSETS[(uint8_t)c].defaultOrder : LedColorOrder::GRB;
}

const char *ledColorOrderName(LedColorOrder o) {
    return validOrder((uint8_t)o) ? ORDER_NAMES[(uint8_t)o] : ORDER_NAMES[0];
}

bool parseLedChipset(const char *s, LedChipset &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)LedChipset::Count_; i++) {
        if (strcasecmp(s, CHIPSETS[i].id) == 0) {
            out = (LedChipset)i;
            return true;
        }
    }
    return false;
}

bool parseLedColorOrder(const char *s, LedColorOrder &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)LedColorOrder::Count_; i++) {
        if (strcasecmp(s, ORDER_NAMES[i]) == 0) {
            out = (LedColorOrder)i;
            return true;
        }
    }
    return false;
}

StripConfig StripConfigStore::defaults() {
    return StripConfig{LED_DEFAULT_COUNT, LED_DEFAULT_CHIPSET, LED_DEFAULT_COLOR_ORDER, LED_DATA_PIN};
}

StripConfig StripConfigStore::load() {
    StripConfig d = defaults();
    Preferences prefs;
    prefs.begin(NS, true);
    uint16_t count = prefs.getUShort(KEY_COUNT, d.count);
    uint8_t chip = prefs.getUChar(KEY_CHIPSET, (uint8_t)d.chipset);
    uint8_t order = prefs.getUChar(KEY_ORDER, (uint8_t)d.order);
    uint8_t pin = prefs.getUChar(KEY_PIN, d.pin);
    prefs.end();

    StripConfig cfg = d;
    if (count >= 1 && count <= LED_MAX_COUNT) cfg.count = count;
    if (validChipset(chip)) cfg.chipset = (LedChipset)chip;
    if (validOrder(order)) cfg.order = (LedColorOrder)order;
    if (ledPinAllowed(pin)) cfg.pin = pin;
    return cfg;
}

void StripConfigStore::save(const StripConfig &cfg) {
    Preferences prefs;
    prefs.begin(NS, false);
    prefs.putUShort(KEY_COUNT, cfg.count);
    prefs.putUChar(KEY_CHIPSET, (uint8_t)cfg.chipset);
    prefs.putUChar(KEY_ORDER, (uint8_t)cfg.order);
    prefs.putUChar(KEY_PIN, cfg.pin);
    prefs.end();
}
