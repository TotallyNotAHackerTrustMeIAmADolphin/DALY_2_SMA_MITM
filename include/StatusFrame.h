#pragma once

// The status-frame decision logic, kept free of Arduino/FreeRTOS so
// test/test_statusframe runs this code natively. canTask is I/O only: under
// dataMutex it calls decide() directly on currentData/bmsLink/uiCommands and
// writes the write-back fields; outside the lock it sends Decision::values
// and logs whatever events fired.

#include <stdint.h>
#include <math.h>
#include <algorithm>
#include "SystemConfig.h"
#include "Glideslope.h"
#include "SMAFrames.h"
#include "DashboardData.h"

namespace StatusFrame
{
    // BMS read bookkeeping, written by bmsTask under dataMutex. Nothing is
    // sent to the SMA until both reads have succeeded once; decide()
    // treats "never read" as stale.
    struct BmsLink
    {
        bool haveBasicInfo = false;
        bool haveCellData = false;
        uint32_t lastBasicInfoMs = 0;
        uint32_t lastCellMs = 0;

        bool ready() const { return haveBasicInfo && haveCellData; }
    };

    // Dashboard buttons, written by handleUIAction() and read/written back
    // by canTask, both under dataMutex. resetHoldStartMs == 0 means
    // requested but not yet armed (see detail::advanceResetHold).
    struct UiCommands
    {
        bool manualMaintForce = false;
        bool resetRequested = false;
        uint32_t resetHoldStartMs = 0;
    };

    // Persistent between ticks; owned by canTask as a local. The reset hold
    // itself is NOT here - handleUIAction() (a different task) writes
    // UiCommands directly, under the same dataMutex; decide() only
    // reads/updates it via the UiCommands in, Decision out pair.
    struct ControlState
    {
        bool autoMaint = false;
        bool framesEnabled = false;
        bool wasFresh = false;
        bool staleBaseline = false;
        bool derating = false;
    };

    struct Decision
    {
        // False until haveBasicInfo && haveCellData - nothing else in this
        // struct is meaningful (and ControlState/the reset globals are left
        // untouched) when this is false.
        bool sendFrames = false;
        // Sent as-is by SMA_CAN::sendStatus().
        SMAFrames::SMATxData values;
        float derateFactor = 1.0f;
        bool fresh = false;

        // canTask writes this back to UiCommands under dataMutex, whether
        // or not sendFrames is true (a no-op copy-back when it's false).
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

    constexpr uint32_t kResetHoldMs = 5500;    // cluster reset: DVL 0 this long
    constexpr uint16_t kMaintCvlDeciV = 560;   // 56.0 V absorption target in maintenance
    constexpr int kDerateEndHysteresisMv = 10; // "derating ended" needs spread this far below start

    namespace detail
    {
        // Advances the reset hold, measured from the first frame actually
        // sent with the reset (DVL 0), not from the click, so a request
        // made while no frames go out still gets its full hold once frames
        // start. Returns true on the tick the hold ends.
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

        // Auto-maintenance hysteresis on the smoothed MINIMUM cell voltage,
        // not the pack average, so it starts as soon as any one weak cell
        // sags below cvMaintStart. Smoothed, not raw: a slow decision that
        // must not chatter on per-read noise.
        inline void updateAutoMaint(bool &autoMaint, float minCellSmoothedV, const SystemConfig &cfg)
        {
            if (!autoMaint && minCellSmoothedV > 0 && minCellSmoothedV < cfg.cvMaintStart)
                autoMaint = true;
            else if (autoMaint && minCellSmoothedV > cfg.cvMaintStop)
                autoMaint = false;
        }

        inline SMAFrames::SMATxData buildValues(const SystemConfig &cfg, const DashboardData &data, bool fresh,
                                  bool maintenanceActive, bool isResetting)
        {
            SMAFrames::SMATxData v;
            v.packVoltage = data.packVoltage;
            v.packCurrent = data.packCurrent;
            v.packTemp = kFixedPackTempDeciC;
            v.packSOC = data.packSOC;
            v.maintenanceActive = maintenanceActive;
            v.isResetting = isResetting;

            v.ccl = Glideslope::calculateCCL(cfg, data.maxCellVoltage, data.maxCellVoltageRaw, data.cellSpreadMv, fresh, maintenanceActive);
            v.dcl = Glideslope::calculateDCL(cfg, data.minCellVoltage, data.minCellVoltageRaw, data.cellSpreadMv, fresh, maintenanceActive);
            // Maintenance: fixed absorption target, never above the normal CVL (#60).
            uint16_t normalCvl = Glideslope::toDeciVolts(cfg.cvMaxCharge * kPackCells);
            v.cvl = maintenanceActive ? std::min(kMaintCvlDeciV, normalCvl) : normalCvl;
            v.dvl = Glideslope::toDeciVolts(cfg.cvMinDischarge * kPackCells);
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

        // Edge-triggered spread derating events; "ended" needs the factor
        // back at 1.0 AND the spread kDerateEndHysteresisMv below
        // spreadStartMv, so it doesn't chatter at the boundary.
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
    inline Decision decide(const SystemConfig &cfg, const DashboardData &data, const BmsLink &link,
                           const UiCommands &ui, uint32_t nowMs, ControlState &st)
    {
        Decision d;
        d.values.isResetting = ui.resetRequested;
        d.resetHoldStartMs = ui.resetHoldStartMs;

        // Send nothing until the BMS has delivered basic info AND cell
        // voltages once - no made-up SOC/voltage/limits ever go out.
        // ControlState and the reset hold are left untouched.
        if (!(link.haveBasicInfo && link.haveCellData))
            return d;

        bool isResetting = ui.resetRequested;
        uint32_t resetHoldStartMs = ui.resetHoldStartMs;
        d.events.resetFinished = detail::advanceResetHold(isResetting, resetHoldStartMs, nowMs);

        detail::updateAutoMaint(st.autoMaint, data.minCellVoltage, cfg);
        bool maintenanceActive = ui.manualMaintForce || st.autoMaint;

        // Both read stamps within cfg.bmsTimeout; "never read" is stale.
        bool fresh = Glideslope::isFresh(link.haveBasicInfo, nowMs, link.lastBasicInfoMs, cfg.bmsTimeout) &&
                     Glideslope::isFresh(link.haveCellData, nowMs, link.lastCellMs, cfg.bmsTimeout);

        // Also computed inside calculateCCL/DCL; mirrored here for the
        // dashboard and the derating events only.
        float derateFactor = Glideslope::spreadFactor(data.cellSpreadMv, cfg.spreadStartMv, cfg.spreadMaxMv);

        d.sendFrames = true;
        d.values = detail::buildValues(cfg, data, fresh, maintenanceActive, isResetting);
        d.derateFactor = derateFactor;
        d.fresh = fresh;
        d.resetHoldStartMs = resetHoldStartMs;

        if (!st.framesEnabled)
        {
            st.framesEnabled = true;
            d.events.firstFrames = true;
        }

        detail::trackFreshness(st, fresh, d.events.wentStale, d.events.freshAgain);
        detail::trackDerating(st, derateFactor, data.cellSpreadMv, cfg,
                              d.events.deratingStarted, d.events.deratingEnded);

        d.events.spreadMv = data.cellSpreadMv;
        d.events.deratePercent = (uint8_t)round(derateFactor * 100.0f);
        d.events.bmsTimeoutS = cfg.bmsTimeout;

        return d;
    }
}
