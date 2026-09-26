#pragma once
#include <stdint.h>
#include <vector>
#include "SystemConfig.h"

// Holds live data to be pushed to the web dashboard and CAN bus. Kept free
// of Arduino/FreeRTOS dependencies, like SystemConfig.h, so TelemetrySchema.h
// and the native unit tests can format this struct without pulling in the
// ESP32 Arduino core; `dataMutex` stays declared in SystemState.h instead.
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
    // Latest BMS read, unsmoothed - drives the glideslope hard cutoff/alarm
    // gate; see Glideslope.h's calculateCCL() comment for why.
    float minCellVoltageRaw = 0.0f;
    float maxCellVoltageRaw = 0.0f;
    std::vector<float> cellVoltages;

    // Raw max-min cell spread in mV, from the same latest read as
    // minCellVoltageRaw/maxCellVoltageRaw - drives Glideslope::spreadFactor().
    // derateFactor mirrors that factor (1.0 = no derating) for the dashboard.
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
    // of Glideslope::calculateCCL/DCL - lets us see a BMS-initiated cutoff.
    bool chargeMosOn = false;
    bool dischargeMosOn = false;
    bool bmsProtectionActive = false;
    bool cellOvervoltLevel1 = false;
    bool cellOvervoltLevel2 = false;
    bool packOvervoltLevel1 = false;
    bool packOvervoltLevel2 = false;
};
