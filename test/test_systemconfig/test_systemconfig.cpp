// Native unit tests for SystemConfig's Setting objects and validate()
// (#53, #61): `pio test -e native`. Includes the real headers - no mirrored
// copy to keep in sync, same pattern as test_glideslope.

#include <unity.h>
#include <math.h>
#include <string.h>
#include <string>
#include "SystemConfig.h"
#include "WebPages.h"

using ValidationResult = SystemConfig::ValidationResult;

// A realistic live-style config (3.55 V max charge, #8), set through set()
// like the firmware does.
static SystemConfig baseline()
{
    SystemConfig cfg;
    TEST_ASSERT_TRUE(cfg.cvStartTaper.set(3.40));
    TEST_ASSERT_TRUE(cfg.cvHighAlarmGate.set(3.45));
    TEST_ASSERT_TRUE(cfg.cvMaxCharge.set(3.55));
    TEST_ASSERT_TRUE(cfg.cvStartDTaper.set(3.10));
    TEST_ASSERT_TRUE(cfg.cvLowAlarmGate.set(3.00));
    TEST_ASSERT_TRUE(cfg.cvMinDischarge.set(2.90));
    TEST_ASSERT_TRUE(cfg.cvMaintStart.set(3.00));
    TEST_ASSERT_TRUE(cfg.cvMaintStop.set(3.10));
    return cfg;
}

void setUp(void) {}
void tearDown(void) {}

// --- The settings themselves ---

static void test_defaults_pass_validation(void)
{
    SystemConfig cfg;
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).ok());
    TEST_ASSERT_TRUE(SystemConfig::validate(baseline()).ok());
}

static void test_every_setting_is_listed_once_with_a_sane_range(void)
{
    SystemConfig cfg;
    auto all = cfg.all();
    for (size_t i = 0; i < all.size(); i++)
    {
        const SettingBase *s = all[i];
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_TRUE_MESSAGE(s->min() < s->max(), s->key());
        TEST_ASSERT_TRUE_MESSAGE(s->def() >= s->min() && s->def() <= s->max(), s->key());
        TEST_ASSERT_TRUE_MESSAGE(s->def() == s->value(), s->key()); // starts at default
        for (size_t j = i + 1; j < all.size(); j++)
        {
            TEST_ASSERT_TRUE_MESSAGE(s != all[j], s->key());
            TEST_ASSERT_TRUE_MESSAGE(strcmp(s->key(), all[j]->key()) != 0, s->key());
        }
    }
}

static void test_every_setting_has_exactly_one_label_and_input_on_config_page(void)
{
    // /config fills !!LABEL_<key>!! and !!IN_<key>!! from each setting. A
    // setting without them would be unsettable or unlabelled; a stray one
    // would show up literally on the page.
    SystemConfig cfg;
    std::string html(config_html);
    const char *kinds[] = {"!!LABEL_", "!!IN_"};
    for (const char *kind : kinds)
    {
        size_t placeholders = 0;
        for (size_t pos = html.find(kind); pos != std::string::npos; pos = html.find(kind, pos + 1))
            placeholders++;
        TEST_ASSERT_EQUAL_MESSAGE(SystemConfig::kNumSettings, placeholders, kind);
        for (const SettingBase *s : cfg.all())
        {
            std::string ph = std::string(kind) + s->key() + "!!";
            size_t first = html.find(ph);
            TEST_ASSERT_TRUE_MESSAGE(first != std::string::npos, ph.c_str());
            TEST_ASSERT_TRUE_MESSAGE(html.find(ph, first + 1) == std::string::npos, ph.c_str());
        }
    }
    TEST_ASSERT_TRUE(html.find("type=\"number\"") == std::string::npos); // no hand-written inputs left
}

static void test_copy_carries_values_and_all_points_into_the_copy(void)
{
    // saveConfig() edits a copy and publishes it with `*_cfg = copy`.
    SystemConfig a;
    TEST_ASSERT_TRUE(a.maxChargeA.set(300));
    SystemConfig b = a;
    TEST_ASSERT_EQUAL_FLOAT(300.0f, b.maxChargeA);
    TEST_ASSERT_TRUE(b.all()[0] == &b.maxChargeA);
    TEST_ASSERT_TRUE(b.all()[0]->set(200));
    TEST_ASSERT_EQUAL_FLOAT(300.0f, a.maxChargeA); // a untouched
    a = b;
    TEST_ASSERT_EQUAL_FLOAT(200.0f, a.maxChargeA);
    TEST_ASSERT_EQUAL_STRING("ca", a.all()[0]->key());
}

// --- set(): the only way a value gets in ---

