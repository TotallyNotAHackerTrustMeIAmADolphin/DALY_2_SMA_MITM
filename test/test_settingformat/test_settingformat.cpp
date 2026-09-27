// Native unit tests for SettingFormat.h's two setting-value formatters
// (#94): `pio test -e native`. Includes the real header - no mirrored copy.

#include <unity.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "SystemConfig.h"
#include "SettingFormat.h"

void setUp(void) {}
void tearDown(void) {}

// --- formatSettingFixed(): the /config page's and range text's own decimals ---

static void test_format_setting_fixed_float_uses_its_own_decimals(void)
{
    SystemConfig cfg;
    char buf[24];

    formatSettingFixed(cfg.cvMaxCharge, 3.55, buf, sizeof(buf)); // 3 decimals
    TEST_ASSERT_EQUAL_STRING("3.550", buf);

    formatSettingFixed(cfg.maxChargeA, 252.5, buf, sizeof(buf)); // 0 decimals
    TEST_ASSERT_EQUAL_STRING("252", buf);

    formatSettingFixed(cfg.trickleA, 2.0, buf, sizeof(buf)); // 1 decimal
    TEST_ASSERT_EQUAL_STRING("2.0", buf);
}

static void test_format_setting_fixed_integer_kind(void)
{
    SystemConfig cfg;
    char buf[24];
    formatSettingFixed(cfg.bmsTimeout, 60.0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("60", buf);
}

// Pins ConfigStore.cpp's "[CFG] Stored ... is outside ... - using the
// default ..." line for Max Charge Vpc: a limit (min/max) must print the
// owner's typed 2.500/3.550, not the setting's literal float value
// (3.5500002) - a value (the stored number, the default) stays round-trip.
static void test_out_of_range_message_uses_value_for_value_fixed_for_limits(void)
{
    SystemConfig cfg;
    char valBuf[24], minBuf[24], maxBuf[24], defBuf[24], line[160];
    formatSettingValue(cfg.cvMaxCharge, 3.5, valBuf, sizeof(valBuf));
    formatSettingFixed(cfg.cvMaxCharge, cfg.cvMaxCharge.min(), minBuf, sizeof(minBuf));
    formatSettingFixed(cfg.cvMaxCharge, cfg.cvMaxCharge.max(), maxBuf, sizeof(maxBuf));
    formatSettingValue(cfg.cvMaxCharge, cfg.cvMaxCharge.def(), defBuf, sizeof(defBuf));
    snprintf(line, sizeof(line), "[CFG] Stored %s (%s) is outside %s-%s %s - using the default %s\n",
             cfg.cvMaxCharge.label(), valBuf, minBuf, maxBuf, cfg.cvMaxCharge.unit(), defBuf);
    TEST_ASSERT_EQUAL_STRING(
        "[CFG] Stored Max Charge Vpc (3.5) is outside 2.500-3.550 V - using the default 3.45\n", line);
}

// --- formatSettingValue(): the shortest round-trip form, for [CFG] lines ---

static void test_format_setting_value_float_precision(void)
{
    SystemConfig cfg;
    char buf[24];

    formatSettingValue(cfg.maxChargeA, 250.0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("250", buf);

    formatSettingValue(cfg.maxChargeA, 252.5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("252.5", buf);

    formatSettingValue(cfg.maxChargeA, 252.0625, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("252.0625", buf);

    formatSettingValue(cfg.trickleA, (double)2.0004f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2.0004", buf);

    formatSettingValue(cfg.cvMaxCharge, (double)3.55f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("3.55", buf);

    formatSettingValue(cfg.cvMaxCharge, (double)3.425f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("3.425", buf);

    formatSettingValue(cfg.maxChargeA, 0.0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0", buf);

    formatSettingValue(cfg.maxChargeA, 1000.0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1000", buf);

    formatSettingValue(cfg.trickleA, (double)0.001f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0.001", buf);
}

static void test_format_setting_value_integer_kind(void)
{
    SystemConfig cfg;
    char buf[24];

    formatSettingValue(cfg.bmsTimeout, 60.0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("60", buf);
}

// A dummy SettingBase-shaped stand-in isn't needed: reuse a real float
// setting (maxChargeA, range wide enough to hold every sweep start/value
// used below) purely for its KIND_FLOAT tag - formatSettingValue() never
// looks at its range or current value, only s.kind() and the v passed in.
static void sweepRange(const SettingBase &floatSetting, float start, int steps)
{
    float v = start;
    char prevBuf[24];
    formatSettingValue(floatSetting, (double)v, prevBuf, sizeof(prevBuf));
    TEST_ASSERT_TRUE_MESSAGE(v == (float)strtod(prevBuf, nullptr), prevBuf);

    for (int i = 0; i < steps; i++)
    {
        float next = nextafterf(v, INFINITY);
        TEST_ASSERT_TRUE(next != v);

        char nextBuf[24];
        formatSettingValue(floatSetting, (double)next, nextBuf, sizeof(nextBuf));

        // Each string parses back to exactly its own float (an exact
        // compare, not TEST_ASSERT_EQUAL_FLOAT - Unity's float assert
        // allows a relative tolerance, which would let a formatter that's
        // off by a ULP pass) ...
        TEST_ASSERT_TRUE_MESSAGE(next == (float)strtod(nextBuf, nullptr), nextBuf);
        // ... and adjacent floats never print identically (the whole point
        // of this fix: a real, distinguishable change must always log as
        // two different numbers).
        TEST_ASSERT_TRUE_MESSAGE(strcmp(prevBuf, nextBuf) != 0, nextBuf);

        v = next;
        strcpy(prevBuf, nextBuf);
    }
}

static void test_format_setting_value_sweep_adjacent_floats_always_differ(void)
{
    SystemConfig cfg;
    // Ranges chosen to stay within maxChargeA's [0, 1000] the whole sweep
    // (2000 consecutive floats moves the value by only a handful of ULPs).
    sweepRange(cfg.maxChargeA, 2.5f, 2000);
    sweepRange(cfg.maxChargeA, 3.4f, 2000);
    sweepRange(cfg.maxChargeA, 250.0f, 2000);
    sweepRange(cfg.maxChargeA, 999.0f, 2000);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_format_setting_fixed_float_uses_its_own_decimals);
    RUN_TEST(test_format_setting_fixed_integer_kind);
    RUN_TEST(test_out_of_range_message_uses_value_for_value_fixed_for_limits);
    RUN_TEST(test_format_setting_value_float_precision);
    RUN_TEST(test_format_setting_value_integer_kind);
    RUN_TEST(test_format_setting_value_sweep_adjacent_floats_always_differ);
    return UNITY_END();
}
