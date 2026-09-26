// Native unit tests for ConfigForm's pure /save parsing, validation and
// change-log formatting (#89): `pio test -e native`. Includes the real
// header - no mirrored copy.

#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "SystemConfig.h"
#include "ConfigForm.h"

void setUp(void) {}
void tearDown(void) {}

// A tiny key->text table, standing in for AsyncWebServerRequest::getParam()
// the way ConfigForm::apply() is meant to be driven natively.
struct Field
{
    const char *key;
    const char *text;
};

static ConfigForm::Result apply(SystemConfig &cfg, const std::vector<Field> &fields, std::vector<std::string> &errors)
{
    auto lookup = [&fields](const char *key) -> const char *
    {
        for (const Field &f : fields)
            if (strcmp(f.key, key) == 0)
                return f.text;
        return nullptr;
    };
    auto emit = [&errors](const char *msg)
    { errors.push_back(msg); };
    return ConfigForm::apply(cfg, lookup, emit);
}

// --- not-a-number ---

static void test_not_a_number_reports_error_and_marks_present(void)
{
    SystemConfig cfg;
    std::vector<std::string> errors;
    ConfigForm::Result r = apply(cfg, {{"ca", "abc"}}, errors);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL(1u, errors.size());
    TEST_ASSERT_EQUAL_STRING("Max Charge Amps is not a valid number.", errors[0].c_str());
    TEST_ASSERT_TRUE(r.present[0]); // maxChargeA is all()[0]
    TEST_ASSERT_EQUAL_FLOAT((float)cfg.maxChargeA.def(), cfg.maxChargeA); // left alone
}

// --- out-of-range ---

static void test_out_of_range_reports_range_message(void)
{
    SystemConfig cfg;
    std::vector<std::string> errors;
    ConfigForm::Result r = apply(cfg, {{"ca", "2500"}}, errors);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL(1u, errors.size());
    TEST_ASSERT_EQUAL_STRING("Max Charge Amps must be between 0 and 1000 A.", errors[0].c_str());
}

// Pins the /save 400 body's line for Max Charge Vpc: a limit prints the
// owner's typed 3.550, not the setting's literal float max (3.5500002).
static void test_out_of_range_range_message_for_max_charge_vpc(void)
{
    SystemConfig cfg;
    std::vector<std::string> errors;
    ConfigForm::Result r = apply(cfg, {{"cmv", "9"}}, errors);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL(1u, errors.size());
    TEST_ASSERT_EQUAL_STRING("Max Charge Vpc must be between 2.500 and 3.550 V.", errors[0].c_str());
}

// --- two-setting violation ---

static void test_two_setting_violation_only_checked_once_everything_parses(void)
{
    SystemConfig cfg;
    std::vector<std::string> errors;
    // Both individually in-range, but cvHighAlarmGate == cvMaxCharge (charge
    // taper order bad).
    ConfigForm::Result r = apply(cfg, {{"cag", "3.450"}, {"cmv", "3.450"}}, errors);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL(1u, errors.size());
    TEST_ASSERT_EQUAL_STRING(
        "Charge thresholds must be ordered: Start Taper Vpc < Target Trickle Vpc < Max Charge Vpc.",
        errors[0].c_str());
}

static void test_two_setting_rule_skipped_when_a_field_fails_to_parse(void)
{
    // cvHighAlarmGate would also violate the ordering rule here, but it
    // never parses - validate() must not run against a copy that still
    // holds cvMaxCharge's OLD value for a field nobody submitted correctly.
    SystemConfig cfg;
    std::vector<std::string> errors;
    ConfigForm::Result r = apply(cfg, {{"cag", "abc"}}, errors);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL(1u, errors.size()); // only the parse error, no validation message
    TEST_ASSERT_EQUAL_STRING("Target Trickle Vpc is not a valid number.", errors[0].c_str());
}

// --- only-present-keys-written ---