static void test_set_accepts_limits_and_refuses_just_outside(void)
{
    SystemConfig cfg;
    for (SettingBase *s : cfg.all())
    {
        double step = s->kind() == SettingBase::KIND_FLOAT ? 0.001 : 1.0;
        TEST_ASSERT_TRUE_MESSAGE(s->set(s->min()), s->key());
        TEST_ASSERT_TRUE_MESSAGE(s->set(s->max()), s->key());
        TEST_ASSERT_FALSE_MESSAGE(s->set(s->min() - step), s->key());
        TEST_ASSERT_FALSE_MESSAGE(s->set(s->max() + step), s->key());
        TEST_ASSERT_FALSE_MESSAGE(s->set(NAN), s->key());
        TEST_ASSERT_FALSE_MESSAGE(s->set(INFINITY), s->key());
        TEST_ASSERT_TRUE_MESSAGE(s->max() == s->value(), s->key()); // refusals left it alone
        s->reset();
        TEST_ASSERT_TRUE_MESSAGE(s->def() == s->value(), s->key());
    }
}

static void test_set_integer_setting_refuses_fraction(void)
{
    SystemConfig cfg;
    TEST_ASSERT_FALSE(cfg.vSamples.set(12.5));
    TEST_ASSERT_EQUAL(12, (int)cfg.vSamples);
}

static void test_charge_headroom_boundary(void)
{
    // #8: 3.550 V is the ceiling (Daly OVP 3.65 minus 100 mV). In binary32
    // 3.65f - 0.10f > 3.55f, so no tolerance is needed.
    SystemConfig cfg;
    TEST_ASSERT_TRUE(cfg.cvMaxCharge.set(3.550f));
    TEST_ASSERT_FALSE(cfg.cvMaxCharge.set(3.5505));
    TEST_ASSERT_FALSE(cfg.cvMaxCharge.set(3.551));
    TEST_ASSERT_EQUAL_FLOAT(3.55f, cfg.cvMaxCharge);
}

static void test_typos_refused_that_ordering_would_miss(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_FALSE(cfg.cvMinDischarge.set(0.3));  // for 3.0: deep discharge
    TEST_ASSERT_FALSE(cfg.maxChargeA.set(2500));     // for 250
    TEST_ASSERT_FALSE(cfg.bmsTimeout.set(-1));       // was a ~49-day stale window
    TEST_ASSERT_FALSE(cfg.spreadStartMv.set(65476)); // -60 rolled over, 2026-09-26
    TEST_ASSERT_FALSE(cfg.trickleA.set(-0.01));
    TEST_ASSERT_TRUE(cfg.trickleA.set(0));
}

// --- parse(): what /save does with a form value ---

static void test_parse_rejects_garbage_and_leaves_value_alone(void)
{
    const char *bad[] = {"", "   ", "abc", "25o", "250A", "1,5", "--1"};
    for (const char *in : bad)
    {
        SystemConfig cfg;
        TEST_ASSERT_EQUAL_MESSAGE(PARSE_NOT_A_NUMBER, cfg.maxChargeA.parse(in), in);
        TEST_ASSERT_EQUAL_FLOAT(250.0f, cfg.maxChargeA);
    }
}

static void test_parse_accepts_numbers_with_spaces(void)
{
    SystemConfig cfg;
    TEST_ASSERT_EQUAL(PARSE_OK, cfg.maxChargeA.parse(" 252.5 "));
    TEST_ASSERT_EQUAL_FLOAT(252.5f, cfg.maxChargeA);
    TEST_ASSERT_EQUAL(PARSE_OK, cfg.spreadStartMv.parse("61"));
    TEST_ASSERT_EQUAL(61, (int)cfg.spreadStartMv);
    TEST_ASSERT_EQUAL(PARSE_OK, cfg.cvMaxCharge.parse("3.550"));
    TEST_ASSERT_EQUAL_FLOAT(3.55f, cfg.cvMaxCharge);
}

static void test_parse_never_wraps(void)
{
    SystemConfig cfg;
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.spreadStartMv.parse("-60"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.spreadStartMv.parse("65596")); // would truncate to 60
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.spreadStartMv.parse("99999999999999999999"));
    TEST_ASSERT_EQUAL(60, (int)cfg.spreadStartMv);
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.bmsTimeout.parse("-1"));
    TEST_ASSERT_EQUAL(60, (int)cfg.bmsTimeout);
}

static void test_parse_integer_setting_rejects_fraction(void)
{
    SystemConfig cfg;
    TEST_ASSERT_EQUAL(PARSE_NOT_A_NUMBER, cfg.vSamples.parse("12.5"));
    TEST_ASSERT_EQUAL(12, (int)cfg.vSamples);
}

