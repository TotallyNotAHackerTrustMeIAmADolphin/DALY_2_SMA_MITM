#pragma once

// The glideslope current-limit math, kept free of Arduino/FreeRTOS
// dependencies so test/test_glideslope compiles and runs *this* code
// natively (`pio test -e native`) instead of a hand-copied mirror of it.
// StatusFrame::decide() (include/StatusFrame.h) is the only caller; it
// feeds in the snapshot canTask copied out of currentData under dataMutex.
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

    // Converts a current in A to the 0.1A units sent in 0x351, clamped to
    // what a uint16_t can hold. A float -> uint16_t cast of a negative (or
    // too large) value is undefined and in practice wraps: a negative
    // trickleA/limpDischargeA/maintAmps (e.g. a /config typo) used to go
    // out as ~6550A instead of 0A (#52). Every current-limit return below
    // goes through here, so no branch can reintroduce the wraparound.
    inline uint16_t toDeciAmps(float amps)
    {
        float deci = roundf(amps * 10.0f);
        if (!(deci > 0.0f))
            return 0;
        if (deci > 65535.0f)
            return 65535;
        return (uint16_t)deci;
    }

    // Derating factor from the raw (max-min) cell spread (#24). A weak
    // cell's IR drop is proportional to current, not state of charge, so a
    // voltage threshold alone reacts late (see #8 - Cell 16's offset grows
    // with current, not SOC); this turns the spread itself into a current
    // cap: 1.0 (no derating) at/below startMv, linear down to 0.0 at/above
    // maxMv. maxMv <= startMv is a degenerate config (e.g. both left at 0,
    // or set the wrong way round) - treated as a step (1.0 below startMv,
    // 0.0 at/above it) rather than dividing by a non-positive span.
    inline float spreadFactor(uint16_t spreadMv, uint16_t startMv, uint16_t maxMv)
    {
        if (maxMv <= startMv)
            return spreadMv < startMv ? 1.0f : 0.0f;

        if (spreadMv <= startMv)
            return 1.0f;
        if (spreadMv >= maxMv)
            return 0.0f;

        return 1.0f - (float)(spreadMv - startMv) / (float)(maxMv - startMv);
    }

    // Charge current limit. bmsFresh=false (comms timeout, or no BMS data
    // yet) forces 0A as a fail-safe, checked before anything else.
    //
    // Two different cell-voltage inputs, by design (#9): the hard cutoff
    // (cvMaxCharge -> 0A) and the alarm gate (cvHighAlarmGate -> trickle)
    // use rawMaxV, the latest single BMS read - a fast per-cell spike (e.g.
    // a weak cell's internal resistance under a sudden current step) must
    // trip these immediately, not ~48s later once bmsTask's moving average
    // catches up. The taper between cvStartTaper and cvHighAlarmGate keeps
    // using smoothedMaxV so the CCL doesn't jitter with normal per-read
    // noise. One consequence: if smoothedMaxV is already at/above the gate
    // while rawMaxV has since dropped back below it, the taper's slope
    // clamp still yields trickle (never more than trickle once the gate has
    // been reached on the smoothed value) - that's intentional, not a bug.
    //
    // spreadMv (#24) is the raw max-min cell spread in mV: a weak cell's IR
    // drop scales with current, not voltage/SOC, so it derates the taper
    // and full-current results via spreadFactor() - multiplicatively,
    // clamped back up to trickleA, same as the "never below trickle" clamp
    // already used for the taper's slope target. Never applied to the hard
    // 0A/gate-trickle branches above (those are the fail-safes; the weak
    // cell's own voltage already gates them) or in maintenance mode.
    inline uint16_t calculateCCL(const SystemConfig &cfg, float smoothedMaxV, float rawMaxV, uint16_t spreadMv, bool bmsFresh, bool maintenanceActive)
    {
        if (!bmsFresh)
            return 0;

        // A NaN voltage (e.g. a corrupted BMS read) or a NaN threshold (e.g.
        // a corrupted NVS float) makes every comparison below false, which
        // would otherwise fall through to the final `return maxChargeA` -
        // full current instead of the fail-safe 0A. A NaN current setpoint
        // would make round(NaN) -> uint16_t undefined. Catch both explicitly.
        if (isnan(smoothedMaxV) || isnan(rawMaxV) || isnan(cfg.cvMaxCharge) || isnan(cfg.cvHighAlarmGate) || isnan(cfg.cvStartTaper) ||
            isnan(cfg.maxChargeA) || isnan(cfg.trickleA) || isnan(cfg.maintAmps))
            return 0;

        if (maintenanceActive)
            return toDeciAmps(cfg.maintAmps);

        if (rawMaxV >= cfg.cvMaxCharge)
            return 0;
        if (rawMaxV >= cfg.cvHighAlarmGate)
            return toDeciAmps(cfg.trickleA);

        float factor = spreadFactor(spreadMv, cfg.spreadStartMv, cfg.spreadMaxMv);

        if (smoothedMaxV > cfg.cvStartTaper)
        {
            float div = cfg.cvHighAlarmGate - cfg.cvStartTaper;
            if (div <= 0.0001f)
                return toDeciAmps(cfg.trickleA);

            // If smoothedMaxV is already at/above cvHighAlarmGate here (raw
            // has since fallen back below the gate, or this cell's smoothed
            // value simply lags above it), slope goes negative and clamps
            // to 0 -> target == trickleA. Intended: see the function
            // comment above.
            float slope = (cfg.cvHighAlarmGate - smoothedMaxV) / div;
            if (slope < 0.0f)
                slope = 0.0f;
            if (slope > 1.0f)
                slope = 1.0f;

            float target = cfg.trickleA + (slope * (cfg.maxChargeA - cfg.trickleA));
            return toDeciAmps(fmaxf(target * factor, cfg.trickleA));
        }
        return toDeciAmps(fmaxf(cfg.maxChargeA * factor, cfg.trickleA));
    }

    // Discharge current limit - mirror image of calculateCCL. See its
    // comment for why the cutoff/gate use rawMinV while the taper uses
    // smoothedMinV, and for spreadMv (#24) - derates the taper/full-current
    // result only, clamped back up to limpDischargeA.
    inline uint16_t calculateDCL(const SystemConfig &cfg, float smoothedMinV, float rawMinV, uint16_t spreadMv, bool bmsFresh, bool maintenanceActive)
    {
        if (!bmsFresh)
            return 0;

        // See the matching check in calculateCCL(): NaN fails every
        // comparison below, which would otherwise fall through to the
        // final `return maxDischargeA` instead of the fail-safe 0A.
        if (isnan(smoothedMinV) || isnan(rawMinV) || isnan(cfg.cvMinDischarge) || isnan(cfg.cvLowAlarmGate) || isnan(cfg.cvStartDTaper) ||
            isnan(cfg.maxDischargeA) || isnan(cfg.limpDischargeA))
            return 0;

        if (maintenanceActive)
            return 0;

        if (rawMinV <= cfg.cvMinDischarge)
            return 0;
        if (rawMinV <= cfg.cvLowAlarmGate)
            return toDeciAmps(cfg.limpDischargeA);

        float factor = spreadFactor(spreadMv, cfg.spreadStartMv, cfg.spreadMaxMv);

        if (smoothedMinV < cfg.cvStartDTaper)
        {
            float div = cfg.cvStartDTaper - cfg.cvLowAlarmGate;
            if (div <= 0.0001f)
                return toDeciAmps(cfg.limpDischargeA);

            // Mirror of calculateCCL()'s slope clamp: smoothedMinV at/below
            // cvLowAlarmGate here (raw has since risen back above it) makes
            // slope negative, clamped to 0 -> target == limpDischargeA.
            // Intended: see calculateCCL()'s comment above.
            float slope = (smoothedMinV - cfg.cvLowAlarmGate) / div;
            if (slope < 0.0f)
                slope = 0.0f;
            if (slope > 1.0f)
                slope = 1.0f;

            float target = cfg.limpDischargeA + (slope * (cfg.maxDischargeA - cfg.limpDischargeA));
            return toDeciAmps(fmaxf(target * factor, cfg.limpDischargeA));
        }
        return toDeciAmps(fmaxf(cfg.maxDischargeA * factor, cfg.limpDischargeA));
    }
}
