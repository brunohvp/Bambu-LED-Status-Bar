#include "LedAnimations.h"
#include <FastLED.h>
#include "AppConfig.h"
#include "StripConfig.h"
#include "NightMode.h"
#include "Log.h"

// Renders whatever StateAnimConfig the current printer state maps to (see
// AnimConfig.h). Only colors (1 or 2, per state) and one global brightness
// are user-editable from the web UI — effect choice and speed/intensity are
// fixed per state, hardcoded below. The effect *algorithms* (Fade/Loading/
// Percent/Plasmoid) are original FastLED reimplementations of the WLED
// effects this project used to run — see README.

// Data pin, LED count, chipset and color order come from StripConfig
// (Settings > LED strip), read once in begin() and handed to FastLED's runtime
// Channel API (FastLED.add(fl::ChannelConfig)) — addLeds<> would need all of
// them as compile-time template arguments.
static CRGB *leds = nullptr;
static uint16_t ledCount = 0;
static volatile bool blackoutRequested = false;
static bool blackoutShown = false;

static const uint16_t FRAME_INTERVAL_MS = 25; // ~40fps

static unsigned long lastFrameMs = 0;
static unsigned long idleSinceMs = 0;

// Confirmed on real hardware that the printer doesn't necessarily flip back
// to IDLE on its own once a finished print's part is removed — it may need
// an explicit ack on the printer's own screen, same class of issue as the
// stuck-FAILED state. Rather than wait on the printer, just stop *showing*
// Finished after a while and revert to Idle regardless (same idea BIQU's
// Panda Status uses — a fixed "done" window, not a real gcode_state check).
static const unsigned long FINISHED_DISPLAY_MS = 60UL * 1000UL;
static unsigned long finishedSinceMs = 0;

// g_cfg is written by reloadConfig() (called from the AsyncWebServer task when
// Settings are saved) and read by update() (on its own ledTask, see main.cpp)
// — different tasks, so a plain struct copy on either side could tear mid-copy
// and briefly render a color that's part old, part new. This spinlock is only
// ever held for a tiny, non-blocking struct copy, so it's cheap enough to wrap
// every frame.
static portMUX_TYPE g_cfgMux = portMUX_INITIALIZER_UNLOCKED;
static AnimConfigSet g_cfg;
static NightModeConfig g_night; // guarded by g_cfgMux too

// Night-mode window state, re-evaluated once a second (not every frame).
static volatile bool g_nightActive = false;
static unsigned long lastNightCheckMs = 0;
static PrinterState lastRenderedState = PrinterState::Unknown;
static bool everRenderedNormal = false;

static uint8_t speedToBpm(uint8_t sx) {
    // WLED's "speed" (0-255) isn't literally BPM; this just maps it to a
    // pleasant-looking animation rate.
    return map(sx, 0, 255, 4, 50);
}

static void fxFade(CRGB color, uint8_t speed, uint8_t brightness) {
    FastLED.setBrightness(brightness);
    uint8_t b = beatsin8(speedToBpm(speed), 15, 255);
    fill_solid(leds, ledCount, color);
    nscale8(leds, ledCount, b);
}

static void fxLoading(CRGB colorA, CRGB colorB, uint8_t speed, uint8_t intensity, uint8_t brightness) {
    FastLED.setBrightness(brightness);
    fadeToBlackBy(leds, ledCount, map(intensity, 0, 255, 80, 15)); // higher intensity = shorter trail
    uint8_t bpm = speedToBpm(speed);
    uint16_t pos = beatsin16(bpm, 0, ledCount - 1); // 16-bit: strips can be longer than 255 LEDs
    leds[pos] = blend(colorA, colorB, beatsin8(bpm / 2 + 1, 0, 255));
}

static void fxPercent(CRGB color, CRGB background, CRGB leadColor, int percent, uint8_t brightness) {
    FastLED.setBrightness(brightness);
    percent = constrain(percent, 0, 100);
    int lit = (percent * ledCount) / 100; // fully completed LEDs (with 10 LEDs: 0 at 0-9%, 1 at 10-19%, ...)
    for (int i = 0; i < ledCount; i++) {
        if (i < lit) {
            leds[i] = color; // done — solid
        } else if (i == lit) {
            // Currently-filling segment pulses instead of sitting static, so
            // "still working on this one" reads differently from "done" —
            // also what makes a fresh 0% print visibly alive, not just off.
            leds[i] = leadColor;
            leds[i].nscale8(beatsin8(30, 40, 255));
        } else {
            leds[i] = background;
        }
    }
}

