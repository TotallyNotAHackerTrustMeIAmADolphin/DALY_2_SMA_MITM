// Native unit tests for SystemConfig::validate() and the kConfigFields
// table (#53, #61): `pio test -e native`. Includes the real headers - no
// mirrored copy to keep in sync, same pattern as test_glideslope.

#include <unity.h>
#include <math.h>
#include <string.h>
#include <string>
#include "SystemConfig.h"
#include "WebPages.h"

using ValidationResult = SystemConfig::ValidationResult;

// Every field at its table default: the config a fresh device boots with.
static SystemConfig defaults()
{
    SystemConfig cfg{};
    for (const ConfigField &f : kConfigFields)
    {
        uint8_t *m = reinterpret_cast<uint8_t *>(&cfg) + f.offset;
        if (f.kind == ConfigField::KIND_FLOAT)
            *reinterpret_cast<float *>(m) = f.def;
        else if (f.kind == ConfigField::KIND_INT)
            *reinterpret_cast<int *>(m) = (int)f.def;
        else
            *reinterpret_cast<uint16_t *>(m) = (uint16_t)f.def;
    }
    return cfg;
}

// A realistic live-style config (3.55 V max charge, #8).
static SystemConfig baseline()
{
    SystemConfig cfg = defaults();
    cfg.cvStartTaper = 3.40f;
    cfg.cvHighAlarmGate = 3.45f;
    cfg.cvMaxCharge = 3.55f;
    cfg.cvStartDTaper = 3.10f;
    cfg.cvLowAlarmGate = 3.00f;
    cfg.cvMinDischarge = 2.90f;
    cfg.cvMaintStart = 3.00f;
    cfg.cvMaintStop = 3.10f;
    return cfg;
}

static int fieldIndex(const char *key)
{
    for (size_t i = 0; i < kNumConfigFields; i++)
        if (strcmp(kConfigFields[i].key, key) == 0)
            return (int)i;
    TEST_FAIL_MESSAGE("unknown key");
    return -1;
}

static bool outOfRange(const SystemConfig &cfg, const char *key)
{
    return (SystemConfig::validate(cfg).outOfRange & (1u << fieldIndex(key))) != 0;
}

void setUp(void) {}
void tearDown(void) {}

// --- The table itself ---

static void test_table_defaults_pass_validation(void)
{
    TEST_ASSERT_TRUE(SystemConfig::validate(defaults()).ok());
    TEST_ASSERT_TRUE(SystemConfig::validate(baseline()).ok());
}

static void test_table_rows_are_sane(void)
{
    for (size_t i = 0; i < kNumConfigFields; i++)
    {
        const ConfigField &f = kConfigFields[i];
        TEST_ASSERT_TRUE_MESSAGE(f.min < f.max, f.key);
        TEST_ASSERT_TRUE_MESSAGE(f.def >= f.min && f.def <= f.max, f.key);
        for (size_t j = i + 1; j < kNumConfigFields; j++)
            TEST_ASSERT_TRUE_MESSAGE(strcmp(f.key, kConfigFields[j].key) != 0, f.key);
    }
}

static void test_every_field_has_exactly_one_input_on_config_page(void)
{
    // /config builds each <input> from its table row by replacing
    // !!IN_<key>!!. A row without a placeholder would be unsettable; a
    // stray placeholder would show up literally on the page.
    std::string html(config_html);
    size_t placeholders = 0;
    for (size_t pos = html.find("!!IN_"); pos != std::string::npos; pos = html.find("!!IN_", pos + 1))
        placeholders++;
    TEST_ASSERT_EQUAL(kNumConfigFields, placeholders);
    for (const ConfigField &f : kConfigFields)
    {
        std::string ph = std::string("!!IN_") + f.key + "!!";
        size_t first = html.find(ph);
        TEST_ASSERT_TRUE_MESSAGE(first != std::string::npos, f.key);
        TEST_ASSERT_TRUE_MESSAGE(html.find(ph, first + 1) == std::string::npos, f.key);
    }
    TEST_ASSERT_TRUE(html.find("type=\"number\"") == std::string::npos); // no hand-written inputs left
}

