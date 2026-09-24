#include "AnimConfig.h"
#include <Preferences.h>

static const char *NS = "bambuled";
static const char *KEY = "anim_v2"; // bump this (v3, v4, ...) if the struct layout ever changes

const char *ledEffectName(LedEffect e) {
    switch (e) {
        case LedEffect::Off: return "Off";
        case LedEffect::Solid: return "Solid";
        case LedEffect::Fade: return "Fade";
        case LedEffect::Loading: return "Loading";
        case LedEffect::Percent: return "Percent";
        case LedEffect::Plasmoid: return "Plasmoid";
        case LedEffect::Bounce: return "Bounce";
        default: return "Solid";
    }
}

AnimConfigSet AnimConfigStore::defaults() {
    AnimConfigSet d;
    // Colors ported from the original WLED presets.json for this project.
    // Effect choice per state (and speed/intensity, hardcoded in
    // LedAnimations.cpp) isn't user-editable — see AnimConfig.h.
    // {133,245,255}/{0,0,255} match the original WLED preset's col[0]/col[1]
    // exactly (fx:133 "Plasma" -> our Plasmoid, sx:134 -> speed in LedAnimations.cpp).
    d.idle        = { LedEffect::Plasmoid, {133, 245, 255}, {0, 0, 255} };
    d.heating     = { LedEffect::Loading,  {255, 0, 0},   {255, 0, 0}   };
    d.calibrating = { LedEffect::Bounce,   {0, 191, 255}, {0, 191, 255} }; // WLED fx:158 GRAVFREQ -> our Bounce (no-mic replacement)
    d.printing    = { LedEffect::Percent,  {0, 174, 239}, {255, 255, 255} };
    d.paused      = { LedEffect::Fade,     {255, 170, 0}, {255, 170, 0} };
    d.finished    = { LedEffect::Fade,     {0, 255, 68},  {0, 255, 68}  }; // same as Paused, just green
    d.error       = { LedEffect::Fade,     {255, 0, 0},   {255, 0, 0}   };
    d.brightness  = 128; // ~50%
    return d;
}

AnimConfigSet AnimConfigStore::load() {
    Preferences prefs;
    prefs.begin(NS, true);
    size_t stored = prefs.getBytesLength(KEY);

    AnimConfigSet cfg;
    if (stored == sizeof(AnimConfigSet)) {
        prefs.getBytes(KEY, &cfg, sizeof(AnimConfigSet));
        prefs.end();
        return cfg;
    }

    prefs.end();

    AnimConfigSet d = defaults();
    save(d); // persist now so future load() calls don't hit "not found" every time (~every 2s while the status page is open)
    return d;
}

void AnimConfigStore::save(const AnimConfigSet &cfg) {
    Preferences prefs;
    prefs.begin(NS, false);
    prefs.putBytes(KEY, &cfg, sizeof(AnimConfigSet));
    prefs.end();
}
