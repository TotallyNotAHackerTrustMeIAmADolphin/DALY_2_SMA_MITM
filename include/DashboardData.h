#pragma once
#include <stdint.h>
#include <vector>
#include "SystemConfig.h"

// Holds live data to be pushed to the web dashboard and CAN bus.
//
// Split out of SystemState.h (#32) and kept free of Arduino/FreeRTOS
// dependencies, like SystemConfig.h, so TelemetrySchema.h - and the native
// unit tests that include it (`pio test -e native`) - can format this
// struct without pulling in the ESP32 Arduino core. `dataMutex`, which
// guards cross-task access to this struct, still needs Arduino.h for
// `SemaphoreHandle_t`, so its declaration stays behind in SystemState.h
// rather than moving here too.
//
// smaChargeMode is a plain `const char*` rather than Arduino's String for
// the same reason: every assignment site (SMA_CAN.cpp's readMessages(),
// main.cpp's setup()) only ever stores one of a handful of string literals
// ("Bulk"/"Absorption"/"Float"/"Equalize"/"Unknown"), so no dynamic String
// behavior (concatenation, resizing, heap allocation) was ever exercised on
// this field - a raw pointer to static storage is sufficient. Defaulted to
// "Unknown" (matching what main.cpp's setup() sets it to) so it's never a
// null pointer before that runs, e.g. if something logs telemetry very
// early during boot.
struct DashboardData {
    float packVoltage;
    float avgCellVoltage;
    float minCellVoltage;
    float maxCellVoltage;
    // Latest BMS read, unsmoothed (no moving average). The smoothed pair
    // above drives the glideslope taper; these drive the hard cutoff/alarm
    // gate so a fast per-cell spike isn't hidden behind the ~48s filter
    // (see #9 - Cell 16 rose to ~3.5V under a 222A step while the smoothed
    // value only reached 3.416V).
    float minCellVoltageRaw;
    float maxCellVoltageRaw;
    std::vector<float> cellVoltages;

    // Raw max-min cell spread (#24), in mV, from the same latest read as
    // minCellVoltageRaw/maxCellVoltageRaw above - drives Glideslope's
    // spreadFactor() derating. derateFactor is that factor (1.0 = no
    // derating), stored for the dashboard so the operator can see the
    // setting act, not consumed by calculateCCL/DCL directly.
    uint16_t cellSpreadRawMv;
    float derateFactor;

    float packCurrent;
    int16_t packTemp;
    float packSOC;
    float requestedCurrent;
    const char *smaChargeMode = "Unknown";
    bool forceCharge;
    bool maintenanceActive;
    bool isResetting;
    bool gridPresent;

    // Daly BMS's own hardware protection state (cmd 0x93/0x98), independent
    // of our calculateCCL/DCL glideslope - lets us see if the BMS itself
    // cut the pack off rather than inferring it from a voltage glitch.
    bool chargeMosOn;
    bool dischargeMosOn;
    bool bmsProtectionActive;
    bool cellOvervoltLevel1;
    bool cellOvervoltLevel2;
    bool packOvervoltLevel1;
    bool packOvervoltLevel2;
};