// --- Per-field ranges ---

static void test_every_field_rejects_just_outside_its_range(void)
{
    for (size_t i = 0; i < kNumConfigFields; i++)
    {
        const ConfigField &f = kConfigFields[i];
        for (int side = 0; side < 2; side++)
        {
            SystemConfig cfg = baseline();
            uint8_t *m = reinterpret_cast<uint8_t *>(&cfg) + f.offset;
            if (f.kind == ConfigField::KIND_FLOAT)
                *reinterpret_cast<float *>(m) = side ? f.max + 0.001f : f.min - 0.001f;
            else if (f.kind == ConfigField::KIND_INT)
                *reinterpret_cast<int *>(m) = side ? (int)f.max + 1 : (int)f.min - 1;
            else if (side) // uint16 below 0 can't be stored; see the parser
                *reinterpret_cast<uint16_t *>(m) = (uint16_t)f.max + 1;
            else
                continue;
            TEST_ASSERT_TRUE_MESSAGE(SystemConfig::validate(cfg).outOfRange & (1u << i), f.key);
        }
    }
}

static void test_charge_headroom_boundary(void)
{
    // #8: 3.550 V is the ceiling (Daly OVP 3.65 minus 100 mV). In binary32
    // 3.65f - 0.10f > 3.55f, so no tolerance is needed.
    SystemConfig cfg = baseline();
    cfg.cvMaxCharge = 3.550f;
    TEST_ASSERT_FALSE(outOfRange(cfg, "cmv"));
    cfg.cvMaxCharge = 3.5505f;
    TEST_ASSERT_TRUE(outOfRange(cfg, "cmv"));
    cfg.cvMaxCharge = 3.551f;
    TEST_ASSERT_TRUE(outOfRange(cfg, "cmv"));
}

static void test_negative_current_rejected_zero_allowed(void)
{
    SystemConfig cfg = baseline();
    cfg.trickleA = 0.0f;
    TEST_ASSERT_FALSE(outOfRange(cfg, "ta"));
    cfg.trickleA = -0.01f;
    TEST_ASSERT_TRUE(outOfRange(cfg, "ta"));
}

static void test_typos_caught_that_ordering_misses(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMinDischarge = 0.3f; // for 3.0: deep discharge, ordering still passes
    ValidationResult r = SystemConfig::validate(cfg);
    TEST_ASSERT_TRUE(outOfRange(cfg, "cmdv"));
    TEST_ASSERT_FALSE(r.dischargeTaperOrderBad);

    cfg = baseline();
    cfg.maxChargeA = 2500.0f; // for 250
    TEST_ASSERT_TRUE(outOfRange(cfg, "ca"));

    cfg = baseline();
    cfg.bmsTimeout = -1; // was a ~49-day window in isFresh()
    TEST_ASSERT_TRUE(outOfRange(cfg, "to"));

    cfg = baseline();
    cfg.spreadStartMv = 65476; // -60 rolled over, 2026-09-26
    TEST_ASSERT_TRUE(outOfRange(cfg, "sps"));
}

static void test_nan_and_inf_flagged_and_short_circuit(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMaintStop = INFINITY; // "1e39"; would keep maintenance on forever
    cfg.cvMaintStart = cfg.cvMinDischarge; // would also be a #12 violation
    ValidationResult r = SystemConfig::validate(cfg);
    TEST_ASSERT_TRUE(r.hasNaN);
    TEST_ASSERT_TRUE(outOfRange(cfg, "cmpp"));
    TEST_ASSERT_FALSE(r.maintStartBelowMinDischarge); // skipped
    TEST_ASSERT_FALSE(r.ok());

    cfg = baseline();
    cfg.maxChargeA = NAN;
    r = SystemConfig::validate(cfg);
    TEST_ASSERT_TRUE(r.hasNaN);
    TEST_ASSERT_TRUE(outOfRange(cfg, "ca"));
}

