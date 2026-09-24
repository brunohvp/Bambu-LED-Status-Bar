#pragma once
#include "PrinterStatus.h"
#include "AnimConfig.h"

namespace LedAnimations {
    void begin();

    // Call every loop() while connected. `percent` (0-100) is only used by the
    // Printing state; pass -1 otherwise. `chamberLightOn` dims the whole strip
    // to ~10% when the printer's own chamber light is off (a good signal
    // nobody's actively watching it). Non-blocking — draws at most 1 frame per
    // call, self-throttled to ~40fps, safe on every loop() tick.
    void update(PrinterState state, int percent, bool chamberLightOn = true);

    // Re-reads the saved config from NVS into memory (call after saving new
    // settings so the change applies immediately, no reboot needed).
    void reloadConfig();
}
