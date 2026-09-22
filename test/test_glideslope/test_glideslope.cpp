// Native unit tests for the glideslope math: `pio test -e native`.
// Includes the real include/Glideslope.h - no mirrored copy to keep in sync.

#include <unity.h>
#include "Glideslope.h"

using Glideslope::calculateCCL;
using Glideslope::calculateDCL;
using Glideslope::isFresh;
using Glideslope::spreadFactor;

static SystemConfig cfg;

void setUp(void)
{
    cfg = SystemConfig{};
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
}

void tearDown(void) {}

// --- CCL taper (carried over from the old device-side test) ---

void test_ccl_full_below_taper(void) { TEST_ASSERT_EQUAL(1000, calculateCCL(cfg, 3.0f, 3.0f, 0, true, false)); }
void test_ccl_full_at_taper_start(void) { TEST_ASSERT_EQUAL(1000, calculateCCL(cfg, 3.3f, 3.3f, 0, true, false)); }
void test_ccl_trickle_at_alarm_gate(void) { TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.4f, 3.4f, 0, true, false)); }
void test_ccl_mid_taper(void)
{
    // midpoint of 3.3..3.4 -> average of 100A and 5A = 52.5A
    TEST_ASSERT_EQUAL(525, calculateCCL(cfg, 3.35f, 3.35f, 0, true, false));
}
void test_ccl_zero_at_hard_max(void) { TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.5f, 3.5f, 0, true, false)); }
void test_ccl_maintenance_overrides(void) { TEST_ASSERT_EQUAL(200, calculateCCL(cfg, 3.0f, 3.0f, 0, true, true)); }

// --- DCL taper ---

void test_dcl_full_above_taper(void) { TEST_ASSERT_EQUAL(2000, calculateDCL(cfg, 3.3f, 3.3f, 0, true, false)); }
void test_dcl_limp_at_alarm_gate(void) { TEST_ASSERT_EQUAL(150, calculateDCL(cfg, 3.1f, 3.1f, 0, true, false)); }
void test_dcl_mid_taper(void)
{
    // midpoint of 3.1..3.2 -> average of 200A and 15A = 107.5A
    TEST_ASSERT_EQUAL(1075, calculateDCL(cfg, 3.15f, 3.15f, 0, true, false));
}
void test_dcl_zero_at_hard_min(void) { TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.0f, 3.0f, 0, true, false)); }
void test_dcl_zero_in_maintenance(void) { TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.3f, 3.3f, 0, true, true)); }

// --- Raw vs. smoothed split (#9): hard cutoff/gate use raw, taper uses smoothed ---

void test_ccl_zero_when_raw_above_max_but_smoothed_below(void)
{
    // rawMaxV(3.5) >= cvMaxCharge(3.5) -> 0A, even though smoothedMaxV(3.35)
    // is only mid-taper and would otherwise read ~52.5A. This is the #9
    // scenario: a fast per-cell spike the moving average hasn't caught up to.
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.35f, 3.5f, 0, true, false));
}

void test_ccl_trickle_when_raw_at_gate_but_smoothed_below_taper(void)
{
    // rawMaxV(3.4) >= cvHighAlarmGate(3.4) -> trickle, even though
    // smoothedMaxV(3.2) is at/below cvStartTaper(3.3) and would otherwise
    // read full maxChargeA(100A).
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.2f, 3.4f, 0, true, false));
}

void test_ccl_trickle_when_smoothed_at_gate_but_raw_below(void)
{
    // rawMaxV(3.3) is below both cvMaxCharge and cvHighAlarmGate, so the
    // raw checks don't fire; the taper then runs on smoothedMaxV(3.4):
    // slope = (cvHighAlarmGate(3.4) - smoothedMaxV(3.4)) / 0.1 = 0 ->
    // target = trickleA(5) -> 50. This is the documented clamp: once the
    // smoothed value has reached the gate, the taper can't give back more
    // than trickle even if the raw value has since dropped.
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.4f, 3.3f, 0, true, false));
}

