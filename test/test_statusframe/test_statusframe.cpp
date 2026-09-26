// Native unit tests for the status-frame decision logic (#29):
// `pio test -e native`. Includes the real include/StatusFrame.h - no
// mirrored copy to keep in sync. These tests pin down the exact semantics
// of the old inline canTask SMA-TX block before it was extracted, so a
// future change to decide() that silently changes wire behaviour fails
// here first.

#include <unity.h>
#include "StatusFrame.h"
#include "GlideslopeFixture.h"

using StatusFrame::ControlState;
using StatusFrame::Decision;
using StatusFrame::Snapshot;
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
    s.packVoltage = 55.0f;
    s.packCurrent = 0.0f;
    s.packSOC = 50.0f;
    s.packTemp = 220;
    s.maxCellSmoothedV = 3.0f;
    s.maxCellRawV = 3.0f;
    s.minCellSmoothedV = 3.3f; // above both maint thresholds -> autoMaint off
    s.minCellRawV = 3.3f;
    s.cellSpreadMv = 0;
    s.manualMaintForce = false;
    s.resetRequested = false;
    s.resetHoldStartMs = 0;
    return s;
}

// --- Gate: nothing sent before both BMS reads have succeeded once ---

static void test_no_frames_before_basic_info(void)
{
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.haveBasicInfo = false;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_FALSE(ctrl.framesEnabled);
}

static void test_no_frames_before_cell_data(void)
{
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.haveCellData = false;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_FALSE(d.sendFrames);
    TEST_ASSERT_FALSE(ctrl.framesEnabled);
}

// --- Frames + full limits once fresh ---

static void test_frames_full_limits_once_fresh(void)
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

static void test_first_frames_fires_exactly_once(void)
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

static void test_stale_forces_zero_then_recovers(void)
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

static void test_reset_not_armed_while_no_frames_sent(void)
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

static void test_reset_hold_arms_on_first_sent_frame_and_finishes_after_5500ms(void)
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

static void test_auto_maint_starts_below_start_stops_above_stop_no_toggle_between(void)
{
    // Thresholds (#12: compared directly against the smoothed minimum
    // cell voltage): start cvMaintStart=3.05V, stop cvMaintStop=3.2V.
    ControlState ctrl;

    // 3.3V: above both -> off.
    Decision d1 = decide(cfg, freshSnapshot(1000), ctrl); // minCellSmoothedV=3.3
    TEST_ASSERT_FALSE(d1.maintenanceActive);

    // 3.1V: between start and stop, autoMaint currently off -> stays off
    // (3.1 is not < 3.05).
    Snapshot s2 = freshSnapshot(1250);
    s2.minCellSmoothedV = 3.1f;
    Decision d2 = decide(cfg, s2, ctrl);
    TEST_ASSERT_FALSE(d2.maintenanceActive);

    // 2.9V: below start(3.05) -> turns on.
    Snapshot s3 = freshSnapshot(1500);
    s3.minCellSmoothedV = 2.9f;
    Decision d3 = decide(cfg, s3, ctrl);
    TEST_ASSERT_TRUE(d3.maintenanceActive);

    // 3.1V again: between start and stop, autoMaint now on -> hysteresis
    // keeps it on (3.1 is not > 3.2).
    Snapshot s4 = freshSnapshot(1750);
    s4.minCellSmoothedV = 3.1f;
    Decision d4 = decide(cfg, s4, ctrl);
    TEST_ASSERT_TRUE(d4.maintenanceActive);

    // 3.3V: above stop(3.2) -> turns off.
    Snapshot s5 = freshSnapshot(2000);
    s5.minCellSmoothedV = 3.3f;
    Decision d5 = decide(cfg, s5, ctrl);
    TEST_ASSERT_FALSE(d5.maintenanceActive);
}

// --- #12: trigger on the minimum cell, not the pack average ---

static void test_auto_maint_starts_on_weak_cell_even_when_pack_average_is_high(void)
{
    // The exact scenario from #12: Cell 16 sags under discharge and hits
    // the discharge floor while the pack average is still well above the
    // old pack-voltage trigger (cvMaintStart(3.05) * kCellCount(16) =
    // 48.8V). packVoltage=49.6V > 48.8V, so the retired pack-average
    // comparison would never have started maintenance here; the minimum
    // cell (2.99V) is what's actually below cvMaintStart(3.05V).
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.packVoltage = 49.6f;
    s.minCellSmoothedV = 2.99f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_TRUE(d.maintenanceActive);
    TEST_ASSERT_TRUE(ctrl.autoMaint);
}