static void test_parse_range_and_nonfinite(void)
{
    SystemConfig cfg;
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.cvMaxCharge.parse("3.5505"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.cvMaxCharge.parse("3.5501"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.cvMaxCharge.parse("nan"));
    TEST_ASSERT_EQUAL(PARSE_OUT_OF_RANGE, cfg.cvMaxCharge.parse("1e39"));
    TEST_ASSERT_EQUAL_FLOAT(3.45f, cfg.cvMaxCharge);
}

// --- validate(): rules relating two settings ---

static void test_maint_start_below_min_discharge_boundary(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvMaintStart.set(cfg.cvMinDischarge));
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).maintStartBelowMinDischarge);
    TEST_ASSERT_TRUE(cfg.cvMaintStart.set(cfg.cvMinDischarge + 0.01f));
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).maintStartBelowMinDischarge);
}

static void test_charge_taper_order_boundary(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvStartTaper.set(cfg.cvHighAlarmGate));
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeTaperOrderBad);
    cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvHighAlarmGate.set(cfg.cvMaxCharge));
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).chargeTaperOrderBad);
    cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvStartTaper.set(cfg.cvHighAlarmGate - 0.01f));
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).chargeTaperOrderBad);
}

static void test_discharge_taper_order_boundary(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvStartDTaper.set(cfg.cvLowAlarmGate));
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).dischargeTaperOrderBad);
    cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvLowAlarmGate.set(cfg.cvMinDischarge));
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).dischargeTaperOrderBad);
    cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvStartDTaper.set(cfg.cvLowAlarmGate + 0.01f));
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).dischargeTaperOrderBad);
}

static void test_maint_hysteresis_boundary(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvMaintStop.set(cfg.cvMaintStart));
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).maintHysteresisBad);
    TEST_ASSERT_TRUE(cfg.cvMaintStop.set(cfg.cvMaintStart + 0.01f));
    TEST_ASSERT_FALSE(SystemConfig::validate(cfg).maintHysteresisBad);
}

static void test_equal_spread_thresholds_allowed(void)
{
    // Glideslope::spreadFactor() treats start >= full as a deliberate step
    // (test_spread_factor_degenerate_config), so it is not a violation.
    SystemConfig cfg = baseline();
    TEST_ASSERT_TRUE(cfg.spreadStartMv.set(100));
    TEST_ASSERT_TRUE(cfg.spreadMaxMv.set(100));
    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).ok());
}

static void test_combined_violations_all_surface(void)
{
    SystemConfig cfg = baseline();
    TEST_ASSERT_TRUE(cfg.cvHighAlarmGate.set(cfg.cvMaxCharge));  // charge order
    TEST_ASSERT_TRUE(cfg.cvMaintStart.set(cfg.cvMinDischarge));  // #12
    TEST_ASSERT_TRUE(cfg.cvLowAlarmGate.set(cfg.cvMinDischarge)); // discharge order
    TEST_ASSERT_TRUE(cfg.cvMaintStop.set(cfg.cvMaintStart));     // hysteresis
    ValidationResult r = SystemConfig::validate(cfg);
    TEST_ASSERT_TRUE(r.chargeTaperOrderBad);
    TEST_ASSERT_TRUE(r.maintStartBelowMinDischarge);
    TEST_ASSERT_TRUE(r.dischargeTaperOrderBad);
    TEST_ASSERT_TRUE(r.maintHysteresisBad);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_pass_validation);
    RUN_TEST(test_every_setting_is_listed_once_with_a_sane_range);
    RUN_TEST(test_every_setting_has_exactly_one_label_and_input_on_config_page);
    RUN_TEST(test_copy_carries_values_and_all_points_into_the_copy);
    RUN_TEST(test_set_accepts_limits_and_refuses_just_outside);
    RUN_TEST(test_set_integer_setting_refuses_fraction);
    RUN_TEST(test_charge_headroom_boundary);
    RUN_TEST(test_typos_refused_that_ordering_would_miss);
    RUN_TEST(test_parse_rejects_garbage_and_leaves_value_alone);
    RUN_TEST(test_parse_accepts_numbers_with_spaces);
    RUN_TEST(test_parse_never_wraps);
    RUN_TEST(test_parse_integer_setting_rejects_fraction);
    RUN_TEST(test_parse_range_and_nonfinite);
    RUN_TEST(test_maint_start_below_min_discharge_boundary);
    RUN_TEST(test_charge_taper_order_boundary);
    RUN_TEST(test_discharge_taper_order_boundary);
    RUN_TEST(test_maint_hysteresis_boundary);
    RUN_TEST(test_equal_spread_thresholds_allowed);
    RUN_TEST(test_combined_violations_all_surface);
    return UNITY_END();
}
