// Native unit tests for the status-frame decision logic (#29):
// `pio test -e native`. Includes the real include/StatusFrame.h - no
// mirrored copy to keep in sync. These tests pin down the exact semantics
// of the old inline canTask SMA-TX block before it was extracted, so a
// future change to decide() that silently changes wire behaviour fails
// here first.

#include <unity.h>
#include "StatusFrame.h"

using StatusFrame::ControlState;
using StatusFrame::Decision;
using StatusFrame::Snapshot;
using StatusFrame::decide;

static SystemConfig cfg;

void setUp(void)
{
    cfg = SystemConfig{};
    // Same charge/discharge shape as test_glideslope's setUp(), so the CCL/
    // DCL numbers below can be cross-checked against that file.
    cfg.maxChargeA = 100.0f;
    cfg.trickleA = 5.0f;
    cfg.cvStartTaper = 3.3f;
    cfg.cvHighAlarmGate = 3.4f;
    cfg.cvMaxCharge = 3.5f;
    cfg.maintAmps = 20.0f;

    cfg.maxDischargeA = 200.0f;
    cfg.limpDischargeA = 15.0f;
    cfg.cvStartDTaper = 3.2f;
    cfg.cvLowAlarmGate = 3.1f;
    cfg.cvMinDischarge = 3.0f;

    cfg.bmsTimeout = 60;

    // Deliberately far from cvStartTaper/cvHighAlarmGate/cvMaxCharge so the
    // maintenance-hysteresis tests don't interact with the taper thresholds.
    // Pack thresholds (x16 cells, StatusFrame::kCellCount): start 48.0V,
    // stop 51.2V.
    cfg.cvMaintStart = 3.0f;
    cfg.cvMaintStop = 3.2f;

    // spreadStartMv=60, spreadMaxMv=150 - left at the SystemConfig defaults,
    // same as test_glideslope.
}

void tearDown(void) {}

// Builds a Snapshot with both BMS reads fresh as of nowMs, cell voltages
// deep in the full-current region both ways (3.0V max / 3.3V min - below
// cvStartTaper, above cvStartDTaper), zero spread, no maintenance/reset
// request. Individual tests override only the fields they care about.
static Snapshot freshSnapshot(uint32_t nowMs)
{
    Snapshot s;
    s.nowMs = nowMs;
    s.haveBasicInfo = true;
    s.haveCellData = true;
    s.lastBasicInfoReadMs = nowMs;
    s.lastCellReadMs = nowMs;
    s.packVoltage = 55.0f; // above both maint thresholds -> autoMaint off
    s.packCurrent = 0.0f;
    s.packSOC = 50.0f;
    s.packTemp = 220;
    s.maxCellSmoothedV = 3.0f;
    s.maxCellRawV = 3.0f;
    s.minCellSmoothedV = 3.3f;
    s.minCellRawV = 3.3f;
    s.cellSpreadMv = 0;
    s.manualMaintForce = false;
    s.resetRequested = false;
    s.resetHoldStartMs = 0;
    return s;
}

// --- Gate: nothing sent before both BMS reads have succeeded once ---

void test_no_frames_before_basic_info(void)
{
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.haveBasicInfo = false;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_FALSE(ctrl.framesEnabled);
}

void test_no_frames_before_cell_data(void)
{
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.haveCellData = false;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_FALSE(ctrl.framesEnabled);
}

// --- Frames + full limits once fresh ---