static void test_auto_maint_hysteresis_min_cell_rising_stays_on_until_above_stop(void)
{
    // Once started, rising back into the start..stop band must not turn
    // maintenance off; only crossing above cvMaintStop(3.2V) does.
    ControlState ctrl;

    // 2.95V: below start(3.05) -> turns on.
    Snapshot s1 = freshSnapshot(1000);
    s1.minCellSmoothedV = 2.95f;
    Decision d1 = decide(cfg, s1, ctrl);
    TEST_ASSERT_TRUE(d1.maintenanceActive);

    // 3.1V: between start(3.05) and stop(3.2), autoMaint on -> stays on
    // (3.1 is not > 3.2).
    Snapshot s2 = freshSnapshot(1250);
    s2.minCellSmoothedV = 3.1f;
    Decision d2 = decide(cfg, s2, ctrl);
    TEST_ASSERT_TRUE(d2.maintenanceActive);

    // 3.25V: above stop(3.2) -> turns off.
    Snapshot s3 = freshSnapshot(1500);
    s3.minCellSmoothedV = 3.25f;
    Decision d3 = decide(cfg, s3, ctrl);
    TEST_ASSERT_FALSE(d3.maintenanceActive);
}

static void test_auto_maint_never_starts_with_zero_min_cell(void)
{
    // minCellSmoothedV == 0 means "no data yet" (same sentinel used
    // elsewhere in Snapshot/Glideslope) - it must never satisfy
    // "< cvMaintStart" and start maintenance on a value nobody measured.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.minCellSmoothedV = 0.0f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_FALSE(d.maintenanceActive);
    TEST_ASSERT_FALSE(ctrl.autoMaint);
}

static void test_manual_force_overrides(void)
{
    // minCellSmoothedV=3.3V would leave autoMaint off; manualMaintForce
    // alone must still drive maintenanceActive.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.manualMaintForce = true;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_TRUE(d.maintenanceActive);
    TEST_ASSERT_TRUE(d.values.forceCharge);
}

static void test_maintenance_overrides_cvl_and_current(void)
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

static void test_maintenance_cvl_is_fixed_560_not_cvmaxcharge(void)
{
    // With cvMaxCharge changed so cvMaxCharge*16*10 != 560, the maintenance
    // CVL must still read 560 - proves it's the fixed override, not the
    // normal formula.
    ControlState ctrl;
    cfg.cvMaxCharge.setUnchecked(3.6f); // normal CVL would be 576, not 560
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

static void test_derate_factor_applied_once_ccl_at_spread_midpoint_is_half(void)
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

static void test_derating_started_once_ended_only_after_hysteresis(void)
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

// --- #71: raw/smoothed and min/max split, driven through decide() ---
// Mirrors the equivalent test_glideslope cases, but through decide() so a
// future change that mixes up which Snapshot field feeds which
// calculateCCL()/calculateDCL() argument (or which freshness flag gates
// which BMS reading) fails here too, not just at the pure-math layer.

static void test_raw_spike_reaches_ccl_through_decide(void)
{
    // smoothedMaxV=3.35V (taper region) but rawMaxV=3.50V == cvMaxCharge ->
    // hard cutoff fires on the raw reading regardless of the smoothed one.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.maxCellSmoothedV = 3.35f;
    s.maxCellRawV = 3.50f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(0, d.values.ccl);
    // DCL is untouched: minCellSmoothedV/RawV=3.3V (freshSnapshot default)
    // is above cvStartDTaper(3.2) -> full-current: 200A * 1.0 -> 2000.
    TEST_ASSERT_EQUAL(2000, d.values.dcl);
}

static void test_raw_spike_at_gate_gives_trickle_through_decide(void)
{
    // smoothedMaxV=3.35V (taper region), rawMaxV=3.45V: at/above
    // cvHighAlarmGate(3.4V) but below cvMaxCharge(3.5V) -> trickle on the
    // raw reading, not the taper's smoothed-value slope.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.maxCellSmoothedV = 3.35f;
    s.maxCellRawV = 3.45f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(50, d.values.ccl); // trickleA(5A) -> 50 (0.1A units)
}

static void test_raw_sag_reaches_dcl_through_decide(void)
{
    // smoothedMinV=3.15V (taper region) but rawMinV=3.00V == cvMinDischarge
    // -> hard cutoff fires on the raw reading.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.minCellSmoothedV = 3.15f;
    s.minCellRawV = 3.00f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(0, d.values.dcl);
    // CCL is untouched: maxCellSmoothedV/RawV=3.0V (freshSnapshot default)
    // is below cvStartTaper(3.3) -> full-current: 100A * 1.0 -> 1000.
    TEST_ASSERT_EQUAL(1000, d.values.ccl);
}

static void test_raw_sag_at_gate_gives_limp_through_decide(void)
{
    // smoothedMinV=3.15V (taper region), rawMinV=3.05V: at/below
    // cvLowAlarmGate(3.1V), above cvMinDischarge(3.0V) -> limp on the raw
    // reading.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.minCellSmoothedV = 3.15f;
    s.minCellRawV = 3.05f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(150, d.values.dcl); // limpDischargeA(15A) -> 150
}

static void test_ccl_trickle_when_smoothed_at_gate_but_raw_below_through_decide(void)
{
    // smoothedMaxV=3.42V is at/above cvHighAlarmGate(3.4V), but
    // rawMaxV=3.35V is below it (and below cvMaxCharge) -> the taper's
    // slope clamps to 0, so target == trickleA even though the raw
    // reading alone wouldn't have gated anything (Glideslope.h's
    // documented "intentional, not a bug" consequence of the raw/smoothed
    // split).
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.maxCellSmoothedV = 3.42f;
    s.maxCellRawV = 3.35f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(50, d.values.ccl); // trickleA(5A) -> 50
}

static void test_dcl_limp_when_smoothed_at_gate_but_raw_above_through_decide(void)
{
    // Mirror of the CCL case above: smoothedMinV=3.08V is at/below
    // cvLowAlarmGate(3.1V), but rawMinV=3.15V is above it (and above
    // cvMinDischarge) -> the taper's slope clamps to 0, target ==
    // limpDischargeA.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.minCellSmoothedV = 3.08f;
    s.minCellRawV = 3.15f;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(150, d.values.dcl); // limpDischargeA(15A) -> 150
}

static void test_dcl_derated_by_spread_through_decide(void)
{
    // spreadMv=105 is the midpoint of the default 60..150 span -> factor
    // 0.5. minCellSmoothedV/RawV=3.3V (freshSnapshot default) is above
    // cvStartDTaper(3.2) -> full-current branch: 200A * 0.5 = 100A -> 1000.
    // CCL gets the same factor via the freshSnapshot default max cell
    // (3.0V, full-current): 100A * 0.5 = 50A -> 500. Same spreadMv as
    // test_derate_factor_applied_once_ccl_at_spread_midpoint_is_half, just
    // asserting the DCL side too.
    ControlState ctrl;
    Snapshot s = freshSnapshot(1000);
    s.cellSpreadMv = 105;
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(500, d.values.ccl);
    TEST_ASSERT_EQUAL(1000, d.values.dcl);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, d.derateFactor);
}

