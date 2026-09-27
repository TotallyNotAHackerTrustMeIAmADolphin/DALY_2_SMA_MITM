// Native unit tests for the single-value moving-average smoother used for
// pack voltage/current: `pio test -e native`. Includes the real
// include/ScalarSmoother.h - no mirrored copy, same pattern as
// test_cellsmoother.

#include <unity.h>
#include "ScalarSmoother.h"

void setUp(void) {}
void tearDown(void) {}

// --- First call reseeds and returns the raw value exactly ---

static void test_first_update_returns_raw_exactly(void)
{
    ScalarSmoother sm;
    float r = sm.update(52.3f, 8);
    TEST_ASSERT_EQUAL_FLOAT(52.3f, r);
}

// --- Full window of constant input converges to (and stays at) that value ---

static void test_constant_input_average_equals_it(void)
{
    ScalarSmoother sm;
    for (int call = 0; call < 5; call++)
        TEST_ASSERT_EQUAL_FLOAT(52.3f, sm.update(52.3f, 5));
}

// --- Step input: hand-derived intermediate average after k updates,
// --- proving this lags a spike the same way CellSmoother's smoothed
// --- min/max do (drives Glideslope, deliberately never fed pack V/I -
// --- see ScalarSmoother.h) ---

static void test_step_input_converges_after_window_updates(void)
{
    ScalarSmoother sm;

    float r = sm.update(50.0f, 4);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, r);

    r = sm.update(54.0f, 4); // [54,50,50,50] -> 51
    TEST_ASSERT_EQUAL_FLOAT(51.0f, r);

    r = sm.update(54.0f, 4); // [54,54,50,50] -> 52
    TEST_ASSERT_EQUAL_FLOAT(52.0f, r);

    r = sm.update(54.0f, 4); // [54,54,54,50] -> 53
    TEST_ASSERT_EQUAL_FLOAT(53.0f, r);

    r = sm.update(54.0f, 4); // fully flushed
    TEST_ASSERT_EQUAL_FLOAT(54.0f, r);
}

// --- Window change reseeds from the current reading, not the old buffer ---

static void test_window_change_reseeds(void)
{
    ScalarSmoother sm;
    sm.update(50.0f, 5);
    float r = sm.update(60.0f, 10); // window changes -> reseed to 60 exactly
    TEST_ASSERT_EQUAL_FLOAT(60.0f, r);
}

// --- Shrinking then growing the window again must not pull stale values back ---

static void test_shrink_then_grow_does_not_pull_stale_values_back(void)
{
    ScalarSmoother sm;
    sm.update(50.0f, 20);
    for (int i = 0; i < 5; i++)
        sm.update(60.0f, 20); // partial window: 5 slots at 60, 15 at 50

    float r = sm.update(55.0f, 3); // shrink -> reseed
    TEST_ASSERT_EQUAL_FLOAT(55.0f, r);

    r = sm.update(45.0f, 20); // grow back -> reseed again, not the 50/60 history
    TEST_ASSERT_EQUAL_FLOAT(45.0f, r);
}

// --- windowSize is clamped to [1, MAX_SAMPLES] ---

static void test_window_size_clamped_to_valid_range(void)
{
    ScalarSmoother sm1;
    TEST_ASSERT_EQUAL_FLOAT(10.0f, sm1.update(10.0f, 0)); // clamps to 1

    ScalarSmoother sm2;
    TEST_ASSERT_EQUAL_FLOAT(10.0f, sm2.update(10.0f, 50)); // clamps to MAX_SAMPLES
    TEST_ASSERT_EQUAL_FLOAT(10.0f, sm2.update(10.0f, ScalarSmoother::MAX_SAMPLES)); // same clamp: no reseed jump
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_update_returns_raw_exactly);
    RUN_TEST(test_constant_input_average_equals_it);
    RUN_TEST(test_step_input_converges_after_window_updates);
    RUN_TEST(test_window_change_reseeds);
    RUN_TEST(test_shrink_then_grow_does_not_pull_stale_values_back);
    RUN_TEST(test_window_size_clamped_to_valid_range);
    return UNITY_END();
}
