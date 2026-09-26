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
    // too large) value is undefined and in practice wraps, so a negative
    // trickleA/limpDischargeA/maintAmps (e.g. a /config typo) would go out
    // as ~6550A instead of 0A without this (#52). Every current-limit
    // return below goes through here, so no branch can reintroduce the
    // wraparound.
    inline uint16_t toDeciAmps(float amps)
    {
        float deci = roundf(amps * 10.0f);
        if (!(deci > 0.0f))
            return 0;
        if (deci > 65535.0f)
            return 65535;
        return (uint16_t)deci;
    }

    // Converts a voltage in V to the 0.1 V units sent in 0x351 (CVL/DVL).
    // Truncates, as the bare (uint16_t) cast it replaces did, but a NaN or
    // negative value gives 0 and a huge one saturates instead of being
    // undefined behaviour.
    inline uint16_t toDeciVolts(float volts)
    {
        float deci = volts * 10.0f;
        if (!(deci > 0.0f))
            return 0;
        if (deci >= 65535.0f)
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

    // Taper spans (start-taper to alarm gate) at or below this are treated
    // as degenerate: floor current, no division.
    constexpr float kMinTaperSpanV = 0.0001f;

    inline float clamp01(float x)
    {
        if (x < 0.0f)
            x = 0.0f;
        if (x > 1.0f)
            x = 1.0f;
        return x;
    }

    // The part calculateCCL() and calculateDCL() share once the cutoff and
    // gate branches have passed, with the voltage axis already folded:
    // headroomV is how far the smoothed cell is from the gate towards full
    // current, spanV the gate-to-start-taper distance. Interpolates floorA
    // (trickle/limp) .. maxA, applies the spread factor and never goes
    // below floorA. headroomV <= 0 (smoothed at/past the gate while raw
    // isn't) clamps to floorA - intended, see calculateCCL()'s comment.
    inline uint16_t taperLimit(float headroomV, float spanV, float floorA, float maxA, float factor)
    {
        if (spanV <= kMinTaperSpanV)
            return toDeciAmps(floorA);
        float slope = clamp01(headroomV / spanV);
        float target = floorA + (slope * (maxA - floorA));
        return toDeciAmps(fmaxf(target * factor, floorA));
    }

    // Below the taper: full current, spread-derated, never below floorA.
    inline uint16_t fullLimit(float maxA, float floorA, float factor)
    {
        return toDeciAmps(fmaxf(maxA * factor, floorA));
    }

    // Charge current limit. bmsFresh=false forces 0A (fail-safe), checked
    // first. Hard cutoff/gate use rawMaxV (the latest single BMS read) so a
    // fast per-cell spike trips them immediately, not ~48s later once the
    // moving average catches up; the taper between cvStartTaper and
    // cvHighAlarmGate keeps using smoothedMaxV so the CCL doesn't jitter on
    // per-read noise - see CLAUDE.md's Glideslope section (#9) for the full
    // rationale and root cause this fixed. Once smoothedMaxV reaches the
    // gate, the taper can't give back more than trickle even if rawMaxV
    // has since dropped below it - intentional, not a bug.
    //
    // spreadMv (#24) derates the taper/full-current result via
    // spreadFactor(), clamped back up to trickleA; never applied to the
    // hard 0A/gate-trickle branches (already gated by the weak cell's own
    // voltage) or in maintenance mode.
    inline uint16_t calculateCCL(const SystemConfig &cfg, float smoothedMaxV, float rawMaxV, uint16_t spreadMv, bool bmsFresh, bool maintenanceActive)
    {
        if (!bmsFresh)
            return 0;

        // NaN (corrupted BMS read, NVS float, or current setpoint) fails
        // every comparison below, which would otherwise fall through to
        // full current instead of 0A; round(NaN)->uint16_t is also
        // undefined. Catch explicitly.
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
            return taperLimit(cfg.cvHighAlarmGate - smoothedMaxV, cfg.cvHighAlarmGate - cfg.cvStartTaper,
                              cfg.trickleA, cfg.maxChargeA, factor);
        return fullLimit(cfg.maxChargeA, cfg.trickleA, factor);
    }

    // Discharge current limit - mirror image of calculateCCL. See its
    // comment for why the cutoff/gate use rawMinV while the taper uses
    // smoothedMinV, and for spreadMv (#24) - derates the taper/full-current
    // result only, clamped back up to limpDischargeA.
    inline uint16_t calculateDCL(const SystemConfig &cfg, float smoothedMinV, float rawMinV, uint16_t spreadMv, bool bmsFresh, bool maintenanceActive)
    {
        if (!bmsFresh)
            return 0;

        // See the matching check in calculateCCL().
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
            return taperLimit(smoothedMinV - cfg.cvLowAlarmGate, cfg.cvStartDTaper - cfg.cvLowAlarmGate,
                              cfg.limpDischargeA, cfg.maxDischargeA, factor);
        return fullLimit(cfg.maxDischargeA, cfg.limpDischargeA, factor);
    }
}
