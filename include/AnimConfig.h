#pragma once
#include <Arduino.h>

// Which animation algorithm each state uses — fixed per state (see
// AnimConfigStore::defaults()), not user-selectable. Speed/intensity are
// likewise fixed, hand-picked per state in LedAnimations.cpp; only colors and
// one global brightness are user-editable from the web UI.
enum class LedEffect : uint8_t {
    Off = 0,
    Solid,
    Fade,
    Loading,
    Percent,
    Plasmoid,
};

const char *ledEffectName(LedEffect e); // "Off", "Solid", "Fade", ...

struct RgbColor {
    uint8_t r, g, b;
};

// color2 is only meaningful for the two-color states (Idle's ambient blend,
// Printing's progress-bar/leading-pixel pair) — single-color states just
// leave it unused.
struct StateAnimConfig {
    LedEffect effect;
    RgbColor color1;
    RgbColor color2;
};

// One config per state shown in the UI, plus a single brightness that applies
// to all of them. "Sleep" isn't a separate slot — it reuses Idle at reduced
// brightness after a timeout (see LED_IDLE_TO_SLEEP_MS).
struct AnimConfigSet {
    StateAnimConfig idle;        // 2 colors (Plasmoid blend)
    StateAnimConfig calibrating; // 1 color (Loading) — covers heating too
    StateAnimConfig printing;    // 2 colors (bar + leading pixel)
    StateAnimConfig paused;      // 1 color
    StateAnimConfig finished;    // 1 color
    StateAnimConfig error;       // 1 color, fast
    uint8_t brightness;          // 0-255, applies to the whole strip regardless of state
};

namespace AnimConfigStore {
    // Loads from NVS; if nothing is stored yet (or the stored blob doesn't match
    // the current struct layout, e.g. after a firmware update), returns defaults().
    AnimConfigSet load();

    void save(const AnimConfigSet &cfg);

    // The factory presets — ported from the original WLED presets.json this
    // project started from (see README > Phase 3).
    AnimConfigSet defaults();
}
