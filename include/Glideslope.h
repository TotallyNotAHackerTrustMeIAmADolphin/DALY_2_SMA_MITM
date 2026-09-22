#pragma once

// The glideslope current-limit math, kept free of Arduino/FreeRTOS
// dependencies so test/test_glideslope compiles and runs *this* code
// natively (`pio test -e native`) instead of a hand-copied mirror of it.
// main.cpp's calculateCCL()/calculateDCL() are thin wrappers that feed in
// live state (cfg, currentData, millis()) under dataMutex.
//
// All limits are returned in 0.1A units, as sent in CAN frame 0x351.

#include <stdint.h>
#include <math.h>
#include "SystemConfig.h"

namespace Glideslope
{
    // True when a read has ever succeeded (haveRead) and the most recent one
    // is at most timeoutS seconds old. "Never read" counts as stale - before
    // this, lastRead == 0 made the check pass for the first timeoutS seconds
    // after boot, so full limits went out based on placeholder voltages.
    // Unsigned subtraction keeps this correct across millis() wraparound.
    inline bool isFresh(bool haveRead, uint32_t nowMs, uint32_t lastReadMs, int timeoutS)
    {
        if (!haveRead)
            return false;
        return (uint32_t)(nowMs - lastReadMs) <= (uint32_t)timeoutS * 1000u;
    }

    // Charge current limit. bmsFresh=false (comms timeout, or no BMS data
    // yet) forces 0A as a fail-safe, checked before anything else.
    inline uint16_t calculateCCL(const SystemConfig &cfg, float maxCellV, bool bmsFresh, bool maintenanceActive)
    {
        if (!bmsFresh)
            return 0;

        // A NaN voltage (e.g. a corrupted BMS read) or a NaN threshold (e.g.
        // a corrupted NVS float) makes every comparison below false, which
        // would otherwise fall through to the final `return maxChargeA` -
        // full current instead of the fail-safe 0A. Catch it explicitly.
        if (isnan(maxCellV) || isnan(cfg.cvMaxCharge) || isnan(cfg.cvHighAlarmGate) || isnan(cfg.cvStartTaper))
            return 0;

        if (maintenanceActive)
            return (uint16_t)round(cfg.maintAmps * 10.0f);

        if (maxCellV >= cfg.cvMaxCharge)
            return 0;
        if (maxCellV >= cfg.cvHighAlarmGate)
            return (uint16_t)round(cfg.trickleA * 10.0f);

        if (maxCellV > cfg.cvStartTaper)
        {
            float div = cfg.cvHighAlarmGate - cfg.cvStartTaper;
            if (div <= 0.0001f)
                return (uint16_t)round(cfg.trickleA * 10.0f);

            float slope = (cfg.cvHighAlarmGate - maxCellV) / div;
            if (slope < 0.0f)
                slope = 0.0f;
            if (slope > 1.0f)
                slope = 1.0f;

            float target = cfg.trickleA + (slope * (cfg.maxChargeA - cfg.trickleA));
            return (uint16_t)round(fmaxf(target, cfg.trickleA) * 10.0f);
        }
        return (uint16_t)round(cfg.maxChargeA * 10.0f);
    }

    // Discharge current limit - mirror image of calculateCCL.
    inline uint16_t calculateDCL(const SystemConfig &cfg, float minCellV, bool bmsFresh, bool maintenanceActive)
    {
        if (!bmsFresh)
            return 0;

        // See the matching check in calculateCCL(): NaN fails every
        // comparison below, which would otherwise fall through to the
        // final `return maxDischargeA` instead of the fail-safe 0A.
        if (isnan(minCellV) || isnan(cfg.cvMinDischarge) || isnan(cfg.cvLowAlarmGate) || isnan(cfg.cvStartDTaper))
            return 0;

        if (maintenanceActive)
            return 0;

        if (minCellV <= cfg.cvMinDischarge)
            return 0;
        if (minCellV <= cfg.cvLowAlarmGate)
            return (uint16_t)round(cfg.limpDischargeA * 10.0f);

        if (minCellV < cfg.cvStartDTaper)
        {
            float div = cfg.cvStartDTaper - cfg.cvLowAlarmGate;
            if (div <= 0.0001f)
                return (uint16_t)round(cfg.limpDischargeA * 10.0f);

            float slope = (minCellV - cfg.cvLowAlarmGate) / div;
            if (slope < 0.0f)
                slope = 0.0f;
            if (slope > 1.0f)
                slope = 1.0f;

            float target = cfg.limpDischargeA + (slope * (cfg.maxDischargeA - cfg.limpDischargeA));
            return (uint16_t)round(fmaxf(target, cfg.limpDischargeA) * 10.0f);
        }
        return (uint16_t)round(cfg.maxDischargeA * 10.0f);
    }
}