void test_ccl_taper_uses_smoothed_not_raw(void)
{
    // Neither raw check fires (rawMaxV 3.30 is below both cvMaxCharge and
    // cvHighAlarmGate). The taper then uses smoothedMaxV(3.35), the same
    // midpoint as test_ccl_mid_taper -> 52.5A -> 525, unaffected by raw
    // being 50mV lower.
    TEST_ASSERT_EQUAL(525, calculateCCL(cfg, 3.35f, 3.30f, 0, true, false));
}

void test_dcl_zero_when_raw_below_min_but_smoothed_above(void)
{
    // rawMinV(3.0) <= cvMinDischarge(3.0) -> 0A, even though
    // smoothedMinV(3.15) is mid-taper and would otherwise read ~107.5A.
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.15f, 3.0f, 0, true, false));
}

void test_dcl_limp_when_raw_at_gate_but_smoothed_above_taper(void)
{
    // rawMinV(3.1) <= cvLowAlarmGate(3.1) -> limp, even though
    // smoothedMinV(3.3) is above cvStartDTaper(3.2) and would otherwise
    // read full maxDischargeA(200A).
    TEST_ASSERT_EQUAL(150, calculateDCL(cfg, 3.3f, 3.1f, 0, true, false));
}

void test_dcl_limp_when_smoothed_at_gate_but_raw_above(void)
{
    // rawMinV(3.15) is above both cvMinDischarge and cvLowAlarmGate, so the
    // raw checks don't fire; the taper then runs on smoothedMinV(3.1):
    // slope = (smoothedMinV(3.1) - cvLowAlarmGate(3.1)) / 0.1 = 0 ->
    // target = limpDischargeA(15) -> 150. Mirror of the CCL clamp case.
    TEST_ASSERT_EQUAL(150, calculateDCL(cfg, 3.1f, 3.15f, 0, true, false));
}

void test_dcl_taper_uses_smoothed_not_raw(void)
{
    // Neither raw check fires (rawMinV 3.20 is above both cvMinDischarge and
    // cvLowAlarmGate). The taper then uses smoothedMinV(3.15), the same
    // midpoint as test_dcl_mid_taper -> 107.5A -> 1075, unaffected by raw
    // being 50mV higher.
    TEST_ASSERT_EQUAL(1075, calculateDCL(cfg, 3.15f, 3.20f, 0, true, false));
}

void test_nan_raw_voltage_is_zero(void)
{
    // A NaN raw reading (e.g. a corrupted latest BMS read) must force 0A
    // even when the smoothed value is otherwise fine.
    float nanV = NAN;
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.0f, nanV, 0, true, false));
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.3f, nanV, 0, true, false));
}

// --- Cell spread derating (#24) ---
// setUp() leaves spreadStartMv/spreadMaxMv at their SystemConfig defaults:
// 60 and 150.

void test_spread_factor_at_start(void)
{
    // At (or below) startMv, no derating: 1.0.
    TEST_ASSERT_EQUAL_FLOAT(1.0f, spreadFactor(60, 60, 150));
}

void test_spread_factor_at_max(void)
{
    // At (or above) maxMv, fully derated: 0.0.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, spreadFactor(150, 60, 150));
}

void test_spread_factor_midpoint(void)
{
    // 105mV is the midpoint of 60..150 -> 1.0 - (105-60)/(150-60)
    //   = 1.0 - 45/90 = 0.5.
    TEST_ASSERT_EQUAL_FLOAT(0.5f, spreadFactor(105, 60, 150));
}

void test_spread_factor_degenerate_config(void)
{
    // maxMv(50) <= startMv(100): treated as a step, not a division by a
    // non-positive span - 1.0 strictly below startMv, 0.0 at/above it.
    TEST_ASSERT_EQUAL_FLOAT(1.0f, spreadFactor(50, 100, 50));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, spreadFactor(100, 100, 50));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, spreadFactor(150, 100, 50));
}

void test_ccl_derated_by_spread_at_midpoint(void)
{
    // 3.0V is below cvStartTaper(3.3) -> full-current branch. spread=105mV
    // is the start..max midpoint -> factor 0.5. 100A * 0.5 = 50A -> 500,
    // still well above trickleA(5A) so the clamp doesn't bite.
    TEST_ASSERT_EQUAL(500, calculateCCL(cfg, 3.0f, 3.0f, 105, true, false));
}

