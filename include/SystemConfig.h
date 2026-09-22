#pragma once
#include <stdint.h>

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

    // Raw (max-min) cell spread, in mV, at which current-limit derating
    // starts (#24) - below this the taper/full-current result is untouched.
    uint16_t spreadStartMv = 60;
    // Raw cell spread, in mV, at which derating bottoms out: the taper/
    // full-current result is forced down to trickle/limp current. Linear
    // in between spreadStartMv and spreadMaxMv.
    uint16_t spreadMaxMv = 150;
};
