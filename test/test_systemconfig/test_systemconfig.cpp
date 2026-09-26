// Native unit tests for SystemConfig::validate() (#53): `pio test -e native`.
// Includes the real include/SystemConfig.h - no mirrored copy to keep in
// sync, same pattern as test_glideslope/test_statusframe.

#include <unity.h>
#include <math.h>
#include "SystemConfig.h"

using ValidationResult = SystemConfig::ValidationResult;

// A realistic, fully-passing baseline config. Individual tests copy this and
// nudge exactly one field to its boundary.
static SystemConfig baseline()
{
    SystemConfig cfg{};
    cfg.maxChargeA = 250.0f;
    cfg.maxDischargeA = 500.0f;
    cfg.cvStartTaper = 3.40f;
    cfg.cvHighAlarmGate = 3.45f;
    cfg.cvMaxCharge = 3.55f;
    cfg.cvStartDTaper = 3.10f;
    cfg.cvLowAlarmGate = 3.00f;
    cfg.cvMinDischarge = 2.90f;
    cfg.cvMaintStart = 3.00f;
    cfg.cvMaintStop = 3.10f;
    cfg.trickleA = 2.0f;
    cfg.limpDischargeA = 15.0f;
    cfg.maintAmps = 20.0f;
    cfg.vSamples = 12;
    cfg.bmsTimeout = 60;
    return cfg;
}

void setUp(void) {}
void tearDown(void) {}

static void test_baseline_is_ok(void)
{
    SystemConfig cfg = baseline();
    ValidationResult r = SystemConfig::validate(cfg);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_FALSE(r.hasNaN);
    TEST_ASSERT_FALSE(r.maintStartBelowMinDischarge);
    TEST_ASSERT_FALSE(r.chargeTaperOrderBad);
    TEST_ASSERT_FALSE(r.dischargeTaperOrderBad);
    TEST_ASSERT_FALSE(r.maintHysteresisBad);
    TEST_ASSERT_FALSE(r.chargeHeadroomBad);
    TEST_ASSERT_FALSE(r.hasNegativeCurrent);
}

// #12: cvMaintStart <= cvMinDischarge must be rejected; one step above must
// pass.
static void test_maint_start_below_min_discharge_boundary(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMinDischarge = 2.90f;

    cfg.cvMaintStart = 2.90f; // equal -> rejected
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).maintStartBelowMinDischarge);

    cfg.cvMaintStart = 2.90f - 0.01f; // below -> rejected
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).maintStartBelowMinDischarge);

    cfg.cvMaintStart = 2.90f + 0.01f; // above -> passes this check
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).maintStartBelowMinDischarge);
}

// Charge taper order: cvStartTaper < cvHighAlarmGate < cvMaxCharge, strict.
static void test_charge_taper_order_boundary(void)
{
    SystemConfig cfg = baseline();

    // cvStartTaper == cvHighAlarmGate -> bad (not strictly less).
    cfg.cvStartTaper = cfg.cvHighAlarmGate;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeTaperOrderBad);

    // One step on the good side -> passes.
    cfg = baseline();
    cfg.cvStartTaper = cfg.cvHighAlarmGate - 0.01f;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).chargeTaperOrderBad);

    // cvHighAlarmGate == cvMaxCharge -> bad.
    cfg = baseline();
    cfg.cvHighAlarmGate = cfg.cvMaxCharge;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeTaperOrderBad);
}

// Discharge taper order: cvStartDTaper > cvLowAlarmGate > cvMinDischarge,
// strict.
static void test_discharge_taper_order_boundary(void)
{
    SystemConfig cfg = baseline();

    // cvStartDTaper == cvLowAlarmGate -> bad (not strictly greater).
    cfg.cvStartDTaper = cfg.cvLowAlarmGate;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).dischargeTaperOrderBad);

    cfg = baseline();
    cfg.cvStartDTaper = cfg.cvLowAlarmGate + 0.01f; // good side
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).dischargeTaperOrderBad);

    cfg = baseline();
    cfg.cvLowAlarmGate = cfg.cvMinDischarge; // equal -> bad
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).dischargeTaperOrderBad);
}

// Maintenance hysteresis: cvMaintStart >= cvMaintStop is bad.
static void test_maint_hysteresis_boundary(void)
{
    SystemConfig cfg = baseline();

    cfg.cvMaintStop = cfg.cvMaintStart; // equal -> bad
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).maintHysteresisBad);

    cfg = baseline();
    cfg.cvMaintStop = cfg.cvMaintStart + 0.01f; // one step above -> good
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).maintHysteresisBad);
}

