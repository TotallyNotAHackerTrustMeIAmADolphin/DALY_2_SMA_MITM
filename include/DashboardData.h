#pragma once
#include <stdint.h>
#include <vector>
#include "SystemConfig.h"

// Holds live data to be pushed to the web dashboard and CAN bus. Kept free
// of Arduino/FreeRTOS dependencies, like SystemConfig.h, so TelemetrySchema.h
// and the native unit tests can format this struct without pulling in the
// ESP32 Arduino core; `dataMutex` is declared where it's used instead.
// Every field has a default, so a default-constructed DashboardData is the
// state before any BMS/SMA data: zeros, no derating, no SMA mode yet.

// No temperature sensor is read: this fixed 22.0 C (0.1 C units) goes to
// the SMA in 0x356.
constexpr int16_t kFixedPackTempDeciC = 220;

struct DashboardData {
    // Smoothed (cfg.vSamples window, via ScalarSmoother.h) pack voltage -
    // feeds no safety decision (Glideslope only ever reads per-cell
    // voltages); display-only, for the dashboard/SSE/CSV graphs so they
    // aren't jittery. The inverter gets packVoltageRaw below instead.
    float packVoltage = 0.0f;
    float avgCellVoltage = 0.0f;
    float minCellVoltage = 0.0f;
    float maxCellVoltage = 0.0f;
    // Latest BMS read, unsmoothed - drives the glideslope hard cutoff/alarm
    // gate; see Glideslope.h's calculateCCL() comment for why.
    float minCellVoltageRaw = 0.0f;
    float maxCellVoltageRaw = 0.0f;
    std::vector<float> cellVoltages;

    // Smoothed max-min cell spread in mV (averaged over cfg.vSamples reads,
    // same window as minCellVoltage/maxCellVoltage) - drives
    // Glideslope::spreadFactor(). derateFactor mirrors that factor (1.0 = no
    // derating) for the dashboard.
    uint16_t cellSpreadMv = 0;
    // Raw (unsmoothed) max-min cell spread from the latest BMS read, same
    // read as minCellVoltageRaw/maxCellVoltageRaw. Diagnostics/telemetry
    // only - does not drive derating.
    uint16_t cellSpreadRawMv = 0;
    float derateFactor = 1.0f;

    // Smoothed the same way as packVoltage above; display-only.
    float packCurrent = 0.0f;
    // Latest BMS read, unsmoothed - what StatusFrame::buildValues() sends
    // to the inverter in CAN frame 0x356, so it never lags the real pack.
    float packVoltageRaw = 0.0f;
    float packCurrentRaw = 0.0f;
    // Left raw (unsmoothed) - the Daly's own SOC estimate is already smooth,
    // and BmsEvents' SOC-jump detector needs the true reading anyway.
    float packSOC = 0.0f;
    float requestedCurrent = 0.0f;
    const char *smaChargeMode = "Unknown";
    bool forceCharge = false;
    bool maintenanceActive = false;
    bool isResetting = false;
    bool gridPresent = false;

    // Daly BMS's own hardware protection state (cmd 0x93/0x98), independent
    // of Glideslope::calculateCCL/DCL - lets us see a BMS-initiated cutoff.
    bool chargeMosOn = false;
    bool dischargeMosOn = false;
    bool bmsProtectionActive = false;
    bool cellOvervoltLevel1 = false;
    bool cellOvervoltLevel2 = false;
    bool packOvervoltLevel1 = false;
    bool packOvervoltLevel2 = false;
};
