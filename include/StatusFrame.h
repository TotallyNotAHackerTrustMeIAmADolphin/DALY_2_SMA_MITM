#pragma once

// The status-frame decision logic (#29), kept free of Arduino/FreeRTOS
// dependencies for the same reason as Glideslope.h: so
// test/test_statusframe compiles and runs *this* code natively
// (`pio test -e native`) instead of a hand-copied mirror of it. canTask in
// main.cpp becomes I/O only: copy a Snapshot out of currentData/the reset
// globals under dataMutex, call decide(), write the results back under the
// same lock, then (outside the lock) send the CAN frame and log whatever
// events fired.
//
// This absorbs main.cpp's old calculateCCL()/calculateDCL() wrappers and
// bmsDataFresh() - their logic now lives inside decide() below, unchanged.

#include <stdint.h>
#include <math.h>
#include "SystemConfig.h"
#include "Glideslope.h"

namespace StatusFrame
{
    // main.cpp hardcodes 16 cells (its MAX_CELLS) into the CVL/DVL
    // formulas below (the auto-maintenance hysteresis compares a per-cell
    // voltage directly since #12, so it no longer needs a cell count);
    // mirrored here as a named constant rather than a magic 16; NOT the
    // DalyRS485 per-cell read count (that one stays MAX_CELLS in
    // main.cpp), just the same value.
    constexpr int kCellCount = 16;

    // Everything canTask reads out of currentData / the shared reset
    // globals under dataMutex to decide one 250ms tick's frame. Plain old
    // data, copied out under the lock so decide() itself never touches the
    // lock.
    struct Snapshot
    {
        uint32_t nowMs = 0;

        bool haveBasicInfo = false;
        bool haveCellData = false;
        uint32_t lastBasicInfoReadMs = 0;
        uint32_t lastCellReadMs = 0;

        float packVoltage = 0.0f;
        float packCurrent = 0.0f;
        float packSOC = 0.0f;
        int16_t packTemp = 0;

        // Smoothed pair drives the taper, raw pair drives the hard
        // cutoff/alarm gate and the spread - see Glideslope.h's comments.
        float maxCellSmoothedV = 0.0f;
        float maxCellRawV = 0.0f;
        float minCellSmoothedV = 0.0f;
        float minCellRawV = 0.0f;
        uint16_t cellSpreadMv = 0;

        bool manualMaintForce = false;

        // Mirrors the shared isResetting/resetHoldStartTime globals
        // (written by handleUIAction() on the web server's task, under the
        // same dataMutex). resetHoldStartMs == 0 means "requested but not
        // yet armed" - see decide()'s comment on the reset hold below.
        bool resetRequested = false;
        uint32_t resetHoldStartMs = 0;
    };

    // Persistent between ticks; owned by canTask as a local (replaces the
    // function-local `framesEnabled` and the `static` flags that used to
    // live inside canTask's SMA-TX block). The reset hold itself is NOT
    // here - handleUIAction() (a different task) writes resetHoldStartTime/
    // isResetting directly, so those stay in the shared globals; decide()
    // only reads/updates them via Snapshot in and Decision out.
    struct ControlState
    {
        bool autoMaint = false;
        bool framesEnabled = false;
        bool wasFresh = false;
        bool staleBaseline = false;
        bool derating = false;
    };

    // The plain frame values, in the units SMATxData uses (ccl/dcl/cvl/dvl
    // in 0.1A/0.1V, packTemp in 0.1 degC). canTask maps this field-by-field
    // into an SMATxData after decide() returns. forceCharge mirrors
    // currentData.forceCharge (dashboard-only today; not part of the CAN
    // frame itself).
    struct Values
    {
        float packVoltage = 0.0f;
        float packCurrent = 0.0f;
        int16_t packTemp = 0;
        float packSOC = 0.0f;
        uint16_t ccl = 0;
        uint16_t dcl = 0;
        uint16_t cvl = 0;
        uint16_t dvl = 0;
        bool forceCharge = false;
        bool maintenanceActive = false;
        bool isResetting = false;
    };

