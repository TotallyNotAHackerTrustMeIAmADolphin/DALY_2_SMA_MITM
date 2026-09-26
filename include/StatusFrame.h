#pragma once

// The status-frame decision logic (#29), kept free of Arduino/FreeRTOS so
// test/test_statusframe runs this code natively. canTask is I/O only: under
// dataMutex it builds a Snapshot (snapshotFrom), calls decide() and writes
// the write-back fields; outside the lock it sends Decision::values and
// logs whatever events fired.

#include <stdint.h>
#include <math.h>
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

    // Dashboard buttons, written by handleUIAction() on the web task and
    // read/written back by canTask, both under dataMutex, so the reset flag
    // and its hold start are always seen together. resetHoldStartMs == 0
    // means requested but not yet armed (see detail::advanceResetHold).
    struct UiCommands
    {
        bool manualMaintForce = false;
        bool resetRequested = false;
        uint32_t resetHoldStartMs = 0;
    };

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

    // The one place the shared state is mapped into a Snapshot; pure, so
    // test/test_canpath can run the same mapping canTask does.
    inline Snapshot snapshotFrom(const DashboardData &data, const BmsLink &link,
                                 const UiCommands &ui, uint32_t nowMs)
    {
        Snapshot snap;
        snap.nowMs = nowMs;
        snap.haveBasicInfo = link.haveBasicInfo;
        snap.haveCellData = link.haveCellData;
        snap.lastBasicInfoReadMs = link.lastBasicInfoMs;
        snap.lastCellReadMs = link.lastCellMs;
        snap.packVoltage = data.packVoltage;
        snap.packCurrent = data.packCurrent;
        snap.packSOC = data.packSOC;
        snap.packTemp = data.packTemp;
        snap.maxCellSmoothedV = data.maxCellVoltage;
        snap.maxCellRawV = data.maxCellVoltageRaw;
        snap.minCellSmoothedV = data.minCellVoltage;
        snap.minCellRawV = data.minCellVoltageRaw;
        snap.cellSpreadMv = data.cellSpreadRawMv;
        snap.manualMaintForce = ui.manualMaintForce;
        snap.resetRequested = ui.resetRequested;
        snap.resetHoldStartMs = ui.resetHoldStartMs;
        return snap;
    }

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

        // canTask writes these back to UiCommands under dataMutex, whether
        // or not sendFrames is true (a no-op copy-back when it's false).
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

        inline SMAFrames::SMATxData buildValues(const SystemConfig &cfg, const Snapshot &s, bool fresh,
                                  bool maintenanceActive, bool isResetting)
        {
            SMAFrames::SMATxData v;
            v.packVoltage = s.packVoltage;
            v.packCurrent = s.packCurrent;
            v.packTemp = s.packTemp;
            v.packSOC = s.packSOC;
            v.maintenanceActive = maintenanceActive;
            v.isResetting = isResetting;

            v.ccl = Glideslope::calculateCCL(cfg, s.maxCellSmoothedV, s.maxCellRawV, s.cellSpreadMv, fresh, maintenanceActive);
            v.dcl = Glideslope::calculateDCL(cfg, s.minCellSmoothedV, s.minCellRawV, s.cellSpreadMv, fresh, maintenanceActive);
            // Maintenance forces the fixed absorption target; otherwise the
            // configured per-cell limits x cell count.
            v.cvl = maintenanceActive ? kMaintCvlDeciV : Glideslope::toDeciVolts(cfg.cvMaxCharge * kPackCells);
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
