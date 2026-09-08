#pragma once
#include <Arduino.h>
#include <vector>

// Guards cross-core access to both SystemConfig (cfg) and DashboardData
// (currentData) - defined in main.cpp, created in setup() before any task
// that touches either struct is started.
extern SemaphoreHandle_t dataMutex;

// Holds all NVS saved settings
struct SystemConfig {
    float maxChargeA;
    float maxDischargeA;
    float cvStartTaper;
    float cvMaxCharge;
    float cvStartDTaper;
    float cvMinDischarge;
    float cvHighAlarmGate;
    float cvLowAlarmGate;
    float trickleA;
    float limpDischargeA;
    int vSamples;
    int bmsTimeout;
    float cvMaintStart;
    float cvMaintStop;
    float maintAmps;
};

// Holds live data to be pushed to the web dashboard and CAN bus
struct DashboardData {
    float packVoltage;
    float avgCellVoltage;
    float minCellVoltage; 
    float maxCellVoltage; 
    std::vector<float> cellVoltages; 
    
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