void test_frames_full_limits_once_fresh(void)
{
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    Decision d = decide(cfg, s, ctrl);

    TEST_ASSERT_TRUE(d.sendFrames);
    TEST_ASSERT_TRUE(d.fresh);
    TEST_ASSERT_FALSE(d.maintenanceActive);
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

void test_first_frames_fires_exactly_once(void)
{
    ControlState ctrl;
    Decision d1 = decide(cfg, freshSnapshot(1000), ctrl);
    Decision d2 = decide(cfg, freshSnapshot(1250), ctrl);
    Decision d3 = decide(cfg, freshSnapshot(1500), ctrl);
    TEST_ASSERT_TRUE(d1.events.firstFrames);
    TEST_ASSERT_FALSE(d2.events.firstFrames);
    TEST_ASSERT_FALSE(d3.events.firstFrames);
}

// --- Staleness: 0A both ways, wentStale once, freshAgain once ---

void test_stale_forces_zero_then_recovers(void)
{
    ControlState ctrl;

    // t=1000: fresh, frames enabled, wasFresh -> true.
    Decision d1 = decide(cfg, freshSnapshot(1000), ctrl);
    TEST_ASSERT_TRUE(d1.fresh);
    TEST_ASSERT_FALSE(d1.events.wentStale);
    TEST_ASSERT_FALSE(d1.events.freshAgain);

    // t=62000: both stamps are 61s old (> cfg.bmsTimeout=60s) -> stale.
    // calculateCCL/DCL fail their bmsFresh check first and return 0
    // regardless of the (still full-current) cell voltages.
    Snapshot staleS = freshSnapshot(1000);
    staleS.nowMs = 62000;
    Decision d2 = decide(cfg, staleS, ctrl);
    TEST_ASSERT_FALSE(d2.fresh);
    TEST_ASSERT_EQUAL(0, d2.values.ccl);
    TEST_ASSERT_EQUAL(0, d2.values.dcl);
    TEST_ASSERT_TRUE(d2.events.wentStale);
    TEST_ASSERT_FALSE(d2.events.freshAgain);

    // Still stale next tick: wentStale must not fire again (edge-triggered).
    Snapshot stillStaleS = freshSnapshot(1000);
    stillStaleS.nowMs = 62250;
    Decision d3 = decide(cfg, stillStaleS, ctrl);
    TEST_ASSERT_FALSE(d3.fresh);
    TEST_ASSERT_FALSE(d3.events.wentStale);
    TEST_ASSERT_FALSE(d3.events.freshAgain);

    // Fresh again: new read stamps at t=62500.
    Decision d4 = decide(cfg, freshSnapshot(62500), ctrl);
    TEST_ASSERT_TRUE(d4.fresh);
    TEST_ASSERT_EQUAL(1000, d4.values.ccl); // limits restored
    TEST_ASSERT_FALSE(d4.events.wentStale);
    TEST_ASSERT_TRUE(d4.events.freshAgain);

    // And freshAgain must not repeat on the next fresh tick either.
    Decision d5 = decide(cfg, freshSnapshot(62750), ctrl);
    TEST_ASSERT_FALSE(d5.events.freshAgain);
}

// --- Reset hold: armed by the first sent frame, 5.5s, isResetting true throughout ---

void test_reset_not_armed_while_no_frames_sent(void)
{
    // Request arrives while the BMS has never reported - decide() must not
    // touch the hold timer (mirrors handleUIAction() arming
    // resetHoldStartTime=0, and canTask never getting to the reset-hold
    // code while !(haveBasicInfo && haveCellData)).
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.haveBasicInfo = false;
    s.resetRequested = true;
    s.resetHoldStartMs = 0;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_TRUE(d.isResetting);      // pass-through, unchanged
    TEST_ASSERT_EQUAL_UINT32(0, d.resetHoldStartMs); // still not armed
}

void test_reset_hold_arms_on_first_sent_frame_and_finishes_after_5500ms(void)
{
    ControlState ctrl;
    uint32_t resetHoldStartMs = 0;
    bool isResetting = true;

    // First tick with real data flowing, t=1000: hold arms here (was 0).
    Snapshot s1 = freshSnapshot(1000);
    s1.resetRequested = isResetting;
    s1.resetHoldStartMs = resetHoldStartMs;
    Decision d1 = decide(cfg, s1, ctrl);
    TEST_ASSERT_TRUE(d1.sendFrames);
    TEST_ASSERT_TRUE(d1.isResetting);
    TEST_ASSERT_EQUAL_UINT32(1000, d1.resetHoldStartMs); // armed at nowMs
    TEST_ASSERT_TRUE(d1.values.isResetting);
    TEST_ASSERT_FALSE(d1.events.resetFinished);
    // Simulate canTask writing the Decision back to the shared globals.
    isResetting = d1.isResetting;
    resetHoldStartMs = d1.resetHoldStartMs;

    // t=6499: 5499ms of hold (> not yet > 5500) -> still resetting.
    Snapshot s2 = freshSnapshot(6499);
    s2.resetRequested = isResetting;
    s2.resetHoldStartMs = resetHoldStartMs;
    Decision d2 = decide(cfg, s2, ctrl);
    TEST_ASSERT_TRUE(d2.isResetting);
    TEST_ASSERT_TRUE(d2.values.isResetting);
    TEST_ASSERT_FALSE(d2.events.resetFinished);
    isResetting = d2.isResetting;
    resetHoldStartMs = d2.resetHoldStartMs;

    // t=6501: 5501ms of hold (> 5500) -> finishes this tick.
    Snapshot s3 = freshSnapshot(6501);
    s3.resetRequested = isResetting;
    s3.resetHoldStartMs = resetHoldStartMs;
    Decision d3 = decide(cfg, s3, ctrl);
    TEST_ASSERT_FALSE(d3.isResetting);
    TEST_ASSERT_FALSE(d3.values.isResetting);
    TEST_ASSERT_TRUE(d3.events.resetFinished);
}

// --- Auto-maintenance hysteresis ---

void test_auto_maint_starts_below_start_stops_above_stop_no_toggle_between(void)
{
    // Thresholds: start 48.0V (cvMaintStart 3.0 x 16), stop 51.2V
    // (cvMaintStop 3.2 x 16).
    ControlState ctrl;

    // 55V: above both -> off.
    Decision d1 = decide(cfg, freshSnapshot(1000), ctrl); // packVoltage=55.0
    TEST_ASSERT_FALSE(d1.maintenanceActive);

    // 50V: between start and stop, autoMaint currently off -> stays off
    // (50 is not < 48).
    Snapshot s2 = freshSnapshot(1250);
    s2.packVoltage = 50.0f;
    Decision d2 = decide(cfg, s2, ctrl);
    TEST_ASSERT_FALSE(d2.maintenanceActive);

    // 47V: below start(48) -> turns on.
    Snapshot s3 = freshSnapshot(1500);
    s3.packVoltage = 47.0f;
    Decision d3 = decide(cfg, s3, ctrl);
    TEST_ASSERT_TRUE(d3.maintenanceActive);

    // 50V again: between start and stop, autoMaint now on -> hysteresis
    // keeps it on (50 is not > 51.2).
    Snapshot s4 = freshSnapshot(1750);
    s4.packVoltage = 50.0f;
    Decision d4 = decide(cfg, s4, ctrl);
    TEST_ASSERT_TRUE(d4.maintenanceActive);

    // 52V: above stop(51.2) -> turns off.
    Snapshot s5 = freshSnapshot(2000);
    s5.packVoltage = 52.0f;
    Decision d5 = decide(cfg, s5, ctrl);
    TEST_ASSERT_FALSE(d5.maintenanceActive);
}

void test_manual_force_overrides(void)
{
    // packVoltage=55V would leave autoMaint off; manualMaintForce alone
    // must still drive maintenanceActive.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.manualMaintForce = true;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_TRUE(d.maintenanceActive);
    TEST_ASSERT_TRUE(d.values.forceCharge);
}

void test_maintenance_overrides_cvl_and_current(void)
{
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.manualMaintForce = true;
    Decision d = decide(cfg, s, ctrl);

    TEST_ASSERT_TRUE(d.maintenanceActive);
    // CVL is forced to the fixed 560 (56.0V) maintenance value, not
    // cvMaxCharge(3.5) x 16 x 10 - which happens to also be 560 with this
    // cfg, so this is re-checked with a different cvMaxCharge below.
    TEST_ASSERT_EQUAL(560, d.values.cvl);
    // CCL = maintAmps(20A) -> 200; DCL is always 0 in maintenance (mirrors
    // test_glideslope's test_dcl_zero_in_maintenance).
    TEST_ASSERT_EQUAL(200, d.values.ccl);
    TEST_ASSERT_EQUAL(0, d.values.dcl);
}

void test_maintenance_cvl_is_fixed_560_not_cvmaxcharge(void)
{
    // With cvMaxCharge changed so cvMaxCharge*16*10 != 560, the maintenance
    // CVL must still read 560 - proves it's the fixed override, not the
    // normal formula.
    ControlState ctrl;
    cfg.cvMaxCharge = 3.6f; // normal CVL would be 576, not 560
    Snapshot s = freshSnapshot(1000);
    s.manualMaintForce = true;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(560, d.values.cvl);

    // And confirm the non-maintenance CVL DOES follow cvMaxCharge, for
    // contrast.
    ControlState ctrl2;
    Snapshot s2 = freshSnapshot(1000);
    Decision d2 = decide(cfg, s2, ctrl2);
    TEST_ASSERT_EQUAL(576, d2.values.cvl);
}

// --- Cell-spread derating ---

void test_derate_factor_applied_once_ccl_at_spread_midpoint_is_half(void)
{
    // spreadMv=105 is the midpoint of the default 60..150 span -> factor
    // 0.5. maxCellSmoothedV/RawV=3.0V is below cvStartTaper -> full-current
    // branch: 100A * 0.5 = 50A -> 500. Same as test_glideslope's
    // test_ccl_derated_by_spread_at_midpoint.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.cellSpreadMv = 105;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(500, d.values.ccl);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, d.derateFactor);
}

void test_derating_started_once_ended_only_after_hysteresis(void)
{
    // spreadStartMv=60 (default) -> "ended" requires spread <= 50.
    ControlState ctrl;

    // t=1000, spread=0: below start -> no derating, no event (baseline).
    Snapshot s1 = freshSnapshot(1000);
    s1.cellSpreadMv = 0;
    Decision d1 = decide(cfg, s1, ctrl);
    TEST_ASSERT_FALSE(d1.events.deratingStarted);
    TEST_ASSERT_FALSE(ctrl.derating);

    // t=1250, spread=100 (60 < 100 < 150) -> derating starts.
    Snapshot s2 = freshSnapshot(1250);
    s2.cellSpreadMv = 100;
    Decision d2 = decide(cfg, s2, ctrl);
    TEST_ASSERT_TRUE(d2.events.deratingStarted);
    TEST_ASSERT_TRUE(ctrl.derating);

    // t=1500, spread=55: factor back to 1.0 (55 <= startMv 60), but
    // 55 > 60-10=50, so NOT far enough below start -> deratingEnded must
    // NOT fire yet (hysteresis).
    Snapshot s3 = freshSnapshot(1500);
    s3.cellSpreadMv = 55;
    Decision d3 = decide(cfg, s3, ctrl);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, d3.derateFactor);
    TEST_ASSERT_FALSE(d3.events.deratingEnded);
    TEST_ASSERT_TRUE(ctrl.derating); // still considered derated

    // t=1750, spread=45 (<= 50) -> deratingEnded fires now.
    Snapshot s4 = freshSnapshot(1750);
    s4.cellSpreadMv = 45;
    Decision d4 = decide(cfg, s4, ctrl);
    TEST_ASSERT_TRUE(d4.events.deratingEnded);
    TEST_ASSERT_FALSE(ctrl.derating);

    // t=2000, spread=45 again: deratingEnded must not repeat.
    Snapshot s5 = freshSnapshot(2000);
    s5.cellSpreadMv = 45;
    Decision d5 = decide(cfg, s5, ctrl);
    TEST_ASSERT_FALSE(d5.events.deratingEnded);
    TEST_ASSERT_FALSE(d5.events.deratingStarted);
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
    RUN_TEST(test_auto_maint_starts_below_start_stops_above_stop_no_toggle_between);
    RUN_TEST(test_manual_force_overrides);
    RUN_TEST(test_maintenance_overrides_cvl_and_current);
    RUN_TEST(test_maintenance_cvl_is_fixed_560_not_cvmaxcharge);
    RUN_TEST(test_derate_factor_applied_once_ccl_at_spread_midpoint_is_half);
    RUN_TEST(test_derating_started_once_ended_only_after_hysteresis);
    return UNITY_END();
}