    struct Decision
    {
        // False until haveBasicInfo && haveCellData - nothing else in this
        // struct is meaningful (and ControlState/the reset globals are left
        // untouched) when this is false.
        bool sendFrames = false;
        Values values;
        float derateFactor = 1.0f;
        bool fresh = false;
        bool maintenanceActive = false;

        // canTask writes these back to the shared isResetting/
        // resetHoldStartTime globals under dataMutex, whether or not
        // sendFrames is true (they're a no-op copy-back when it's false).
        bool isResetting = false;
        uint32_t resetHoldStartMs = 0;

        // One-shot log events for this tick - canTask emits the existing
        // log lines for whichever of these fired, outside the lock.
        struct
        {
            bool firstFrames = false;
            bool resetFinished = false;
            bool wentStale = false;
            bool freshAgain = false;
            bool deratingStarted = false;
            bool deratingEnded = false;
            uint16_t spreadMv = 0;
            uint8_t deratePercent = 0;
            int bmsTimeoutS = 0;
        } events;
    };

    constexpr uint32_t kResetHoldMs = 5500;     // cluster reset: DVL 0 this long
    constexpr uint16_t kMaintCvlDeciV = 560;    // 56.0 V absorption target in maintenance
    constexpr int kDerateEndHysteresisMv = 10;  // "derating ended" needs spread this far below start

    namespace detail
    {
        // Advances the reset hold. Measured from the first frame actually
        // sent with the reset (DVL 0, #63), not from the click:
        // handleUIAction() arms it with resetHoldStartMs = 0, so a request
        // made while no frames go out still gets its full hold on the bus
        // once frames start. Returns true on the tick the hold ends.
        inline bool advanceResetHold(bool &resetting, uint32_t &holdStartMs, uint32_t nowMs)
        {
            if (!resetting)
                return false;
            if (holdStartMs == 0)
                holdStartMs = nowMs ? nowMs : 1;
            else if (nowMs - holdStartMs > kResetHoldMs)
            {
                resetting = false;
                return true;
            }
            return false;
        }

        // Auto-maintenance hysteresis on the smoothed MINIMUM cell voltage
        // (#12): starts as soon as any one weak cell sags below
        // cvMaintStart, not the pack average, which under discharge stays
        // well above it while one cell hits the floor. Smoothed, not raw:
        // maintenance is a slow decision and must not chatter on per-read
        // noise.
        inline void updateAutoMaint(bool &autoMaint, float minCellSmoothedV, const SystemConfig &cfg)
        {
            if (!autoMaint && minCellSmoothedV > 0 && minCellSmoothedV < cfg.cvMaintStart)
                autoMaint = true;
            else if (autoMaint && minCellSmoothedV > cfg.cvMaintStop)
                autoMaint = false;
        }

        // Both read stamps within cfg.bmsTimeout; "never read" is stale.
        inline bool bmsFresh(const SystemConfig &cfg, const Snapshot &s)
        {
            return Glideslope::isFresh(s.haveBasicInfo, s.nowMs, s.lastBasicInfoReadMs, cfg.bmsTimeout) &&
                   Glideslope::isFresh(s.haveCellData, s.nowMs, s.lastCellReadMs, cfg.bmsTimeout);
        }

        inline Values buildValues(const SystemConfig &cfg, const Snapshot &s, bool fresh,
                                  bool maintenanceActive, bool isResetting)
        {
            Values v;
            v.packVoltage = s.packVoltage;
            v.packCurrent = s.packCurrent;
            v.packTemp = s.packTemp;
            v.packSOC = s.packSOC;
            v.maintenanceActive = maintenanceActive;
            v.forceCharge = maintenanceActive;
            v.isResetting = isResetting;

            v.ccl = Glideslope::calculateCCL(cfg, s.maxCellSmoothedV, s.maxCellRawV, s.cellSpreadMv, fresh, maintenanceActive);
            v.dcl = Glideslope::calculateDCL(cfg, s.minCellSmoothedV, s.minCellRawV, s.cellSpreadMv, fresh, maintenanceActive);
            // Maintenance forces the fixed absorption target; otherwise the
            // configured per-cell limits x cell count.
            v.cvl = maintenanceActive ? kMaintCvlDeciV : Glideslope::toDeciVolts(cfg.cvMaxCharge * kCellCount);
            v.dvl = Glideslope::toDeciVolts(cfg.cvMinDischarge * kCellCount);
            return v;
        }

