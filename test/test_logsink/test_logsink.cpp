// Native tests for include/LogSink.h (#84): `pio test -e native`.

#include <unity.h>
#include <cstring>
#include "LogSink.h"

static char g_captured[512];
static int g_calls = 0;

static void capture(const char *line)
{
    g_calls++;
    strncpy(g_captured, line, sizeof(g_captured) - 1);
    g_captured[sizeof(g_captured) - 1] = '\0';
}

void setUp(void)
{
    g_captured[0] = '\0';
    g_calls = 0;
}
void tearDown(void) {}

static void test_null_sink_is_a_noop(void)
{
    logf(nullptr, "%d", 1);
    TEST_ASSERT_EQUAL(0, g_calls);
}

static void test_formats_into_the_sink(void)
{
    logf(capture, "[BMS] SOC=%d%%", 42);
    TEST_ASSERT_EQUAL_STRING("[BMS] SOC=42%", g_captured);
}

// The whole point of taking fmt+args instead of a pre-formatted string: a
// literal '%' inside an argument must survive, not be reinterpreted as one
// of logf's own format specifiers.
static void test_percent_inside_an_argument_passes_through(void)
{
    logf(capture, "%s", "battery at 50% and falling");
    TEST_ASSERT_EQUAL_STRING("battery at 50% and falling", g_captured);
}

static void test_truncates_at_255_chars(void)
{
    char big[400];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    logf(capture, "%s", big);
    TEST_ASSERT_EQUAL(255, strlen(g_captured));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_sink_is_a_noop);
    RUN_TEST(test_formats_into_the_sink);
    RUN_TEST(test_percent_inside_an_argument_passes_through);
    RUN_TEST(test_truncates_at_255_chars);
    return UNITY_END();
}
