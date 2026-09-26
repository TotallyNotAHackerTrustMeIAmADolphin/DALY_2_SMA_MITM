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

static void test_every_field_has_exactly_one_label_and_input_on_config_page(void)
{
    // /config fills !!LABEL_<key>!! and !!IN_<key>!! from each table row.
    // A row without them would be unsettable or unlabelled; a stray one
    // would show up literally on the page.
    std::string html(config_html);
    const char *kinds[] = {"!!LABEL_", "!!IN_"};
    for (const char *kind : kinds)
    {
        size_t placeholders = 0;
        for (size_t pos = html.find(kind); pos != std::string::npos; pos = html.find(kind, pos + 1))
            placeholders++;
        TEST_ASSERT_EQUAL_MESSAGE(kNumConfigFields, placeholders, kind);
        for (const ConfigField &f : kConfigFields)
        {
            std::string ph = std::string(kind) + f.key + "!!";
            size_t first = html.find(ph);
            TEST_ASSERT_TRUE_MESSAGE(first != std::string::npos, ph.c_str());
            TEST_ASSERT_TRUE_MESSAGE(html.find(ph, first + 1) == std::string::npos, ph.c_str());
        }
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

static void test_equal_spread_thresholds_allowed(void)
{
    // Glideslope::spreadFactor() treats start >= full as a deliberate step
    // (test_spread_factor_degenerate_config), so it is not a violation.
    SystemConfig cfg = baseline();
    cfg.spreadStartMv = cfg.spreadMaxMv = 100;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).ok());
}

// --- parseConfigField(): what /save stores ---

static void test_parse_rejects_garbage_and_leaves_field_untouched(void)
{
    const ConfigField &f = kConfigFields[fieldIndex("ca")];
    const char *bad[] = {"", "   ", "abc", "25o", "250A", "1,5", "--1"};
    for (const char *in : bad)
    {
        SystemConfig cfg = baseline();
        TEST_ASSERT_EQUAL_MESSAGE(PARSE_NOT_A_NUMBER, parseConfigField(cfg, f, in), in);
        TEST_ASSERT_EQUAL_FLOAT(250.0f, cfg.maxChargeA);
    }
}

static void test_parse_accepts_numbers_with_spaces(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_EQUAL(PARSE_OK, parseConfigField(cfg, kConfigFields[fieldIndex("ca")], " 252.5 "));
    TEST_ASSERT_EQUAL_FLOAT(252.5f, cfg.maxChargeA);
    TEST_ASSERT_EQUAL(PARSE_OK, parseConfigField(cfg, kConfigFields[fieldIndex("sps")], "60"));
    TEST_ASSERT_EQUAL(60, cfg.spreadStartMv);
    TEST_ASSERT_EQUAL(PARSE_OK, parseConfigField(cfg, kConfigFields[fieldIndex("cmv")], "3.550"));
    TEST_ASSERT_EQUAL_FLOAT(3.55f, cfg.cvMaxCharge);
}

static void test_parse_never_wraps(void)
{
    // 2026-09-26: "-60" for the spread start was stored as 65476.
    SystemConfig cfg = baseline();
    const ConfigField &sps = kConfigFields[fieldIndex("sps")];
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, sps, "-60"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, sps, "65596")); // would wrap to 60
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, sps, "99999999999999999999"));
    TEST_ASSERT_EQUAL(60, cfg.spreadStartMv);
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, kConfigFields[fieldIndex("to")], "-1"));
    TEST_ASSERT_EQUAL(60, cfg.bmsTimeout);
}

static void test_parse_integer_field_rejects_fraction(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_EQUAL(PARSE_NOT_A_NUMBER, parseConfigField(cfg, kConfigFields[fieldIndex("vs")], "12.5"));
    TEST_ASSERT_EQUAL(12, cfg.vSamples);
}

static void test_parse_range_and_nonfinite(void)
{
    SystemConfig cfg = baseline();
    const ConfigField &cmv = kConfigFields[fieldIndex("cmv")];
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, cmv, "3.5505"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, cmv, "3.5501"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, cmv, "nan"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, parseConfigField(cfg, cmv, "1e39"));
    TEST_ASSERT_EQUAL_FLOAT(3.55f, cfg.cvMaxCharge);
}

// --- storeConfigField(): what loadConfig() keeps from NVS ---

static void test_store_checks_before_narrowing(void)
{
    // A stored uint32 of 65596 must not become 60 by truncation.
    SystemConfig cfg = baseline();
    TEST_ASSERT_FALSE(storeConfigField(cfg, kConfigFields[fieldIndex("sps")], 65596.0));
    TEST_ASSERT_FALSE(storeConfigField(cfg, kConfigFields[fieldIndex("sps")], 65476.0));
    TEST_ASSERT_EQUAL(60, cfg.spreadStartMv);
    TEST_ASSERT_FALSE(storeConfigField(cfg, kConfigFields[fieldIndex("ca")], NAN));
    TEST_ASSERT_EQUAL_FLOAT(250.0f, cfg.maxChargeA);
    TEST_ASSERT_TRUE(storeConfigField(cfg, kConfigFields[fieldIndex("ca")], 300.0));
    TEST_ASSERT_EQUAL_FLOAT(300.0f, cfg.maxChargeA);
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
    RUN_TEST(test_every_field_has_exactly_one_label_and_input_on_config_page);
    RUN_TEST(test_every_field_rejects_just_outside_its_range);
    RUN_TEST(test_charge_headroom_boundary);
    RUN_TEST(test_negative_current_rejected_zero_allowed);
    RUN_TEST(test_typos_caught_that_ordering_misses);
    RUN_TEST(test_nan_and_inf_flagged_and_short_circuit);
    RUN_TEST(test_maint_start_below_min_discharge_boundary);
    RUN_TEST(test_charge_taper_order_boundary);
    RUN_TEST(test_discharge_taper_order_boundary);
    RUN_TEST(test_maint_hysteresis_boundary);
    RUN_TEST(test_equal_spread_thresholds_allowed);
    RUN_TEST(test_parse_rejects_garbage_and_leaves_field_untouched);
    RUN_TEST(test_parse_accepts_numbers_with_spaces);
    RUN_TEST(test_parse_never_wraps);
    RUN_TEST(test_parse_integer_field_rejects_fraction);
    RUN_TEST(test_parse_range_and_nonfinite);
    RUN_TEST(test_store_checks_before_narrowing);
    RUN_TEST(test_combined_violations_all_surface);
    return UNITY_END();
}
