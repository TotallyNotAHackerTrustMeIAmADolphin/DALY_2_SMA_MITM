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

    // Reproduces the old canTask SMA-TX block exactly - see the retired
    // block this replaced (main.cpp, canTask, the `if (now - lastSmaTx >
    // 250)` section) for the byte-for-byte behaviour this must match.
    inline Decision decide(const SystemConfig &cfg, const Snapshot &s, ControlState &st)
    {
        Decision d;
        d.isResetting = s.resetRequested;
        d.resetHoldStartMs = s.resetHoldStartMs;

        // Send nothing until the BMS has delivered basic info AND cell
        // voltages once - currentData holds no real values before that,
        // and the SMA already rides through a few seconds of CAN silence
        // on every reboot, which is better than made-up SOC/voltage/limits
        // going out. ControlState and the reset hold are left untouched.
        if (!(s.haveBasicInfo && s.haveCellData))
            return d;

        // The reset hold is measured from the first frame actually sent
        // with the reset bit (handleUIAction() arms it with
        // resetHoldStartTime = 0 under the same mutex as this whole
        // block), not from the click, so a request made while no frames go
        // out still gets its full 5.5s on the bus once frames start.
        bool isResetting = s.resetRequested;
        uint32_t resetHoldStartMs = s.resetHoldStartMs;
        if (isResetting)
        {
            if (resetHoldStartMs == 0)
                resetHoldStartMs = s.nowMs ? s.nowMs : 1;
            else if (s.nowMs - resetHoldStartMs > 5500)
            {
                isResetting = false;
                d.events.resetFinished = true;
            }
        }

        // Auto-maintenance hysteresis on the smoothed MINIMUM cell voltage
        // vs cvMaintStart/cvMaintStop (#12), matching the config page's
        // "any cell" wording instead of the pack average: under discharge
        // one weak cell can hit the discharge floor while the pack
        // average is still well above cvMaintStart * kCellCount, so the
        // old pack-voltage comparison never fired. Smoothed, not raw,
        // because maintenance is a slow decision and must not chatter on
        // per-read noise (same smoothed/raw split Glideslope.h uses for
        // the taper vs. the hard cutoff).
        if (!st.autoMaint && s.minCellSmoothedV > 0 && s.minCellSmoothedV < cfg.cvMaintStart)
            st.autoMaint = true;
        else if (st.autoMaint && s.minCellSmoothedV > cfg.cvMaintStop)
            st.autoMaint = false;

        bool maintenanceActive = s.manualMaintForce || st.autoMaint;

        // Same freshness check the old calculateCCL()/calculateDCL()
        // wrappers derived via bmsDataFresh() - both read stamps must be
        // within cfg.bmsTimeout, "never read" counts as stale.
        bool fresh = Glideslope::isFresh(s.haveBasicInfo, s.nowMs, s.lastBasicInfoReadMs, cfg.bmsTimeout) &&
                     Glideslope::isFresh(s.haveCellData, s.nowMs, s.lastCellReadMs, cfg.bmsTimeout);

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
        // Force-charge / maintenance CVL override (560 = 56.0V, the fixed
        // absorption target used while maintenanceActive); otherwise the
        // configured hard max x cell count, in 0.1V units like the rest of
        // SMATxData.
        v.cvl = maintenanceActive ? 560 : (uint16_t)(cfg.cvMaxCharge * kCellCount * 10);
        v.dvl = (uint16_t)(cfg.cvMinDischarge * kCellCount * 10);

        // Spread factor (#24), computed once and reused for CCL, DCL (both
        // inside calculateCCL/DCL above) and mirrored here as
        // derateFactor purely for the dashboard - not consumed a second
        // time by anything in this function.
        float derateFactor = Glideslope::spreadFactor(s.cellSpreadMv, cfg.spreadStartMv, cfg.spreadMaxMv);

        d.sendFrames = true;
        d.values = v;
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

        // Edge-triggered, and only meaningful once real BMS data has
        // started flowing (framesEnabled, just possibly set above on this
        // very tick) - staleBaseline gates the "fresh again" event so the
        // very first-ever fresh reading isn't reported as a recovery.
        if (st.framesEnabled)
        {
            if (st.wasFresh && !fresh)
            {
                d.events.wentStale = true;
                st.staleBaseline = true;
            }
            else if (!st.wasFresh && fresh && st.staleBaseline)
            {
                d.events.freshAgain = true;
            }
            st.wasFresh = fresh;
        }

        // Edge-triggered cell-spread derating event (#24), with hysteresis
        // on the "ended" side so it doesn't chatter right at the boundary:
        // only reported once derateFactor is back at 1.0 AND the spread
        // has fallen at least 10mV below spreadStartMv, not merely back
        // under it.
        if (st.framesEnabled)
        {
            if (!st.derating && derateFactor < 1.0f)
            {
                d.events.deratingStarted = true;
                st.derating = true;
            }
            else if (st.derating && derateFactor >= 1.0f &&
                     (int)s.cellSpreadMv <= (int)cfg.spreadStartMv - 10)
            {
                d.events.deratingEnded = true;
                st.derating = false;
            }
        }

        d.events.spreadMv = s.cellSpreadMv;
        d.events.deratePercent = (uint8_t)round(derateFactor * 100.0f);
        d.events.bmsTimeoutS = cfg.bmsTimeout;

        return d;
    }
}
