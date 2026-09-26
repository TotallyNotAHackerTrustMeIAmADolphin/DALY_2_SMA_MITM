// Native tests for include/LocalClock.h (#83): `pio test -e native`.

#include <unity.h>
#include "LocalClock.h"

using namespace LocalClock;

void setUp(void) {}
void tearDown(void) {}

static void test_below_threshold_is_invalid(void)
{
    TEST_ASSERT_FALSE(clockValid(0));
    TEST_ASSERT_FALSE(clockValid(kMinValidEpoch - 1));
    TEST_ASSERT_FALSE(clockValid(kMinValidEpoch));
}

static void test_above_threshold_is_valid(void)
{
    TEST_ASSERT_TRUE(clockValid(kMinValidEpoch + 1));
}

// Picks the stricter (2001) threshold, not the old 1971 one this replaces
// in netLog/SDLogger - a clock reading, say, 1980 must still count invalid.
static void test_between_old_and_new_threshold_is_invalid(void)
{
    // 1980-01-01 UTC, well past tm_year > 70 but well under kMinValidEpoch.
    TEST_ASSERT_FALSE(clockValid(315532800L));
}

// The host running this test has a real, already-synced clock (it's well
// past 2001) - localNow() must report valid and fill in a plausible year.
static void test_localnow_on_a_real_clock_is_valid(void)
{
    tm out{};
    TEST_ASSERT_TRUE(localNow(out));
    TEST_ASSERT_GREATER_OR_EQUAL(101, out.tm_year); // > 2001
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_below_threshold_is_invalid);
    RUN_TEST(test_above_threshold_is_valid);
    RUN_TEST(test_between_old_and_new_threshold_is_invalid);
    RUN_TEST(test_localnow_on_a_real_clock_is_valid);
    return UNITY_END();
}
