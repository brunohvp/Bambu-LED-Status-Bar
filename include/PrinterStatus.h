#pragma once
#include <Arduino.h>

// Simplified state derived from the printer's raw gcode_state, refined by
// stg_cur where gcode_state alone is too vague (see PrinterMqtt.cpp mapStage()
// for the sub-stage -> state table, e.g. homing/leveling -> Calibrating).
enum class PrinterState {
    Unknown,
    Idle,
    Heating,
    Calibrating,
    Printing,
    Paused,
    Finished,
    Error,
};

const char *printerStateName(PrinterState s);

struct PrinterStatus {
    PrinterState state = PrinterState::Unknown;
    String gcodeState;              // raw value (e.g. "RUNNING") — useful for debugging/mapping new states
    int percent = -1;                // mc_percent, -1 = not received yet
    int remainingMinutes = -1;        // mc_remaining_time
    float nozzleTemp = -1;
    float bedTemp = -1;
    String subtaskName;
    unsigned long lastUpdateMs = 0; // millis() of the last processed report
    bool everConnected = false;      // we've received at least one report from the printer
    bool chamberLightOn = true;      // defaults to "on" so we don't dim unexpectedly before the first report
};