// --- Rules relating two fields ---

static void test_maint_start_below_min_discharge_boundary(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMaintStart = cfg.cvMinDischarge;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).maintStartBelowMinDischarge);
    cfg.cvMaintStart = cfg.cvMinDischarge + 0.01f;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).maintStartBelowMinDischarge);
}

static void test_charge_taper_order_boundary(void)
{
    SystemConfig cfg = baseline();
    cfg.cvStartTaper = cfg.cvHighAlarmGate;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeTaperOrderBad);
    cfg = baseline();
    cfg.cvHighAlarmGate = cfg.cvMaxCharge;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeTaperOrderBad);
    cfg = baseline();
    cfg.cvStartTaper = cfg.cvHighAlarmGate - 0.01f;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).chargeTaperOrderBad);
}

static void test_discharge_taper_order_boundary(void)
{
    SystemConfig cfg = baseline();
    cfg.cvStartDTaper = cfg.cvLowAlarmGate;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).dischargeTaperOrderBad);
    cfg = baseline();
    cfg.cvLowAlarmGate = cfg.cvMinDischarge;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).dischargeTaperOrderBad);
    cfg = baseline();
    cfg.cvStartDTaper = cfg.cvLowAlarmGate + 0.01f;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).dischargeTaperOrderBad);
}

static void test_maint_hysteresis_boundary(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMaintStop = cfg.cvMaintStart;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).maintHysteresisBad);
    cfg.cvMaintStop = cfg.cvMaintStart + 0.01f;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).maintHysteresisBad);
}

static void test_spread_order_boundary(void)
{
    SystemConfig cfg = baseline();
    cfg.spreadStartMv = cfg.spreadMaxMv;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).spreadOrderBad);
    cfg.spreadStartMv = cfg.spreadMaxMv - 1;
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).spreadOrderBad);
}

static void test_combined_violations_all_surface(void)
{
    SystemConfig cfg = baseline();
    cfg.cvMaxCharge = 3.6f;                 // range (headroom)
    cfg.cvHighAlarmGate = cfg.cvMaxCharge;  // charge order
    cfg.cvMaintStart = cfg.cvMinDischarge;  // #12
    cfg.cvLowAlarmGate = cfg.cvMinDischarge; // discharge order
    cfg.cvMaintStop = cfg.cvMaintStart;     // hysteresis
    cfg.trickleA = -1.0f;                   // range
    ValidationResult r = SystemConfig::validate(cfg);
    TEST_ASSERT_TRUE(outOfRange(cfg, "cmv"));
    TEST_ASSERT_TRUE(outOfRange(cfg, "ta"));
    TEST_ASSERT_TRUE(r.chargeTaperOrderBad);
    TEST_ASSERT_TRUE(r.maintStartBelowMinDischarge);
    TEST_ASSERT_TRUE(r.dischargeTaperOrderBad);
    TEST_ASSERT_TRUE(r.maintHysteresisBad);
    TEST_ASSERT_FALSE(r.hasNaN);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_table_defaults_pass_validation);
    RUN_TEST(test_table_rows_are_sane);
    RUN_TEST(test_every_field_has_exactly_one_input_on_config_page);
    RUN_TEST(test_every_field_rejects_just_outside_its_range);
    RUN_TEST(test_charge_headroom_boundary);
    RUN_TEST(test_negative_current_rejected_zero_allowed);
    RUN_TEST(test_typos_caught_that_ordering_misses);
    RUN_TEST(test_nan_and_inf_flagged_and_short_circuit);
    RUN_TEST(test_maint_start_below_min_discharge_boundary);
    RUN_TEST(test_charge_taper_order_boundary);
    RUN_TEST(test_discharge_taper_order_boundary);
    RUN_TEST(test_maint_hysteresis_boundary);
    RUN_TEST(test_spread_order_boundary);
    RUN_TEST(test_combined_violations_all_surface);
    return UNITY_END();
}