void test_ccl_derated_by_spread_never_below_trickle(void)
{
    // spread=150mV = spreadMaxMv -> factor 0.0. 100A * 0.0 = 0A, but
    // fmaxf(0, trickleA(5)) clamps back up to trickle -> 50.
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.0f, 3.0f, 150, true, false));
}

void test_dcl_derated_by_spread_at_midpoint(void)
{
    // 3.3V is above cvStartDTaper(3.2) -> full-current branch (mirror of
    // test_dcl_full_above_taper). factor 0.5 -> 200A * 0.5 = 100A -> 1000.
    TEST_ASSERT_EQUAL(1000, calculateDCL(cfg, 3.3f, 3.3f, 105, true, false));
}

void test_dcl_derated_by_spread_never_below_limp(void)
{
    // factor 0.0 -> 200A * 0.0 = 0A, but fmaxf(0, limpDischargeA(15))
    // clamps back up to limp -> 150.
    TEST_ASSERT_EQUAL(150, calculateDCL(cfg, 3.3f, 3.3f, 150, true, false));
}

void test_ccl_gate_unaffected_by_spread(void)
{
    // rawMaxV(3.4) >= cvHighAlarmGate(3.4) -> trickle is returned before
    // the spread factor is even computed - a huge spread (200mV, well past
    // spreadMaxMv) changes nothing. Same value as test_ccl_trickle_at_alarm_gate.
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.4f, 3.4f, 200, true, false));
}

void test_ccl_hard_cutoff_unaffected_by_spread(void)
{
    // rawMaxV(3.5) >= cvMaxCharge(3.5) -> 0A regardless of spread.
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.5f, 3.5f, 200, true, false));
}

void test_ccl_maintenance_unaffected_by_spread(void)
{
    // maintenanceActive short-circuits before the spread factor is
    // computed -> still maintAmps(20A) -> 200, not derated.
    TEST_ASSERT_EQUAL(200, calculateCCL(cfg, 3.0f, 3.0f, 200, true, true));
}

void test_dcl_gate_unaffected_by_spread(void)
{
    // rawMinV(3.1) <= cvLowAlarmGate(3.1) -> limp, unaffected by spread.
    // Same value as test_dcl_limp_at_alarm_gate.
    TEST_ASSERT_EQUAL(150, calculateDCL(cfg, 3.1f, 3.1f, 200, true, false));
}

void test_dcl_hard_cutoff_unaffected_by_spread(void)
{
    // rawMinV(3.0) <= cvMinDischarge(3.0) -> 0A regardless of spread.
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.0f, 3.0f, 200, true, false));
}

void test_dcl_maintenance_unaffected_by_spread(void)
{
    // maintenanceActive short-circuits before the spread factor is
    // computed -> still 0A (DCL is always 0 in maintenance), unaffected.
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.3f, 3.3f, 200, true, true));
}

// --- Boundary values, clamp path, rounding and degenerate-config guard ---

void test_ccl_just_above_taper_start(void)
{
    // 3.31V: slope = (3.4-3.31)/0.1 = 0.9 -> target = 5 + 0.9*(100-5)
    //        = 5 + 85.5 = 90.5A -> 905 (confirmed against a native float
    //        build: no rounding surprise at this point).
    TEST_ASSERT_EQUAL(905, calculateCCL(cfg, 3.31f, 3.31f, 0, true, false));
}

void test_ccl_trickle_between_gate_and_max(void)
{
    // 3.45V is above cvHighAlarmGate(3.4) and below cvMaxCharge(3.5) -> trickle.
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.45f, 3.45f, 0, true, false));
}

void test_ccl_zero_above_hard_max(void)
{
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.6f, 3.6f, 0, true, false));
}

void test_dcl_full_at_taper_start_boundary(void)
{
    // `minCellV < cvStartDTaper` is false when equal (3.2 < 3.2 is false),
    // so 3.2V exactly falls through to the final `return maxDischargeA`
    // -> full 200A -> 2000. This is the `<` boundary named in the review.
    TEST_ASSERT_EQUAL(2000, calculateDCL(cfg, 3.2f, 3.2f, 0, true, false));
}