static void fxPlasmoid(CRGB colorA, CRGB colorB, uint8_t speed, uint8_t brightness) {
    FastLED.setBrightness(brightness);
    uint8_t basePhase = beat8(speedToBpm(speed) / 2 + 1);
    for (int i = 0; i < ledCount; i++) {
        uint8_t phaseOffset = (uint8_t)((i * 256) / ledCount);
        uint8_t wave = sin8(basePhase + phaseOffset);
        leds[i] = blend(colorA, colorB, wave);
    }
}

static void renderEffect(const StateAnimConfig &c, int percent, uint8_t speed, uint8_t intensity, uint8_t brightness) {
    CRGB c1(c.color1.r, c.color1.g, c.color1.b);
    CRGB c2(c.color2.r, c.color2.g, c.color2.b);

    switch (c.effect) {
        case LedEffect::Off:
            FastLED.setBrightness(0);
            fill_solid(leds, ledCount, CRGB::Black);
            break;
        case LedEffect::Solid:
            FastLED.setBrightness(brightness);
            fill_solid(leds, ledCount, c1);
            break;
        case LedEffect::Fade:
            fxFade(c1, speed, brightness);
            break;
        case LedEffect::Loading:
            fxLoading(c1, c2, speed, intensity, brightness);
            break;
        case LedEffect::Percent:
            // Background is always off — only the bar color (c1) and the
            // leading-pixel color (c2) are user-editable for Printing.
            fxPercent(c1, CRGB::Black, c2, percent, brightness);
            break;
        case LedEffect::Plasmoid:
            fxPlasmoid(c1, c2, speed, brightness);
            break;
    }
}

// Same timings the matching addLeds<CHIPSET> controllers use (see chipsets.h).
static fl::ClocklessChipset clocklessFor(LedChipset chipset, int pin) {
    switch (chipset) {
        case LedChipset::WS2811:     return fl::makeClockless<fl::TIMING_WS2811_800KHZ_LEGACY>(pin);
        case LedChipset::WS2811_400: return fl::makeClockless<fl::TIMING_WS2811_400KHZ>(pin);
        case LedChipset::WS2813:     return fl::makeClockless<fl::TIMING_WS2813>(pin);
        case LedChipset::WS2815:     return fl::makeClockless<fl::TIMING_WS2815>(pin);
        case LedChipset::SK6812:     return fl::makeClockless<fl::TIMING_SK6812>(pin);
        case LedChipset::APA106:     return fl::makeClockless<fl::TIMING_SK6822>(pin); // FastLED's APA106 = SK6822 timing
        case LedChipset::WS2812B:
        default:                     return fl::makeClockless<fl::TIMING_WS2812_800KHZ>(pin);
    }
}

static EOrder eorderFor(LedColorOrder order) {
    switch (order) {
        case LedColorOrder::RBG: return RBG;
        case LedColorOrder::GRB: return GRB;
        case LedColorOrder::GBR: return GBR;
        case LedColorOrder::BRG: return BRG;
        case LedColorOrder::BGR: return BGR;
        case LedColorOrder::RGB:
        default:                 return RGB;
    }
}

static void showFrame() {
    FastLED.show();
}

void LedAnimations::begin() {
    StripConfig strip = StripConfigStore::load();
    ledCount = strip.count;
    leds = new CRGB[ledCount]; // lives for the whole run: the channel keeps a span over it
    fl::ChannelConfig config(clocklessFor(strip.chipset, strip.pin), fl::span<CRGB>(leds, ledCount),
                             eorderFor(strip.order));
    if (FastLED.add(config) == nullptr) {
        Log::add("[LED] FastLED could not create the strip on GPIO%u", strip.pin);
    }
    FastLED.setBrightness(LED_MAX_BRIGHTNESS);
    Log::add("[LED] strip: %u x %s, order %s, GPIO%u", ledCount, ledChipsetId(strip.chipset),
             ledColorOrderName(strip.order), strip.pin);
    // Deliberately no fill_solid()/show() here — the strip has its own power
    // and already holds whatever it displayed before this boot; blacking it
    // out on every reboot was the actual source of the "flash" (reboots are
    // frequent, thanks to the known TLS instability). Leave it alone until
    // update() has real data to render.
    reloadConfig();
}

void LedAnimations::reloadConfig() {
    AnimConfigSet loaded = AnimConfigStore::load(); // NVS I/O stays outside the lock
    NightModeConfig night = NightModeStore::load();
    portENTER_CRITICAL(&g_cfgMux);
    g_cfg = loaded;
    g_night = night;
    portEXIT_CRITICAL(&g_cfgMux);
    lastNightCheckMs = 0; // re-evaluate the night window on the next frame
}

bool LedAnimations::nightActive() {
    return g_nightActive;
}