static void test_only_present_keys_marked_and_others_left_alone(void)
{
    SystemConfig cfg;
    TEST_ASSERT_TRUE(cfg.maxDischargeA.set(300));
    std::vector<std::string> errors;
    ConfigForm::Result r = apply(cfg, {{"ca", "200"}}, errors);

    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, cfg.maxChargeA);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, cfg.maxDischargeA); // untouched, kept its pre-call value

    size_t idxCa = 0, idxDa = 0;
    auto all = cfg.all();
    for (size_t i = 0; i < all.size(); i++)
    {
        if (strcmp(all[i]->key(), "ca") == 0)
            idxCa = i;
        if (strcmp(all[i]->key(), "da") == 0)
            idxDa = i;
    }
    TEST_ASSERT_TRUE(r.present[idxCa]);
    TEST_ASSERT_FALSE(r.present[idxDa]);
}

// --- all-or-nothing ---

static void test_all_or_nothing_ok_false_whenever_any_field_errors(void)
{
    SystemConfig cfg;
    std::vector<std::string> errors;
    // One good field, one bad: apply() still parses the good one (so a
    // caller inspecting cfg would see it changed), but ok=false is the
    // caller's signal to discard the whole copy rather than publish or
    // store it.
    ConfigForm::Result r = apply(cfg, {{"ca", "200"}, {"da", "abc"}}, errors);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, cfg.maxChargeA); // parsed despite the sibling error
    TEST_ASSERT_EQUAL(1u, errors.size());
}

static void test_all_present_fields_valid_is_ok(void)
{
    SystemConfig cfg;
    std::vector<std::string> errors;
    ConfigForm::Result r = apply(cfg, {{"ca", "300"}, {"vs", "5"}}, errors);

    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(errors.empty());
    TEST_ASSERT_EQUAL_FLOAT(300.0f, cfg.maxChargeA);
    TEST_ASSERT_EQUAL(5, (int)cfg.vSamples);
}

// --- logChanges(): the change-log lines ---

static void test_log_changes_emits_one_line_per_changed_setting(void)
{
    SystemConfig before;
    SystemConfig after = before;
    TEST_ASSERT_TRUE(after.maxChargeA.set(300));

    std::vector<std::string> lines;
    ConfigForm::logChanges(before, after, [&lines](const char *line)
                            { lines.push_back(line); });

    TEST_ASSERT_EQUAL(1u, lines.size());
    TEST_ASSERT_EQUAL_STRING("[CFG] Max Charge Amps: 250 -> 300 A\n", lines[0].c_str());
}

static void test_log_changes_no_changes_line_when_nothing_changed(void)
{
    SystemConfig before;
    SystemConfig after = before;

    std::vector<std::string> lines;
    ConfigForm::logChanges(before, after, [&lines](const char *line)
                            { lines.push_back(line); });

    TEST_ASSERT_EQUAL(1u, lines.size());
    TEST_ASSERT_EQUAL_STRING("[CFG] Saved, no changes\n", lines[0].c_str());
}

static void test_log_changes_tiny_real_change_never_prints_x_arrow_x(void)
{
    // The #94 bug this whole formatter exists for: two distinct binary32
    // values that would print identically at fixed decimals must still be
    // reported as a real, distinguishable change here.
    SystemConfig before;
    SystemConfig after = before;
    TEST_ASSERT_TRUE(after.trickleA.set(2.0004));

    std::vector<std::string> lines;
    ConfigForm::logChanges(before, after, [&lines](const char *line)
                            { lines.push_back(line); });

    TEST_ASSERT_EQUAL(1u, lines.size());
    TEST_ASSERT_EQUAL_STRING("[CFG] Trickle Amps: 2 -> 2.0004 A\n", lines[0].c_str());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_not_a_number_reports_error_and_marks_present);
    RUN_TEST(test_out_of_range_reports_range_message);
    RUN_TEST(test_out_of_range_range_message_for_max_charge_vpc);
    RUN_TEST(test_two_setting_violation_only_checked_once_everything_parses);
    RUN_TEST(test_two_setting_rule_skipped_when_a_field_fails_to_parse);
    RUN_TEST(test_only_present_keys_marked_and_others_left_alone);
    RUN_TEST(test_all_or_nothing_ok_false_whenever_any_field_errors);
    RUN_TEST(test_all_present_fields_valid_is_ok);
    RUN_TEST(test_log_changes_emits_one_line_per_changed_setting);
    RUN_TEST(test_log_changes_no_changes_line_when_nothing_changed);
    RUN_TEST(test_log_changes_tiny_real_change_never_prints_x_arrow_x);
    return UNITY_END();
}