static void test_half_stale_cells_stale_basic_info_fresh_forces_zero(void)
{
    // haveBasicInfo fresh, haveCellData stale (last read further back than
    // cfg.bmsTimeout(60s)=60000ms) -> decide()'s `fresh` is the AND of
    // both, so both limits go to 0.
    ControlState ctrl;
    Snapshot s = freshSnapshot(100000);
    s.lastCellReadMs = 100000 - 70000; // 70s ago, > 60s timeout
    Decision d = decide(cfg, s, ctrl);
    TEST_ASSERT_EQUAL(0, d.values.ccl);
    TEST_ASSERT_EQUAL(0, d.values.dcl);
    TEST_ASSERT_FALSE(d.fresh);
}

static void test_half_stale_basic_info_stale_cells_fresh_forces_zero(void)
{
    // Reverse of the above: haveCellData fresh, haveBasicInfo stale ->
    // same AND, same result.
    ControlState ctrl;
    Snapshot s = freshSnapshot(100000);
    s.lastBasicInfoReadMs = 100000 - 70000; // 70s ago, > 60s timeout
    Decision d = decide(cfg, s, ctrl);
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
            Snapshot s = freshSnapshot(t);
            if (phase % 2 == 1)
                s.lastCellReadMs = t - 70000; // > 60 s bmsTimeout
            Decision d = decide(cfg, s, ctrl);
            wentStale += d.events.wentStale;
            freshAgain += d.events.freshAgain;
            TEST_ASSERT_EQUAL(phase % 2 == 0, d.fresh);
        }
    TEST_ASSERT_EQUAL(2, wentStale);
    TEST_ASSERT_EQUAL(2, freshAgain);
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
    RUN_TEST(test_auto_maint_starts_on_weak_cell_even_when_pack_average_is_high);
    RUN_TEST(test_auto_maint_hysteresis_min_cell_rising_stays_on_until_above_stop);
    RUN_TEST(test_auto_maint_never_starts_with_zero_min_cell);
    RUN_TEST(test_manual_force_overrides);
    RUN_TEST(test_maintenance_overrides_cvl_and_current);
    RUN_TEST(test_maintenance_cvl_is_fixed_560_not_cvmaxcharge);
    RUN_TEST(test_derate_factor_applied_once_ccl_at_spread_midpoint_is_half);
    RUN_TEST(test_derating_started_once_ended_only_after_hysteresis);
    RUN_TEST(test_raw_spike_reaches_ccl_through_decide);
    RUN_TEST(test_raw_spike_at_gate_gives_trickle_through_decide);
    RUN_TEST(test_raw_sag_reaches_dcl_through_decide);
    RUN_TEST(test_raw_sag_at_gate_gives_limp_through_decide);
    RUN_TEST(test_ccl_trickle_when_smoothed_at_gate_but_raw_below_through_decide);
    RUN_TEST(test_dcl_limp_when_smoothed_at_gate_but_raw_above_through_decide);
    RUN_TEST(test_dcl_derated_by_spread_through_decide);
    RUN_TEST(test_half_stale_cells_stale_basic_info_fresh_forces_zero);
    RUN_TEST(test_half_stale_basic_info_stale_cells_fresh_forces_zero);
    RUN_TEST(test_stale_fresh_cycles_each_event_once);
    return UNITY_END();
}
