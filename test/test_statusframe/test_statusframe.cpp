// Native unit tests for the status-frame decision logic (#29):
// `pio test -e native`. Includes the real include/StatusFrame.h - no
// mirrored copy to keep in sync. These tests pin down the exact semantics
// of the old inline canTask SMA-TX block before it was extracted, so a
// future change to decide() that silently changes wire behaviour fails
// here first.

#include <unity.h>
#include "StatusFrame.h"
#include "GlideslopeFixture.h"

using StatusFrame::BmsLink;
using StatusFrame::ControlState;
using StatusFrame::Decision;
using StatusFrame::UiCommands;
using StatusFrame::decide;

static SystemConfig cfg;

void setUp(void)
{
    // Same charge/discharge/maintenance shape as test_glideslope's setUp(),
    // so the CCL/DCL numbers below can be cross-checked against that file
    // (#73). Maintenance thresholds: start 3.05V, stop 3.2V - see
    // GlideslopeFixture.h for why start isn't 3.0V (cvMinDischarge).
    cfg = glideslopeTestConfig();
}

void tearDown(void) {}

// Fresh BmsLink as of nowMs.
static BmsLink freshLink(uint32_t nowMs)
{
    BmsLink l;
    l.haveBasicInfo = true;
    l.haveCellData = true;
    l.lastBasicInfoMs = nowMs;
    l.lastCellMs = nowMs;
    return l;
}

// Cell voltages deep in the full-current region both ways (3.0V max / 3.3V
// min - below cvStartTaper, above cvStartDTaper), zero spread. Individual
// tests override only the fields they care about.
static DashboardData freshData()
{
    DashboardData d;
    d.packVoltage = 55.0f;
    d.packCurrent = 0.0f;
    d.packSOC = 50.0f;
    d.maxCellVoltage = 3.0f;
    d.maxCellVoltageRaw = 3.0f;
    d.minCellVoltage = 3.3f; // above both maint thresholds -> autoMaint off
    d.minCellVoltageRaw = 3.3f;
    d.cellSpreadMv = 0;
    d.cellSpreadRawMv = 0;
    return d;
}

// Mirrors canTask's applyDecision() (src/main.cpp): feeds ui's reset pair
// into each tick and writes the Decision's back, so a multi-tick reset
// sequence doesn't hand-copy Decision fields into locals at each call site.
struct Harness
{
    ControlState ctrl;
    UiCommands ui;

    Decision tick(const DashboardData &d, const BmsLink &link, uint32_t nowMs)
    {
        Decision dec = decide(cfg, d, link, ui, nowMs, ctrl);
        ui.resetRequested = dec.values.isResetting;
        ui.resetHoldStartMs = dec.resetHoldStartMs;
        return dec;
    }
};

// --- Gate: nothing sent before both BMS reads have succeeded once ---