// #54/#8: 3.550f must pass (the live deployed practical ceiling), 3.551f
// must fail (meaningfully above the margin).
static void test_charge_headroom_boundary(void)
{
    SystemConfig cfg = baseline();

    cfg.cvMaxCharge = 3.550f;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).chargeHeadroomBad);

    cfg.cvMaxCharge = 3.551f;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeHeadroomBad);

    cfg.cvMaxCharge = 3.5505f; // just above the ceiling, no slack
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeHeadroomBad);
}

// Negative current setpoints: 0 A allowed, any negative value rejected, one
// field at a time.
static void test_negative_current_boundary(void)
{
    SystemConfig cfg = baseline();

    cfg.trickleA = 0.0f;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).hasNegativeCurrent);
    cfg.trickleA = -0.01f;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).hasNegativeCurrent);

    cfg = baseline();
    cfg.limpDischargeA = -1.0f;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).hasNegativeCurrent);

    cfg = baseline();
    cfg.maintAmps = -1.0f;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).hasNegativeCurrent);

    cfg = baseline();
    cfg.maxChargeA = -1.0f;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).hasNegativeCurrent);

    cfg = baseline();
    cfg.maxDischargeA = -1.0f;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).hasNegativeCurrent);
}

// Several violations at once must all surface together (no early return
// except for hasNaN).
static void test_combined_violations_all_surface(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMaxCharge = 3.6f;                        // chargeHeadroomBad
    cfg.cvHighAlarmGate = cfg.cvMaxCharge;         // chargeTaperOrderBad (set after cvMaxCharge above)
    cfg.cvMaintStart = cfg.cvMinDischarge;        // maintStartBelowMinDischarge
    cfg.cvLowAlarmGate = cfg.cvMinDischarge;       // dischargeTaperOrderBad
    cfg.cvMaintStop = cfg.cvMaintStart;            // maintHysteresisBad (after cvMaintStart above)
    cfg.trickleA = -1.0f;                          // hasNegativeCurrent

    ValidationResult r = SystemConfig::validate(cfg);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_FALSE(r.hasNaN);
    TEST_ASSERT_TRUE(r.maintStartBelowMinDischarge);
    TEST_ASSERT_TRUE(r.chargeTaperOrderBad);
    TEST_ASSERT_TRUE(r.dischargeTaperOrderBad);
    TEST_ASSERT_TRUE(r.maintHysteresisBad);
    TEST_ASSERT_TRUE(r.chargeHeadroomBad);
    TEST_ASSERT_TRUE(r.hasNegativeCurrent);
}

// A NaN field short-circuits every other check, even when other fields also
// violate ordering/hysteresis rules that would otherwise be flagged.
static void test_nan_short_circuits_other_checks(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMaintStart = cfg.cvMinDischarge;   // would be maintStartBelowMinDischarge
    cfg.cvHighAlarmGate = cfg.cvMaxCharge;   // would be chargeTaperOrderBad
    cfg.trickleA = -1.0f;                    // would be hasNegativeCurrent
    cfg.cvMaxCharge = NAN;                   // -> hasNaN, short-circuits everything else

    ValidationResult r = SystemConfig::validate(cfg);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_TRUE(r.hasNaN);
    TEST_ASSERT_FALSE(r.maintStartBelowMinDischarge);
    TEST_ASSERT_FALSE(r.chargeTaperOrderBad);
    TEST_ASSERT_FALSE(r.dischargeTaperOrderBad);
    TEST_ASSERT_FALSE(r.maintHysteresisBad);
    TEST_ASSERT_FALSE(r.chargeHeadroomBad);
    TEST_ASSERT_FALSE(r.hasNegativeCurrent);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_baseline_is_ok);
    RUN_TEST(test_maint_start_below_min_discharge_boundary);
    RUN_TEST(test_charge_taper_order_boundary);
    RUN_TEST(test_discharge_taper_order_boundary);
    RUN_TEST(test_maint_hysteresis_boundary);
    RUN_TEST(test_charge_headroom_boundary);
    RUN_TEST(test_negative_current_boundary);
    RUN_TEST(test_combined_violations_all_surface);
    RUN_TEST(test_nan_short_circuits_other_checks);
    return UNITY_END();
}