// Is the optional night-mode window active right now? Off whenever the clock
// isn't synced yet, so the strip falls back to its normal brightness.
static void updateNightActive(unsigned long nowMs, const NightModeConfig &night) {
    if (lastNightCheckMs != 0 && nowMs - lastNightCheckMs < 1000) return;
    lastNightCheckMs = nowMs;
    uint16_t minutes;
    bool active = night.enabled && Clock::localMinutes(minutes) && nightWindowContains(night, minutes);
    if (active != g_nightActive) {
        g_nightActive = active;
        Log::add("[LED] night mode %s", active ? "on" : "off");
    }
}

void LedAnimations::blackout() {
    blackoutRequested = true;
}

void LedAnimations::update(PrinterState state, int percent, bool chamberLightOn) {
    if (!leds) return; // begin() not called yet
    unsigned long now = millis();
    if (now - lastFrameMs < FRAME_INTERVAL_MS) return;
    lastFrameMs = now;

    // About to reboot with a new strip config: clear every LED the current
    // config drives, so LEDs past a smaller new count don't stay lit with
    // their last frame (the strip has its own power and holds it otherwise).
    if (blackoutRequested) {
        if (!blackoutShown) {
            fill_solid(leds, ledCount, CRGB::Black);
            showFrame();
            blackoutShown = true;
        }
        return;
    }

    // Not a real printer state (haven't heard from it yet, or lost the
    // connection). Used to flash a dedicated blue<->red indicator here, but
    // confirmed on real hardware that made every brief reboot (frequent,
    // thanks to the known TLS instability) look like a jarring glitch. The
    // strip has its own power and already holds its last frame on its own,
    // so simplest is best: just don't touch it and let it sit on whatever it
    // last showed until a real state comes in.
    if (state == PrinterState::Unknown) {
        if (state != lastRenderedState) {
            Serial.println("[LED] not connected yet — holding last frame");
            lastRenderedState = state;
        }
        return;
    }

    if (state == PrinterState::Finished) {
        if (finishedSinceMs == 0) finishedSinceMs = now;
        if (now - finishedSinceMs > FINISHED_DISPLAY_MS) state = PrinterState::Idle;
    } else {
        finishedSinceMs = 0;
    }

    if (state == PrinterState::Idle) {
        if (idleSinceMs == 0) idleSinceMs = now;
    } else {
        idleSinceMs = 0;
    }
    bool sleeping = (state == PrinterState::Idle) && idleSinceMs != 0 &&
                    (now - idleSinceMs > LED_IDLE_TO_SLEEP_MS);

    // Speed/intensity are fixed per state, hand-picked to fit each animation
    // (Percent and Solid ignore both anyway). Error is the urgent end.
    StateAnimConfig c{};
    uint8_t speed = 128, intensity = 128, brightness;
    portENTER_CRITICAL(&g_cfgMux);
    switch (state) {
        case PrinterState::Idle: c = g_cfg.idle; speed = 134; break; // matches the original WLED preset's sx
        case PrinterState::Calibrating: c = g_cfg.calibrating; speed = 130; intensity = 100; break; // covers heating too
        case PrinterState::Printing: c = g_cfg.printing; break;
        case PrinterState::Paused: c = g_cfg.paused; speed = 60; break;
        case PrinterState::Finished: c = g_cfg.finished; speed = 60; break; // same as Paused, just green
        case PrinterState::Error: c = g_cfg.error; speed = 255; break;
        default:
            break; // unreachable — Unknown returns early above
    }
    brightness = g_cfg.brightness;
    NightModeConfig night = g_night;
    portEXIT_CRITICAL(&g_cfgMux);

    // Optional night mode: inside its time window the night brightness
    // replaces the normal one; the dimming rules below still stack on top.
    updateNightActive(now, night);
    if (g_nightActive) brightness = night.brightness;

    // The printer's own chamber light being off is a decent signal nobody's
    // actively watching it — cap at a dim ~10% regardless of state,
    // independent of (and stacking with) the longer-timeout "sleeping" dim below.
    // A cap, not a fixed value, so an even dimmer night brightness wins.
    if (!chamberLightOn && brightness > 26) brightness = 26;
    if (sleeping) brightness = brightness / 5;

    if (!everRenderedNormal || state != lastRenderedState) {
        Serial.printf("[LED] normal render: state=%s effect=%s color1=#%02X%02X%02X brightness=%u\n",
                      printerStateName(state), ledEffectName(c.effect), c.color1.r, c.color1.g, c.color1.b, brightness);
        lastRenderedState = state;
        everRenderedNormal = true;
    }

    renderEffect(c, percent, speed, intensity, brightness);
    showFrame();
}