void test_dcl_just_below_taper_start(void)
{
    // 3.19V: slope = (3.19-3.1)/0.1 = 0.9 -> target = 15 + 0.9*(200-15)
    //        = 15 + 166.5 = 181.5A -> 1815 (confirmed against a native
    //        float build: no rounding surprise at this point either).
    TEST_ASSERT_EQUAL(1815, calculateDCL(cfg, 3.19f, 3.19f, 0, true, false));
}

void test_dcl_limp_between_min_and_gate(void)
{
    // 3.05V is above cvMinDischarge(3.0) and at/below cvLowAlarmGate(3.1) -> limp.
    TEST_ASSERT_EQUAL(150, calculateDCL(cfg, 3.05f, 3.05f, 0, true, false));
}

void test_dcl_zero_below_hard_min(void)
{
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 2.9f, 2.9f, 0, true, false));
}

void test_ccl_equal_gate_and_taper_still_trickle(void)
{
    // A naive "cvHighAlarmGate == cvStartTaper" degenerate config does NOT
    // actually exercise the `div <= 0.0001f` guard: any voltage >= the
    // (now-shared) threshold is caught by the earlier
    // `maxCellV >= cfg.cvHighAlarmGate` check first, so the taper branch
    // (and its guard) is never entered. Still correct - trickle - just via
    // a different code path than the guard.
    cfg.cvHighAlarmGate = 3.3f;
    cfg.cvStartTaper = 3.3f;
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.35f, 3.35f, 0, true, false));
}

void test_ccl_degenerate_taper_guard(void)
{
    // To actually exercise the `div <= 0.0001f` guard we need
    // cvStartTaper < maxCellV < cvHighAlarmGate (so neither boundary check
    // above fires first) with the gate/taper gap itself <= 0.0001f. Build
    // the thresholds from an epsilon offset rather than decimal literals so
    // the ordering is exact regardless of how the literals themselves round.
    cfg.cvStartTaper = 3.3f;
    cfg.cvHighAlarmGate = cfg.cvStartTaper + 0.00005f; // gap 0.00005 <= 0.0001f
    float v = cfg.cvStartTaper + 0.00002f;             // strictly between
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, v, v, 0, true, false));
}

void test_dcl_degenerate_taper_guard(void)
{
    // Mirrors test_ccl_degenerate_taper_guard. (A first guess that this
    // guard is unreachable for DCL - because "gate == taper" doesn't reach
    // it, same as CCL above - doesn't hold up under the same epsilon-gap
    // construction used for CCL: the earlier `minCellV <= cvLowAlarmGate`
    // check only intercepts when the gap is exactly zero or negative, not
    // when it's merely tiny, so this guard IS reachable with sane-direction,
    // near-equal thresholds.)
    cfg.cvLowAlarmGate = 3.1f;
    cfg.cvStartDTaper = cfg.cvLowAlarmGate + 0.00005f;
    float v = cfg.cvLowAlarmGate + 0.00002f;
    TEST_ASSERT_EQUAL(150, calculateDCL(cfg, v, v, 0, true, false));
}

void test_ccl_inverted_gate_taper_trickle(void)
{
    // Inverted config: gate(3.3) below taper(3.4). The
    // `maxCellV >= cfg.cvHighAlarmGate` check still fires first for any
    // voltage at/above the (lower) gate, so this never yields more than
    // trickle even though the config itself is nonsensical.
    cfg.cvHighAlarmGate = 3.3f;
    cfg.cvStartTaper = 3.4f;
    TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.35f, 3.35f, 0, true, false));
}

void test_ccl_clamp_when_trickle_exceeds_max(void)
{
    // trickleA(50) > maxChargeA(10): target = 50 + 0.5*(10-50) = 30, but
    // fmaxf(target, trickleA) clamps back up to 50 -> 500.
    cfg.trickleA = 50.0f;
    cfg.maxChargeA = 10.0f;
    TEST_ASSERT_EQUAL(500, calculateCCL(cfg, 3.35f, 3.35f, 0, true, false));
}