static void test_no_frames_before_basic_info(void)
{
    ControlState ctrl;
    BmsLink link = freshLink(1000);
    link.haveBasicInfo = false;
    Decision d = decide(cfg, freshData(), link, UiCommands{}, 1000, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_FALSE(ctrl.framesEnabled);
}

static void test_no_frames_before_cell_data(void)
{
    ControlState ctrl;
    BmsLink link = freshLink(1000);
    link.haveCellData = false;
    Decision d = decide(cfg, freshData(), link, UiCommands{}, 1000, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_FALSE(ctrl.framesEnabled);
}

// --- Frames + full limits once fresh ---

static void test_frames_full_limits_once_fresh(void)
{
    ControlState ctrl;
    Decision d = decide(cfg, freshData(), freshLink(1000), UiCommands{}, 1000, ctrl);

    TEST_ASSERT_TRUE(d.sendFrames);
    TEST_ASSERT_TRUE(d.fresh);
    TEST_ASSERT_FALSE(d.values.maintenanceActive);
    // 3.0V is below cvStartTaper(3.3)/above cvStartDTaper(3.2), spread 0 ->
    // full current both ways, same numbers as test_glideslope's
    // test_ccl_full_below_taper / test_dcl_full_above_taper.
    TEST_ASSERT_EQUAL(1000, d.values.ccl); // 100.0A
    TEST_ASSERT_EQUAL(2000, d.values.dcl); // 200.0A
    // cvl = cvMaxCharge(3.5) * 16 cells * 10 = 560 (56.0V); dvl =
    // cvMinDischarge(3.0) * 16 * 10 = 480 (48.0V).
    TEST_ASSERT_EQUAL(560, d.values.cvl);
    TEST_ASSERT_EQUAL(480, d.values.dvl);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, d.derateFactor);
    TEST_ASSERT_TRUE(d.events.firstFrames);
    TEST_ASSERT_TRUE(ctrl.framesEnabled);
}

static void test_first_frames_fires_exactly_once(void)
{
    ControlState ctrl;
    Decision d1 = decide(cfg, freshData(), freshLink(1000), UiCommands{}, 1000, ctrl);
    Decision d2 = decide(cfg, freshData(), freshLink(1250), UiCommands{}, 1250, ctrl);
    Decision d3 = decide(cfg, freshData(), freshLink(1500), UiCommands{}, 1500, ctrl);
    TEST_ASSERT_TRUE(d1.events.firstFrames);
    TEST_ASSERT_FALSE(d2.events.firstFrames);
    TEST_ASSERT_FALSE(d3.events.firstFrames);
}

// --- Staleness: 0A both ways, wentStale once, freshAgain once ---

static void test_stale_forces_zero_then_recovers(void)
{
    ControlState ctrl;

    // t=1000: fresh, frames enabled, wasFresh -> true.
    Decision d1 = decide(cfg, freshData(), freshLink(1000), UiCommands{}, 1000, ctrl);
    TEST_ASSERT_TRUE(d1.fresh);
    TEST_ASSERT_FALSE(d1.events.wentStale);
    TEST_ASSERT_FALSE(d1.events.freshAgain);

    // t=62000: both stamps are 61s old (> cfg.bmsTimeout=60s) -> stale.
    // calculateCCL/DCL fail their bmsFresh check first and return 0
    // regardless of the (still full-current) cell voltages.
    Decision d2 = decide(cfg, freshData(), freshLink(1000), UiCommands{}, 62000, ctrl);
    TEST_ASSERT_FALSE(d2.fresh);
    TEST_ASSERT_EQUAL(0, d2.values.ccl);
    TEST_ASSERT_EQUAL(0, d2.values.dcl);
    TEST_ASSERT_TRUE(d2.events.wentStale);
    TEST_ASSERT_FALSE(d2.events.freshAgain);

    // Still stale next tick: wentStale must not fire again (edge-triggered).
    Decision d3 = decide(cfg, freshData(), freshLink(1000), UiCommands{}, 62250, ctrl);
    TEST_ASSERT_FALSE(d3.fresh);
    TEST_ASSERT_FALSE(d3.events.wentStale);
    TEST_ASSERT_FALSE(d3.events.freshAgain);

    // Fresh again: new read stamps at t=62500.
    Decision d4 = decide(cfg, freshData(), freshLink(62500), UiCommands{}, 62500, ctrl);
    TEST_ASSERT_TRUE(d4.fresh);
    TEST_ASSERT_EQUAL(1000, d4.values.ccl); // limits restored
    TEST_ASSERT_FALSE(d4.events.wentStale);
    TEST_ASSERT_TRUE(d4.events.freshAgain);

    // And freshAgain must not repeat on the next fresh tick either.
    Decision d5 = decide(cfg, freshData(), freshLink(62500), UiCommands{}, 62750, ctrl);
    TEST_ASSERT_FALSE(d5.events.freshAgain);
}

// --- Reset hold: armed by the first sent frame, 5.5s, isResetting true throughout ---

static void test_reset_not_armed_while_no_frames_sent(void)
{
    // Request arrives while the BMS has never reported - decide() must not
    // touch the hold timer (mirrors handleUIAction() arming
    // resetHoldStartMs=0, and canTask never getting to the reset-hold code
    // while !(haveBasicInfo && haveCellData)).
    ControlState ctrl;
    BmsLink link = freshLink(1000);
    link.haveBasicInfo = false;
    UiCommands ui;
    ui.resetRequested = true;
    Decision d = decide(cfg, freshData(), link, ui, 1000, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_TRUE(d.values.isResetting);      // pass-through, unchanged
    TEST_ASSERT_EQUAL_UINT32(0, d.resetHoldStartMs); // still not armed
}

static void test_reset_hold_arms_on_first_sent_frame_and_finishes_after_5500ms(void)
{
    Harness h;
    h.ui.resetRequested = true;

    // First tick with real data flowing, t=1000: hold arms here (was 0).
    Decision d1 = h.tick(freshData(), freshLink(1000), 1000);
    TEST_ASSERT_TRUE(d1.sendFrames);
    TEST_ASSERT_EQUAL_UINT32(1000, d1.resetHoldStartMs); // armed at nowMs
    TEST_ASSERT_TRUE(d1.values.isResetting);
    TEST_ASSERT_FALSE(d1.events.resetFinished);

    // t=6499: 5499ms of hold (not yet > 5500) -> still resetting.
    Decision d2 = h.tick(freshData(), freshLink(6499), 6499);
    TEST_ASSERT_TRUE(d2.values.isResetting);
    TEST_ASSERT_FALSE(d2.events.resetFinished);

    // t=6501: 5501ms of hold (> 5500) -> finishes this tick.
    Decision d3 = h.tick(freshData(), freshLink(6501), 6501);
    TEST_ASSERT_FALSE(d3.values.isResetting);
    TEST_ASSERT_TRUE(d3.events.resetFinished);
}

static void test_reset_hold_arms_at_nowms_zero_uses_sentinel_one(void)
{
    // holdStartMs==0 means "not yet armed" (see advanceResetHold), so
    // nowMs==0 can't be stored as the real start stamp - it substitutes 1.
    Harness h;
    h.ui.resetRequested = true;
    Decision d = h.tick(freshData(), freshLink(0), 0);
    TEST_ASSERT_TRUE(d.values.isResetting);
    TEST_ASSERT_EQUAL_UINT32(1, d.resetHoldStartMs);
}

static void test_reset_hold_survives_millis_wraparound(void)
{
    // holdStartMs armed just before millis() wraps; unsigned subtraction
    // in advanceResetHold wraps the same way, so the hold must still end
    // after kResetHoldMs of real elapsed time - not before, not never.
    Harness h;
    h.ui.resetRequested = true;
    uint32_t start = 0xFFFFFFF0u; // 16ms before wraparound
    Decision d1 = h.tick(freshData(), freshLink(start), start);
    TEST_ASSERT_EQUAL_UINT32(start, d1.resetHoldStartMs);

    Decision d2 = h.tick(freshData(), freshLink(start), start + 5000u); // wrapped, < hold
    TEST_ASSERT_TRUE(d2.values.isResetting);
    TEST_ASSERT_FALSE(d2.events.resetFinished);

    Decision d3 = h.tick(freshData(), freshLink(start), start + StatusFrame::kResetHoldMs + 1u); // wrapped, > hold
    TEST_ASSERT_FALSE(d3.values.isResetting);
    TEST_ASSERT_TRUE(d3.events.resetFinished);
}

// --- Auto-maintenance hysteresis ---

static void test_auto_maint_starts_below_start_stops_above_stop_no_toggle_between(void)
{
    // Thresholds (#12: compared directly against the smoothed minimum
    // cell voltage): start cvMaintStart=3.05V, stop cvMaintStop=3.2V.
    ControlState ctrl;

    // 3.3V: above both -> off.
    Decision d1 = decide(cfg, freshData(), freshLink(1000), UiCommands{}, 1000, ctrl); // minCellVoltage=3.3
    TEST_ASSERT_FALSE(d1.values.maintenanceActive);

    // 3.1V: between start and stop, autoMaint currently off -> stays off
    // (3.1 is not < 3.05).
    DashboardData d2data = freshData();
    d2data.minCellVoltage = 3.1f;
    Decision d2 = decide(cfg, d2data, freshLink(1250), UiCommands{}, 1250, ctrl);
    TEST_ASSERT_FALSE(d2.values.maintenanceActive);

    // 2.9V: below start(3.05) -> turns on.
    DashboardData d3data = freshData();
    d3data.minCellVoltage = 2.9f;
    Decision d3 = decide(cfg, d3data, freshLink(1500), UiCommands{}, 1500, ctrl);
    TEST_ASSERT_TRUE(d3.values.maintenanceActive);

    // 3.1V again: between start and stop, autoMaint now on -> hysteresis
    // keeps it on (3.1 is not > 3.2).
    DashboardData d4data = freshData();
    d4data.minCellVoltage = 3.1f;
    Decision d4 = decide(cfg, d4data, freshLink(1750), UiCommands{}, 1750, ctrl);
    TEST_ASSERT_TRUE(d4.values.maintenanceActive);

    // 3.3V: above stop(3.2) -> turns off.
    DashboardData d5data = freshData();
    d5data.minCellVoltage = 3.3f;
    Decision d5 = decide(cfg, d5data, freshLink(2000), UiCommands{}, 2000, ctrl);
    TEST_ASSERT_FALSE(d5.values.maintenanceActive);
}

// --- #12: trigger on the minimum cell, not the pack average ---

static void test_auto_maint_starts_on_weak_cell_even_when_pack_average_is_high(void)
{
    // The exact scenario from #12: Cell 16 sags under discharge and hits
    // the discharge floor while the pack average is still well above the
    // old pack-voltage trigger (cvMaintStart(3.05) * kPackCells(16) =
    // 48.8V). packVoltage=49.6V > 48.8V, so the retired pack-average
    // comparison would never have started maintenance here; the minimum
    // cell (2.99V) is what's actually below cvMaintStart(3.05V).
    ControlState ctrl;
    DashboardData d = freshData();
    d.packVoltage = 49.6f;
    d.minCellVoltage = 2.99f;
    Decision dec = decide(cfg, d, freshLink(1000), UiCommands{}, 1000, ctrl);
    TEST_ASSERT_TRUE(dec.values.maintenanceActive);
    TEST_ASSERT_TRUE(ctrl.autoMaint);
}

static void test_auto_maint_never_starts_with_zero_min_cell(void)
{
    // minCellVoltage == 0 means "no data yet" (same sentinel used
    // elsewhere in Glideslope) - it must never satisfy "< cvMaintStart"
    // and start maintenance on a value nobody measured.
    ControlState ctrl;
    DashboardData d = freshData();
    d.minCellVoltage = 0.0f;
    Decision dec = decide(cfg, d, freshLink(1000), UiCommands{}, 1000, ctrl);
    TEST_ASSERT_FALSE(dec.values.maintenanceActive);
    TEST_ASSERT_FALSE(ctrl.autoMaint);
}

static void test_manual_force_overrides(void)
{
    // minCellVoltage=3.3V would leave autoMaint off; manualMaintForce
    // alone must still drive maintenanceActive.
    ControlState ctrl;
    UiCommands ui;
    ui.manualMaintForce = true;
    Decision d = decide(cfg, freshData(), freshLink(1000), ui, 1000, ctrl);
    TEST_ASSERT_TRUE(d.values.maintenanceActive);
}

static void test_maintenance_cvl_is_fixed_560_not_cvmaxcharge(void)
{
    // With cvMaxCharge changed so cvMaxCharge*16*10 != 560, the maintenance
    // CVL must still read 560 - proves it's the fixed override, not the
    // normal formula.
    ControlState ctrl;
    cfg.cvMaxCharge.setUnchecked(3.6f); // normal CVL would be 576, not 560
    UiCommands ui;
    ui.manualMaintForce = true;
    Decision d = decide(cfg, freshData(), freshLink(1000), ui, 1000, ctrl);
    TEST_ASSERT_EQUAL(560, d.values.cvl);

    // And confirm the non-maintenance CVL DOES follow cvMaxCharge, for
    // contrast.
    ControlState ctrl2;
    Decision d2 = decide(cfg, freshData(), freshLink(1000), UiCommands{}, 1000, ctrl2);
    TEST_ASSERT_EQUAL(576, d2.values.cvl);
}

static void test_maintenance_cvl_capped_by_lowered_cvmaxcharge(void)
{
    // #60: a lowered cvMaxCharge (here 3.0V x 16 x 10 = 480, below the
    // fixed 560 maintenance target) must cap the maintenance CVL at the
    // normal value - maintenance must never ask for a HIGHER pack voltage
    // than normal operation allows.
    ControlState ctrl;
    cfg.cvMaxCharge.setUnchecked(3.0f); // normal CVL = 480
    UiCommands ui;
    ui.manualMaintForce = true;
    Decision d = decide(cfg, freshData(), freshLink(1000), ui, 1000, ctrl);
    TEST_ASSERT_EQUAL(480, d.values.cvl);
}

// --- Cell-spread derating ---

static void test_derate_factor_applied_once_ccl_at_spread_midpoint_is_half(void)
{
    // spreadMv=105 is the midpoint of the default 60..150 span -> factor
    // 0.5. maxCellVoltage/Raw=3.0V is below cvStartTaper -> full-current
    // branch: 100A * 0.5 = 50A -> 500. minCellVoltage/Raw=3.3V (freshData
    // default) is above cvStartDTaper -> 200A * 0.5 = 100A -> 1000. Same
    // spreadMv, same numbers as test_glideslope's
    // test_ccl_derated_by_spread_at_midpoint / test_dcl_derated_by_spread.
    ControlState ctrl;
    DashboardData d = freshData();
    d.cellSpreadMv = 105;
    Decision dec = decide(cfg, d, freshLink(1000), UiCommands{}, 1000, ctrl);
    TEST_ASSERT_EQUAL(500, dec.values.ccl);
    TEST_ASSERT_EQUAL(1000, dec.values.dcl);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, dec.derateFactor);
}

static void test_half_stale_cells_stale_basic_info_fresh_forces_zero(void)
{
    // haveBasicInfo fresh, haveCellData stale (last read further back than
    // cfg.bmsTimeout(60s)=60000ms) -> decide()'s `fresh` is the AND of
    // both, so both limits go to 0.
    ControlState ctrl;
    BmsLink link = freshLink(100000);
    link.lastCellMs = 100000 - 70000; // 70s ago, > 60s timeout
    Decision d = decide(cfg, freshData(), link, UiCommands{}, 100000, ctrl);
    TEST_ASSERT_EQUAL(0, d.values.ccl);
    TEST_ASSERT_EQUAL(0, d.values.dcl);
    TEST_ASSERT_FALSE(d.fresh);
}

static void test_half_stale_basic_info_stale_cells_fresh_forces_zero(void)
{
    // Reverse of the above: haveCellData fresh, haveBasicInfo stale ->
    // same AND, same result.
    ControlState ctrl;
    BmsLink link = freshLink(100000);
    link.lastBasicInfoMs = 100000 - 70000; // 70s ago, > 60s timeout
    Decision d = decide(cfg, freshData(), link, UiCommands{}, 100000, ctrl);
    TEST_ASSERT_EQUAL(0, d.values.ccl);
    TEST_ASSERT_EQUAL(0, d.values.dcl);
    TEST_ASSERT_FALSE(d.fresh);
}

// Two full stale/fresh cycles: each transition fires its event exactly
// once, and the second recovery is reported like the first (#87).
static void test_stale_fresh_cycles_each_event_once(void)
{
    ControlState ctrl;
    uint32_t t = 100000;
    int wentStale = 0, freshAgain = 0;
    // fresh (5 ticks), stale (5), fresh (5), stale (5), fresh (5)
    for (int phase = 0; phase < 5; phase++)
        for (int i = 0; i < 5; i++, t += 250)
        {
            BmsLink link = freshLink(t);
            if (phase % 2 == 1)
                link.lastCellMs = t - 70000; // > 60 s bmsTimeout
            Decision d = decide(cfg, freshData(), link, UiCommands{}, t, ctrl);
            wentStale += d.events.wentStale;
            freshAgain += d.events.freshAgain;
            TEST_ASSERT_EQUAL(phase % 2 == 0, d.fresh);
        }
    TEST_ASSERT_EQUAL(2, wentStale);
    TEST_ASSERT_EQUAL(2, freshAgain);
}

// --- Charge cutoff / gate events: debounced, fresh-only, raw voltage ---

// One tick at nowMs with raw max cell voltage rawV and a fresh link.
static Decision chargeTick(ControlState &ctrl, float rawV, uint32_t nowMs)
{
    DashboardData d = freshData();
    d.maxCellVoltageRaw = rawV;
    d.maxCellVoltage = rawV;
    return decide(cfg, d, freshLink(nowMs), UiCommands{}, nowMs, ctrl);
}

static void test_cutoff_trips_once_on_first_crossing(void)
{
    ControlState ctrl;
    Decision d0 = chargeTick(ctrl, 3.3f, 1000);
    TEST_ASSERT_FALSE(d0.events.chargeCutoffTripped);
    Decision d1 = chargeTick(ctrl, 3.51f, 1250);
    TEST_ASSERT_TRUE(d1.events.chargeCutoffTripped);
    TEST_ASSERT_FALSE(d1.events.chargeCutoffCleared);
    Decision d2 = chargeTick(ctrl, 3.52f, 1500);
    TEST_ASSERT_FALSE(d2.events.chargeCutoffTripped);
    TEST_ASSERT_FALSE(d2.events.chargeCutoffCleared);
}

static void test_cutoff_short_clear_then_retrip_is_silent(void)
{
    ControlState ctrl;
    chargeTick(ctrl, 3.51f, 1000);
    // Clear for 10 s, then back above: no cleared, no second tripped.
    Decision a = chargeTick(ctrl, 3.45f, 2000);
    Decision b = chargeTick(ctrl, 3.45f, 12000);
    Decision c = chargeTick(ctrl, 3.51f, 12250);
    TEST_ASSERT_FALSE(a.events.chargeCutoffCleared);
    TEST_ASSERT_FALSE(b.events.chargeCutoffCleared);
    TEST_ASSERT_FALSE(c.events.chargeCutoffTripped);
    TEST_ASSERT_FALSE(c.events.chargeCutoffCleared);
    // The clear timer restarted: a new clear shorter than the hold still
    // does not fire.
    chargeTick(ctrl, 3.45f, 20000);
    Decision e = chargeTick(ctrl, 3.45f, 20000 + StatusFrame::kChargeLimitClearHoldMs - 1);
    TEST_ASSERT_FALSE(e.events.chargeCutoffCleared);
}

static void test_cutoff_clear_held_60s_fires_cleared_once(void)
{
    ControlState ctrl;
    chargeTick(ctrl, 3.51f, 1000);
    Decision a = chargeTick(ctrl, 3.45f, 2000);
    TEST_ASSERT_FALSE(a.events.chargeCutoffCleared);
    Decision b = chargeTick(ctrl, 3.45f, 2000 + StatusFrame::kChargeLimitClearHoldMs);
    TEST_ASSERT_TRUE(b.events.chargeCutoffCleared);
    TEST_ASSERT_FALSE(b.events.chargeCutoffTripped);
    Decision c = chargeTick(ctrl, 3.45f, 2000 + StatusFrame::kChargeLimitClearHoldMs + 250);
    TEST_ASSERT_FALSE(c.events.chargeCutoffCleared);
    // Trips again afterwards.
    Decision d = chargeTick(ctrl, 3.51f, 70000);
    TEST_ASSERT_TRUE(d.events.chargeCutoffTripped);
}

static void test_gate_trips_once_on_first_crossing(void)
{
    ControlState ctrl;
    // 3.45 V: above the gate (3.4) but below the cutoff (3.5).
    Decision d1 = chargeTick(ctrl, 3.45f, 1000);
    TEST_ASSERT_TRUE(d1.events.chargeGateTripped);
    TEST_ASSERT_FALSE(d1.events.chargeCutoffTripped);
    Decision d2 = chargeTick(ctrl, 3.46f, 1250);
    TEST_ASSERT_FALSE(d2.events.chargeGateTripped);
    TEST_ASSERT_FALSE(d2.events.chargeGateCleared);
}

static void test_gate_short_clear_then_retrip_is_silent(void)
{
    ControlState ctrl;
    chargeTick(ctrl, 3.45f, 1000);
    Decision a = chargeTick(ctrl, 3.3f, 2000);
    Decision b = chargeTick(ctrl, 3.3f, 30000);
    Decision c = chargeTick(ctrl, 3.45f, 30250);
    TEST_ASSERT_FALSE(a.events.chargeGateCleared);
    TEST_ASSERT_FALSE(b.events.chargeGateCleared);
    TEST_ASSERT_FALSE(c.events.chargeGateTripped);
    TEST_ASSERT_FALSE(c.events.chargeGateCleared);
}

static void test_gate_clear_held_60s_fires_cleared_once(void)
{
    ControlState ctrl;
    chargeTick(ctrl, 3.45f, 1000);
    chargeTick(ctrl, 3.3f, 2000);
    Decision b = chargeTick(ctrl, 3.3f, 2000 + StatusFrame::kChargeLimitClearHoldMs);
    TEST_ASSERT_TRUE(b.events.chargeGateCleared);
    Decision c = chargeTick(ctrl, 3.3f, 2000 + StatusFrame::kChargeLimitClearHoldMs + 250);
    TEST_ASSERT_FALSE(c.events.chargeGateCleared);
}

static void test_charge_limit_events_silent_and_state_kept_when_stale(void)
{
    ControlState ctrl;
    chargeTick(ctrl, 3.51f, 1000); // trips cutoff + gate
    TEST_ASSERT_TRUE(ctrl.cutoffActive);
    TEST_ASSERT_TRUE(ctrl.gateActive);

    // Stale (stamps 61 s old), cell voltage low: nothing emitted, state kept.
    DashboardData d = freshData();
    d.maxCellVoltageRaw = 3.0f;
    Decision s = decide(cfg, d, freshLink(1000), UiCommands{}, 62000, ctrl);
    TEST_ASSERT_FALSE(s.fresh);
    TEST_ASSERT_FALSE(s.events.chargeCutoffTripped);
    TEST_ASSERT_FALSE(s.events.chargeCutoffCleared);
    TEST_ASSERT_FALSE(s.events.chargeGateTripped);
    TEST_ASSERT_FALSE(s.events.chargeGateCleared);
    TEST_ASSERT_TRUE(ctrl.cutoffActive);
    TEST_ASSERT_TRUE(ctrl.gateActive);
    TEST_ASSERT_EQUAL_UINT32(0, ctrl.cutoffClearSinceMs);
    TEST_ASSERT_EQUAL_UINT32(0, ctrl.gateClearSinceMs);

    // Stale + high voltage on a fresh ControlState: no trip either.
    ControlState ctrl2;
    DashboardData hi = freshData();
    hi.maxCellVoltageRaw = 3.6f;
    Decision s2 = decide(cfg, hi, freshLink(1000), UiCommands{}, 62000, ctrl2);
    TEST_ASSERT_FALSE(s2.events.chargeCutoffTripped);
    TEST_ASSERT_FALSE(s2.events.chargeGateTripped);
    TEST_ASSERT_FALSE(ctrl2.cutoffActive);
}

static void test_charge_limit_events_carry_voltages(void)
{
    ControlState ctrl;
    Decision d = chargeTick(ctrl, 3.512f, 1000);
    TEST_ASSERT_TRUE(d.events.chargeCutoffTripped);
    TEST_ASSERT_EQUAL_FLOAT(3.512f, d.events.rawMaxV);
    TEST_ASSERT_EQUAL_FLOAT(3.5f, d.events.cutoffV);
    TEST_ASSERT_EQUAL_FLOAT(3.4f, d.events.gateV);
}

// A stale gap during a running clear timer must not count toward the hold:
// the first fresh tick after it starts the 60 s afresh.
static void test_stale_gap_resets_clear_timer(void)
{
    ControlState ctrl;
    chargeTick(ctrl, 3.51f, 1000);
    chargeTick(ctrl, 3.45f, 2000); // clear timer starts at 2000
    // Stale tick well past the hold: no event, timer dropped.
    DashboardData d = freshData();
    d.maxCellVoltageRaw = 3.0f;
    Decision s = decide(cfg, d, freshLink(1000), UiCommands{}, 2000 + StatusFrame::kChargeLimitClearHoldMs + 5000, ctrl);
    TEST_ASSERT_FALSE(s.fresh);
    TEST_ASSERT_FALSE(s.events.chargeCutoffCleared);
    TEST_ASSERT_TRUE(ctrl.cutoffActive);
    // First fresh tick after the gap: not cleared yet, the hold restarts.
    uint32_t t = 2000 + StatusFrame::kChargeLimitClearHoldMs + 10000;
    Decision a = chargeTick(ctrl, 3.45f, t);
    TEST_ASSERT_FALSE(a.events.chargeCutoffCleared);
    Decision b = chargeTick(ctrl, 3.45f, t + StatusFrame::kChargeLimitClearHoldMs);
    TEST_ASSERT_TRUE(b.events.chargeCutoffCleared);
}

// One read jumping from below the gate to above the cutoff trips both in
// the same tick (the log layer suppresses the redundant gate line).
static void test_cutoff_and_gate_trip_same_tick(void)
{
    ControlState ctrl;
    chargeTick(ctrl, 3.3f, 1000);
    Decision d = chargeTick(ctrl, 3.52f, 1250);
    TEST_ASSERT_TRUE(d.events.chargeCutoffTripped);
    TEST_ASSERT_TRUE(d.events.chargeGateTripped);
}

// The event thresholds must agree with what Glideslope actually does:
// cutoff trips exactly when CCL is forced to 0 A, gate exactly when the
// raw voltage reaches cvHighAlarmGate.
static void test_events_match_glideslope_thresholds(void)
{
    {
        ControlState ctrl;
        Decision below = chargeTick(ctrl, cfg.cvMaxCharge - 0.001f, 1000);
        TEST_ASSERT_FALSE(below.events.chargeCutoffTripped);
        TEST_ASSERT_TRUE(below.values.ccl > 0);
        ControlState ctrl2;
        Decision at = chargeTick(ctrl2, cfg.cvMaxCharge, 1000);
        TEST_ASSERT_TRUE(at.events.chargeCutoffTripped);
        TEST_ASSERT_EQUAL(0, at.values.ccl);
    }
    {
        ControlState ctrl;
        Decision below = chargeTick(ctrl, cfg.cvHighAlarmGate - 0.001f, 1000);
        TEST_ASSERT_FALSE(below.events.chargeGateTripped);
        ControlState ctrl2;
        Decision at = chargeTick(ctrl2, cfg.cvHighAlarmGate, 1000);
        TEST_ASSERT_TRUE(at.events.chargeGateTripped);
        TEST_ASSERT_TRUE(at.values.ccl <= below.values.ccl);
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_frames_before_basic_info);
    RUN_TEST(test_no_frames_before_cell_data);
    RUN_TEST(test_frames_full_limits_once_fresh);
    RUN_TEST(test_first_frames_fires_exactly_once);
    RUN_TEST(test_stale_forces_zero_then_recovers);
    RUN_TEST(test_reset_not_armed_while_no_frames_sent);
    RUN_TEST(test_reset_hold_arms_on_first_sent_frame_and_finishes_after_5500ms);
    RUN_TEST(test_reset_hold_arms_at_nowms_zero_uses_sentinel_one);
    RUN_TEST(test_reset_hold_survives_millis_wraparound);
    RUN_TEST(test_auto_maint_starts_below_start_stops_above_stop_no_toggle_between);
    RUN_TEST(test_auto_maint_starts_on_weak_cell_even_when_pack_average_is_high);
    RUN_TEST(test_auto_maint_never_starts_with_zero_min_cell);
    RUN_TEST(test_manual_force_overrides);
    RUN_TEST(test_maintenance_cvl_is_fixed_560_not_cvmaxcharge);
    RUN_TEST(test_maintenance_cvl_capped_by_lowered_cvmaxcharge);
    RUN_TEST(test_derate_factor_applied_once_ccl_at_spread_midpoint_is_half);
    RUN_TEST(test_half_stale_cells_stale_basic_info_fresh_forces_zero);
    RUN_TEST(test_half_stale_basic_info_stale_cells_fresh_forces_zero);
    RUN_TEST(test_stale_fresh_cycles_each_event_once);
    RUN_TEST(test_cutoff_trips_once_on_first_crossing);
    RUN_TEST(test_cutoff_short_clear_then_retrip_is_silent);
    RUN_TEST(test_cutoff_clear_held_60s_fires_cleared_once);
    RUN_TEST(test_gate_trips_once_on_first_crossing);
    RUN_TEST(test_gate_short_clear_then_retrip_is_silent);
    RUN_TEST(test_gate_clear_held_60s_fires_cleared_once);
    RUN_TEST(test_charge_limit_events_silent_and_state_kept_when_stale);
    RUN_TEST(test_charge_limit_events_carry_voltages);
    RUN_TEST(test_stale_gap_resets_clear_timer);
    RUN_TEST(test_cutoff_and_gate_trip_same_tick);
    RUN_TEST(test_events_match_glideslope_thresholds);
    return UNITY_END();
}
