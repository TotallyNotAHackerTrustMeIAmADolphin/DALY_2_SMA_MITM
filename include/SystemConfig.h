#pragma once

// NVS-persisted settings (loaded/saved by WebDashboard). Kept free of
// Arduino/FreeRTOS includes so the pure glideslope math in Glideslope.h,
// and the native unit tests that include it, can use it too.
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