        // Edge-triggered stale/fresh events. staleBaseline keeps the very
        // first fresh reading from being reported as a recovery.
        inline void trackFreshness(ControlState &st, bool fresh, bool &wentStale, bool &freshAgain)
        {
            if (st.wasFresh && !fresh)
            {
                wentStale = true;
                st.staleBaseline = true;
            }
            else if (!st.wasFresh && fresh && st.staleBaseline)
            {
                freshAgain = true;
            }
            st.wasFresh = fresh;
        }

        // Edge-triggered spread derating events (#24), with hysteresis on
        // the "ended" side so it doesn't chatter at the boundary: ended only
        // once the factor is back at 1.0 AND the spread is at least
        // kDerateEndHysteresisMv below spreadStartMv.
        inline void trackDerating(ControlState &st, float derateFactor, uint16_t spreadMv,
                                  const SystemConfig &cfg, bool &started, bool &ended)
        {
            if (!st.derating && derateFactor < 1.0f)
            {
                started = true;
                st.derating = true;
            }
            else if (st.derating && derateFactor >= 1.0f &&
                     (int)spreadMv <= (int)cfg.spreadStartMv - kDerateEndHysteresisMv)
            {
                ended = true;
                st.derating = false;
            }
        }
    }

    // Everything canTask decides per 250 ms tick. Byte-for-byte the
    // behaviour test/test_statusframe pins down.
    inline Decision decide(const SystemConfig &cfg, const Snapshot &s, ControlState &st)
    {
        Decision d;
        d.isResetting = s.resetRequested;
        d.resetHoldStartMs = s.resetHoldStartMs;

        // Send nothing until the BMS has delivered basic info AND cell
        // voltages once - there are no real values before that, and the SMA
        // already rides through a few seconds of CAN silence on every
        // reboot, which is better than made-up SOC/voltage/limits going
        // out. ControlState and the reset hold are left untouched.
        if (!(s.haveBasicInfo && s.haveCellData))
            return d;

        bool isResetting = s.resetRequested;
        uint32_t resetHoldStartMs = s.resetHoldStartMs;
        d.events.resetFinished = detail::advanceResetHold(isResetting, resetHoldStartMs, s.nowMs);

        detail::updateAutoMaint(st.autoMaint, s.minCellSmoothedV, cfg);
        bool maintenanceActive = s.manualMaintForce || st.autoMaint;

        bool fresh = detail::bmsFresh(cfg, s);

        // Also computed inside calculateCCL/DCL; mirrored here for the
        // dashboard and the derating events only.
        float derateFactor = Glideslope::spreadFactor(s.cellSpreadMv, cfg.spreadStartMv, cfg.spreadMaxMv);

        d.sendFrames = true;
        d.values = detail::buildValues(cfg, s, fresh, maintenanceActive, isResetting);
        d.derateFactor = derateFactor;
        d.fresh = fresh;
        d.maintenanceActive = maintenanceActive;
        d.isResetting = isResetting;
        d.resetHoldStartMs = resetHoldStartMs;

        if (!st.framesEnabled)
        {
            st.framesEnabled = true;
            d.events.firstFrames = true;
        }

        detail::trackFreshness(st, fresh, d.events.wentStale, d.events.freshAgain);
        detail::trackDerating(st, derateFactor, s.cellSpreadMv, cfg,
                              d.events.deratingStarted, d.events.deratingEnded);

        d.events.spreadMv = s.cellSpreadMv;
        d.events.deratePercent = (uint8_t)round(derateFactor * 100.0f);
        d.events.bmsTimeoutS = cfg.bmsTimeout;

        return d;
    }
}
