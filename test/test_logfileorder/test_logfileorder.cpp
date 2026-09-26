// Native unit tests for the pure log-file name ordering (#97): `pio test -e
// native`. Includes the real include/LogFileOrder.h - no mirrored copy to
// keep in sync.

#include <unity.h>
#include "LogFileOrder.h"

using LogFileOrder::isOlder;

void setUp(void) {}
void tearDown(void) {}

// --- boot_* always sorts before any dated file, regardless of boot ID ---

static void test_boot_sorts_before_dated_files(void)
{
    // A plain byte compare would put "boot_..." AFTER "2026-..." ('b' >
    // '2'), which is exactly the bug this rule guards against.
    TEST_ASSERT_TRUE(isOlder("boot_12345", "2026-01-01.csv"));
    TEST_ASSERT_FALSE(isOlder("2026-01-01.csv", "boot_12345"));

    // Even a boot ID that looks numerically "later" than the date string
    // as text still sorts first, since the boot_ prefix alone decides it.
    TEST_ASSERT_TRUE(isOlder("boot_999999999", "2026-01-01.log"));
}

// --- dated files sort ascending by date ---

static void test_dated_files_sort_ascending_by_date(void)
{
    TEST_ASSERT_TRUE(isOlder("2026-01-01.csv", "2026-01-02.csv"));
    TEST_ASSERT_FALSE(isOlder("2026-01-02.csv", "2026-01-01.csv"));
    TEST_ASSERT_TRUE(isOlder("2026-01-01.csv", "2026-12-31.csv"));
}

// --- boot_* files among themselves: plain byte compare of the full name ---

static void test_boot_files_sort_among_themselves_by_name(void)
{
    TEST_ASSERT_TRUE(isOlder("boot_100", "boot_200"));
    TEST_ASSERT_FALSE(isOlder("boot_200", "boot_100"));
    // Preserves the pre-refactor lambda's exact (non-numeric) behaviour: a
    // plain byte compare, so "boot_2" sorts AFTER "boot_100" (byte '2' >
    // '1') even though 2 < 100 numerically - not fixed here, just kept.
    TEST_ASSERT_TRUE(isOlder("boot_100", "boot_2"));
}

// --- same date, different extension: whatever the current order is (.csv < .log) ---

static void test_same_date_csv_sorts_before_log(void)
{
    TEST_ASSERT_TRUE(isOlder("2026-01-01.csv", "2026-01-01.log"));
    TEST_ASSERT_FALSE(isOlder("2026-01-01.log", "2026-01-01.csv"));
}

// --- irreflexivity: a name is never older than itself ---

static void test_irreflexive(void)
{
    TEST_ASSERT_FALSE(isOlder("2026-01-01.csv", "2026-01-01.csv"));
    TEST_ASSERT_FALSE(isOlder("boot_12345", "boot_12345"));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_sorts_before_dated_files);
    RUN_TEST(test_dated_files_sort_ascending_by_date);
    RUN_TEST(test_boot_files_sort_among_themselves_by_name);
    RUN_TEST(test_same_date_csv_sorts_before_log);
    RUN_TEST(test_irreflexive);
    return UNITY_END();
}