void test_dcl_clamp_when_limp_exceeds_max(void)
{
    // limpDischargeA(100) > maxDischargeA(20): target = 100 + 0.5*(20-100)
    // = 60, but fmaxf(target, limpDischargeA) clamps back up to 100 -> 1000.
    cfg.limpDischargeA = 100.0f;
    cfg.maxDischargeA = 20.0f;
    TEST_ASSERT_EQUAL(1000, calculateDCL(cfg, 3.15f, 3.15f, 0, true, false));
}

void test_ccl_rounding_artifact(void)
{
    // maxChargeA=12.35f is not exactly representable (its actual value is
    // 12.35000038...); multiplying by 10.0f in single precision gives
    // 123.50000038..., which rounds up to 124. This is an IEEE-754 single
    // precision artefact of the literal itself, not of the round() call -
    // it is identical on native (this test) and on the xtensa device build,
    // since both use IEEE binary32 float. Confirmed against a standalone
    // float build before pinning.
    cfg.maxChargeA = 12.35f;
    TEST_ASSERT_EQUAL(124, calculateCCL(cfg, 3.0f, 3.0f, 0, true, false));
}

void test_fresh_at_boot_time_zero(void)
{
    // A real read recorded at t=0 is fresh - only "never read"
    // (haveRead=false) must be stale at t=0, not haveRead=true at t=0.
    TEST_ASSERT_TRUE(isFresh(true, 5000, 0, 60));
    TEST_ASSERT_TRUE(isFresh(true, 0, 0, 0));
    TEST_ASSERT_FALSE(isFresh(true, 1, 0, 0));
}

// --- Fail-safe: NaN voltage or threshold forces 0A ---

void test_nan_voltage_is_zero(void)
{
    float nanV = NAN;
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, nanV, nanV, 0, true, false));
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, nanV, nanV, 0, true, false));
}

void test_nan_threshold_is_zero(void)
{
    cfg.cvHighAlarmGate = NAN;
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.3f, 3.3f, 0, true, false));

    cfg.cvLowAlarmGate = NAN;
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.15f, 3.15f, 0, true, false));
}

void test_nan_current_setpoint_is_zero(void)
{
    // round(NaN * 10) cast to uint16_t is undefined; the guard must catch
    // a NaN current setpoint too, including in maintenance mode.
    cfg.maxChargeA = NAN;
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.0f, 3.0f, 0, true, false));
    cfg = SystemConfig{}; setUp();
    cfg.maintAmps = NAN;
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.0f, 3.0f, 0, true, true));
    cfg = SystemConfig{}; setUp();
    cfg.limpDischargeA = NAN;
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.05f, 3.05f, 0, true, false));
}

// --- Fail-safe: no data / stale data forces 0A (#10) ---

void test_limits_zero_when_not_fresh(void)
{
    // Cell voltages that would otherwise mean full current both ways.
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.3f, 3.3f, 0, false, false));
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.3f, 3.3f, 0, false, false));
    // ...including maintenance, which would otherwise request maintAmps.
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.3f, 3.3f, 0, false, true));
}

void test_never_read_is_stale_right_after_boot(void)
{
    // The old check (millis() - 0 > timeout) passed for the first 60s after
    // boot; "no read yet" must be stale at any uptime.
    TEST_ASSERT_FALSE(isFresh(false, 10000, 0, 60));
    TEST_ASSERT_FALSE(isFresh(false, 0, 0, 60));
}

void test_fresh_within_timeout(void)
{
    TEST_ASSERT_TRUE(isFresh(true, 100000, 100000, 60));
    TEST_ASSERT_TRUE(isFresh(true, 160000, 100000, 60)); // exactly at the limit
    TEST_ASSERT_FALSE(isFresh(true, 160001, 100000, 60));
}

