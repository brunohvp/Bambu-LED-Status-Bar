#include "LedAnimations.h"
#include <FastLED.h>
#include "AppConfig.h"

// Renders whatever StateAnimConfig the current printer state maps to (see
// AnimConfig.h). Only colors (1 or 2, per state) and one global brightness
// are user-editable from the web UI — effect choice and speed/intensity are
// fixed per state, hardcoded below. The effect *algorithms* (Fade/Loading/
// Percent/Plasmoid) are original FastLED reimplementations of the WLED
// effects this project used to run — see README.

static CRGB leds[LED_COUNT];
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
    fill_solid(leds, LED_COUNT, color);
    nscale8(leds, LED_COUNT, b);
}

static void fxLoading(CRGB colorA, CRGB colorB, uint8_t speed, uint8_t intensity, uint8_t brightness) {
    FastLED.setBrightness(brightness);
    fadeToBlackBy(leds, LED_COUNT, map(intensity, 0, 255, 80, 15)); // higher intensity = shorter trail
    uint8_t bpm = speedToBpm(speed);
    uint8_t pos = beatsin8(bpm, 0, LED_COUNT - 1);
    leds[pos] = blend(colorA, colorB, beatsin8(bpm / 2 + 1, 0, 255));
}

static void fxPercent(CRGB color, CRGB background, CRGB leadColor, int percent, uint8_t brightness) {
    FastLED.setBrightness(brightness);
    percent = constrain(percent, 0, 100);
    int lit = (percent * LED_COUNT) / 100; // fully completed segments (0 at 0-9%, 1 at 10-19%, ...)
    for (int i = 0; i < LED_COUNT; i++) {
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
    for (int i = 0; i < LED_COUNT; i++) {
        uint8_t phaseOffset = (uint8_t)((i * 256) / LED_COUNT);
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
            fill_solid(leds, LED_COUNT, CRGB::Black);
            break;
        case LedEffect::Solid:
            FastLED.setBrightness(brightness);
            fill_solid(leds, LED_COUNT, c1);
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

void LedAnimations::begin() {
    FastLED.addLeds<WS2811, LED_DATA_PIN, LED_COLOR_ORDER>(leds, LED_COUNT);
    FastLED.setBrightness(LED_MAX_BRIGHTNESS);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();
    reloadConfig();
}

void LedAnimations::reloadConfig() {
    AnimConfigSet loaded = AnimConfigStore::load(); // NVS I/O stays outside the lock
    portENTER_CRITICAL(&g_cfgMux);
    g_cfg = loaded;
    portEXIT_CRITICAL(&g_cfgMux);
}

void LedAnimations::update(PrinterState state, int percent, bool chamberLightOn) {
    unsigned long now = millis();
    if (now - lastFrameMs < FRAME_INTERVAL_MS) return;
    lastFrameMs = now;

    // Not a real printer state (haven't heard from it yet, or lost the
    // connection) — just the first pixel fading blue<->red, rest off, so it
    // reads as "not connected" at a glance instead of looking like any other
    // configured state.
    if (state == PrinterState::Unknown) {
        FastLED.setBrightness(chamberLightOn ? g_cfg.brightness : 26);
        fill_solid(leds, LED_COUNT, CRGB::Black);
        leds[0] = blend(CRGB::Blue, CRGB::Red, beatsin8(15, 0, 255));
        if (!everRenderedNormal || state != lastRenderedState) {
            Serial.println("[LED] normal render: state=unknown (disconnected indicator)");
            lastRenderedState = state;
            everRenderedNormal = true;
        }
        FastLED.show();
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
    portEXIT_CRITICAL(&g_cfgMux);

    // The printer's own chamber light being off is a decent signal nobody's
    // actively watching it — drop to a dim ~10% regardless of state,
    // independent of (and stacking with) the longer-timeout "sleeping" dim below.
    if (!chamberLightOn) brightness = 26;
    if (sleeping) brightness = brightness / 5;

    if (!everRenderedNormal || state != lastRenderedState) {
        Serial.printf("[LED] normal render: state=%s effect=%s color1=#%02X%02X%02X brightness=%u\n",
                      printerStateName(state), ledEffectName(c.effect), c.color1.r, c.color1.g, c.color1.b, brightness);
        lastRenderedState = state;
        everRenderedNormal = true;
    }

    renderEffect(c, percent, speed, intensity, brightness);
    FastLED.show();
}
