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
};