void test_fresh_across_millis_wraparound(void)
{
    // Read 1s before the 32-bit millis() wrap, checked 2s after it.
    TEST_ASSERT_TRUE(isFresh(true, 2000, 0xFFFFFFFFu - 999u, 60));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_ccl_full_below_taper);
    RUN_TEST(test_ccl_full_at_taper_start);
    RUN_TEST(test_ccl_trickle_at_alarm_gate);
    RUN_TEST(test_ccl_mid_taper);
    RUN_TEST(test_ccl_zero_at_hard_max);
    RUN_TEST(test_ccl_maintenance_overrides);
    RUN_TEST(test_dcl_full_above_taper);
    RUN_TEST(test_dcl_limp_at_alarm_gate);
    RUN_TEST(test_dcl_mid_taper);
    RUN_TEST(test_dcl_zero_at_hard_min);
    RUN_TEST(test_dcl_zero_in_maintenance);
    RUN_TEST(test_ccl_zero_when_raw_above_max_but_smoothed_below);
    RUN_TEST(test_ccl_trickle_when_raw_at_gate_but_smoothed_below_taper);
    RUN_TEST(test_ccl_trickle_when_smoothed_at_gate_but_raw_below);
    RUN_TEST(test_ccl_taper_uses_smoothed_not_raw);
    RUN_TEST(test_dcl_zero_when_raw_below_min_but_smoothed_above);
    RUN_TEST(test_dcl_limp_when_raw_at_gate_but_smoothed_above_taper);
    RUN_TEST(test_dcl_limp_when_smoothed_at_gate_but_raw_above);
    RUN_TEST(test_dcl_taper_uses_smoothed_not_raw);
    RUN_TEST(test_nan_raw_voltage_is_zero);
    RUN_TEST(test_spread_factor_at_start);
    RUN_TEST(test_spread_factor_at_max);
    RUN_TEST(test_spread_factor_midpoint);
    RUN_TEST(test_spread_factor_degenerate_config);
    RUN_TEST(test_ccl_derated_by_spread_at_midpoint);
    RUN_TEST(test_ccl_derated_by_spread_never_below_trickle);
    RUN_TEST(test_dcl_derated_by_spread_at_midpoint);
    RUN_TEST(test_dcl_derated_by_spread_never_below_limp);
    RUN_TEST(test_ccl_gate_unaffected_by_spread);
    RUN_TEST(test_ccl_hard_cutoff_unaffected_by_spread);
    RUN_TEST(test_ccl_maintenance_unaffected_by_spread);
    RUN_TEST(test_dcl_gate_unaffected_by_spread);
    RUN_TEST(test_dcl_hard_cutoff_unaffected_by_spread);
    RUN_TEST(test_dcl_maintenance_unaffected_by_spread);
    RUN_TEST(test_ccl_just_above_taper_start);
    RUN_TEST(test_ccl_trickle_between_gate_and_max);
    RUN_TEST(test_ccl_zero_above_hard_max);
    RUN_TEST(test_dcl_full_at_taper_start_boundary);
    RUN_TEST(test_dcl_just_below_taper_start);
    RUN_TEST(test_dcl_limp_between_min_and_gate);
    RUN_TEST(test_dcl_zero_below_hard_min);
    RUN_TEST(test_ccl_equal_gate_and_taper_still_trickle);
    RUN_TEST(test_ccl_degenerate_taper_guard);
    RUN_TEST(test_dcl_degenerate_taper_guard);
    RUN_TEST(test_ccl_inverted_gate_taper_trickle);
    RUN_TEST(test_ccl_clamp_when_trickle_exceeds_max);
    RUN_TEST(test_dcl_clamp_when_limp_exceeds_max);
    RUN_TEST(test_ccl_rounding_artifact);
    RUN_TEST(test_fresh_at_boot_time_zero);
    RUN_TEST(test_nan_voltage_is_zero);
    RUN_TEST(test_nan_threshold_is_zero);
    RUN_TEST(test_nan_current_setpoint_is_zero);
    RUN_TEST(test_limits_zero_when_not_fresh);
    RUN_TEST(test_never_read_is_stale_right_after_boot);
    RUN_TEST(test_fresh_within_timeout);
    RUN_TEST(test_fresh_across_millis_wraparound);
    return UNITY_END();
}
