// Native unit tests for the glideslope math: `pio test -e native`.
// Includes the real include/Glideslope.h - no mirrored copy to keep in sync.

#include <unity.h>
#include "Glideslope.h"

using Glideslope::calculateCCL;
using Glideslope::calculateDCL;
using Glideslope::isFresh;

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

void test_ccl_full_below_taper(void) { TEST_ASSERT_EQUAL(1000, calculateCCL(cfg, 3.0f, true, false)); }
void test_ccl_full_at_taper_start(void) { TEST_ASSERT_EQUAL(1000, calculateCCL(cfg, 3.3f, true, false)); }
void test_ccl_trickle_at_alarm_gate(void) { TEST_ASSERT_EQUAL(50, calculateCCL(cfg, 3.4f, true, false)); }
void test_ccl_mid_taper(void)
{
    // midpoint of 3.3..3.4 -> average of 100A and 5A = 52.5A
    TEST_ASSERT_EQUAL(525, calculateCCL(cfg, 3.35f, true, false));
}
void test_ccl_zero_at_hard_max(void) { TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.5f, true, false)); }
void test_ccl_maintenance_overrides(void) { TEST_ASSERT_EQUAL(200, calculateCCL(cfg, 3.0f, true, true)); }

// --- DCL taper ---

void test_dcl_full_above_taper(void) { TEST_ASSERT_EQUAL(2000, calculateDCL(cfg, 3.3f, true, false)); }
void test_dcl_limp_at_alarm_gate(void) { TEST_ASSERT_EQUAL(150, calculateDCL(cfg, 3.1f, true, false)); }
void test_dcl_mid_taper(void)
{
    // midpoint of 3.1..3.2 -> average of 200A and 15A = 107.5A
    TEST_ASSERT_EQUAL(1075, calculateDCL(cfg, 3.15f, true, false));
}
void test_dcl_zero_at_hard_min(void) { TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.0f, true, false)); }
void test_dcl_zero_in_maintenance(void) { TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.3f, true, true)); }

// --- Fail-safe: NaN voltage or threshold forces 0A ---

void test_nan_voltage_is_zero(void)
{
    float nanV = NAN;
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, nanV, true, false));
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, nanV, true, false));
}

void test_nan_threshold_is_zero(void)
{
    cfg.cvHighAlarmGate = NAN;
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.3f, true, false));

    cfg.cvLowAlarmGate = NAN;
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.15f, true, false));
}

// --- Fail-safe: no data / stale data forces 0A (#10) ---

void test_limits_zero_when_not_fresh(void)
{
    // Cell voltages that would otherwise mean full current both ways.
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.3f, false, false));
    TEST_ASSERT_EQUAL(0, calculateDCL(cfg, 3.3f, false, false));
    // ...including maintenance, which would otherwise request maintAmps.
    TEST_ASSERT_EQUAL(0, calculateCCL(cfg, 3.3f, false, true));
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
    RUN_TEST(test_nan_voltage_is_zero);
    RUN_TEST(test_nan_threshold_is_zero);
    RUN_TEST(test_limits_zero_when_not_fresh);
    RUN_TEST(test_never_read_is_stale_right_after_boot);
    RUN_TEST(test_fresh_within_timeout);
    RUN_TEST(test_fresh_across_millis_wraparound);
    return UNITY_END();
}
