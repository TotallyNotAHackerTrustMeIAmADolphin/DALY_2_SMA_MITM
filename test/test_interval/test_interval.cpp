// Native tests for include/Interval.h (#82): `pio test -e native`.

#include <unity.h>
#include "Interval.h"

void setUp(void) {}
void tearDown(void) {}

static void test_first_call_waits_one_period_from_boot(void)
{
    Interval t(250);
    TEST_ASSERT_FALSE(t.due(0));
    TEST_ASSERT_FALSE(t.due(250));
    TEST_ASSERT_TRUE(t.due(251));
}

static void test_exact_period_is_not_due(void)
{
    Interval t(250);
    TEST_ASSERT_TRUE(t.due(1000));
    TEST_ASSERT_FALSE(t.due(1250)); // strictly greater than
    TEST_ASSERT_TRUE(t.due(1251));
}

static void test_restarts_from_the_firing_time(void)
{
    Interval t(100);
    TEST_ASSERT_TRUE(t.due(500));
    TEST_ASSERT_FALSE(t.due(550));
    TEST_ASSERT_TRUE(t.due(700)); // late: next period counts from 700
    TEST_ASSERT_FALSE(t.due(800));
    TEST_ASSERT_TRUE(t.due(801));
}

static void test_millis_wraparound(void)
{
    Interval t(300);
    TEST_ASSERT_TRUE(t.due(0xFFFFFF00u));
    TEST_ASSERT_FALSE(t.due(0x00000009u)); // 265 ms after, across the wrap
    TEST_ASSERT_FALSE(t.due(0x0000002Cu)); // exactly 300
    TEST_ASSERT_TRUE(t.due(0x0000002Du));  // 301
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_call_waits_one_period_from_boot);
    RUN_TEST(test_exact_period_is_not_due);
    RUN_TEST(test_restarts_from_the_firing_time);
    RUN_TEST(test_millis_wraparound);
    return UNITY_END();
}
