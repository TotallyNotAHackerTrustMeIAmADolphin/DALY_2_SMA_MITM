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
// smaChargeMode is a plain `const char*` rather than Arduino's String: every
// assignment stores one of a handful of string literals, so a pointer to
// static storage is enough.
//
// Every field has a default, so a default-constructed DashboardData is the
// state before any BMS/SMA data: zeros, no derating, no SMA mode yet.

// No temperature sensor is read: this fixed 22.0 C (0.1 C units) goes to
// the SMA in 0x356.
constexpr int16_t kFixedPackTempDeciC = 220;

struct DashboardData {
    float packVoltage = 0.0f;
    float avgCellVoltage = 0.0f;
    float minCellVoltage = 0.0f;
    float maxCellVoltage = 0.0f;
    // Latest BMS read, unsmoothed (no moving average). The smoothed pair
    // above drives the glideslope taper; these drive the hard cutoff/alarm
    // gate so a fast per-cell spike isn't hidden behind the ~48s filter
    // (see #9 - Cell 16 rose to ~3.5V under a 222A step while the smoothed
    // value only reached 3.416V).
    float minCellVoltageRaw = 0.0f;
    float maxCellVoltageRaw = 0.0f;
    std::vector<float> cellVoltages;

    // Raw max-min cell spread (#24), in mV, from the same latest read as
    // minCellVoltageRaw/maxCellVoltageRaw above - drives Glideslope's
    // spreadFactor() derating. derateFactor is that factor (1.0 = no
    // derating), stored for the dashboard so the operator can see the
    // setting act, not consumed by Glideslope::calculateCCL/DCL directly.
    uint16_t cellSpreadRawMv = 0;
    float derateFactor = 1.0f;

    float packCurrent = 0.0f;
    int16_t packTemp = kFixedPackTempDeciC;
    float packSOC = 0.0f;
    float requestedCurrent = 0.0f;
    const char *smaChargeMode = "Unknown";
    bool forceCharge = false;
    bool maintenanceActive = false;
    bool isResetting = false;
    bool gridPresent = false;

    // Daly BMS's own hardware protection state (cmd 0x93/0x98), independent
    // of Glideslope::calculateCCL/DCL - lets us see if the BMS itself
    // cut the pack off rather than inferring it from a voltage glitch.
    bool chargeMosOn = false;
    bool dischargeMosOn = false;
    bool bmsProtectionActive = false;
    bool cellOvervoltLevel1 = false;
    bool cellOvervoltLevel2 = false;
    bool packOvervoltLevel1 = false;
    bool packOvervoltLevel2 = false;
};
