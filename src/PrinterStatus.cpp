#include "PrinterStatus.h"

const char *printerStateName(PrinterState s) {
    switch (s) {
        case PrinterState::Idle: return "idle";
        case PrinterState::Calibrating: return "calibrating";
        case PrinterState::Printing: return "printing";
        case PrinterState::Paused: return "paused";
        case PrinterState::Finished: return "finished";
        case PrinterState::Error: return "error";
        default: return "unknown";
    }
}
