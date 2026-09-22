#pragma once
#include <Arduino.h>
#include <vector>

// Guards cross-core access to both SystemConfig (cfg) and DashboardData
// (currentData) - defined in main.cpp, created in setup() before any task
// that touches either struct is started.
extern SemaphoreHandle_t dataMutex;

#include "SystemConfig.h"

// Holds live data to be pushed to the web dashboard and CAN bus
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
    String smaChargeMode;
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