// Native unit tests for the per-cell moving-average smoother (#31, #70):
// `pio test -e native`. Includes the real include/CellSmoother.h - no
// mirrored copy to keep in sync, same pattern as test_glideslope.

#include <unity.h>
#include "CellSmoother.h"

void setUp(void) {}
void tearDown(void) {}

// --- First call: raw passthrough + reseed (regression for the boot swing) ---

static void test_first_update_returns_raw_exactly_and_reseeds(void)
{
    CellSmoother sm;
    uint16_t raw[CellSmoother::MAX_CELLS];
    for (int i = 0; i < CellSmoother::MAX_CELLS; i++)
        raw[i] = 3300 + 10 * i; // 3300 .. 3450 mV

    CellSmoother::Result r = sm.update(raw, CellSmoother::MAX_CELLS, 8);

    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_INT(16, r.cells);
    for (int i = 0; i < CellSmoother::MAX_CELLS; i++)
        TEST_ASSERT_EQUAL_FLOAT(raw[i] / 1000.0f, r.smoothedV[i]);

    TEST_ASSERT_EQUAL_FLOAT(3.30f, r.minV);
    TEST_ASSERT_EQUAL_FLOAT(3.45f, r.maxV);
    TEST_ASSERT_EQUAL_FLOAT(3.30f, r.rawMinV);
    TEST_ASSERT_EQUAL_FLOAT(3.45f, r.rawMaxV);
    TEST_ASSERT_EQUAL_UINT16(150, r.rawSpreadMv);

    // avgV now divides by the cells actually read (16 here): sum of
    // 3300..3450 in 10mV steps = 16*3300 + 10*(0+..+15) = 52800 + 1200 =
    // 54000 mV -> /16 = 3375 mV = 3.375 V, exact (integer mV throughout).
    TEST_ASSERT_EQUAL_FLOAT(3.375f, r.avgV);
}

// --- Full window of constant input converges to (and stays at) that value ---

static void test_constant_input_average_equals_it(void)
{
    CellSmoother sm;
    uint16_t v[1] = {3300};

    for (int call = 0; call < 5; call++)
    {
        CellSmoother::Result r = sm.update(v, 1, 5);
        TEST_ASSERT_EQUAL_FLOAT(3.300f, r.smoothedV[0]);
        TEST_ASSERT_EQUAL_INT(call == 0 ? 1 : 0, r.reseeded ? 1 : 0);
    }
}

// --- Step input: hand-derived intermediate average after k updates ---

static void test_step_input_converges_after_window_updates(void)
{
    CellSmoother sm;
    uint16_t v0[1] = {3000};
    uint16_t v1[1] = {3400};

    CellSmoother::Result r = sm.update(v0, 1, 4);
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.000f, r.smoothedV[0]);

    // Window is [v0,v0,v0,v0]; each call below overwrites one more slot
    // with v1, in ring order, starting at index 0 (reset by the reseed).
    r = sm.update(v1, 1, 4); // slots [v1,v0,v0,v0] -> (3400+3*3000)/4 = 3100
    TEST_ASSERT_FALSE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.100f, r.smoothedV[0]);

    r = sm.update(v1, 1, 4); // slots [v1,v1,v0,v0] -> (2*3400+2*3000)/4 = 3200
    TEST_ASSERT_EQUAL_FLOAT(3.200f, r.smoothedV[0]);

    r = sm.update(v1, 1, 4); // slots [v1,v1,v1,v0] -> (3*3400+3000)/4 = 3300
    TEST_ASSERT_EQUAL_FLOAT(3.300f, r.smoothedV[0]);

    r = sm.update(v1, 1, 4); // slots [v1,v1,v1,v1] -> fully flushed to v1
    TEST_ASSERT_EQUAL_FLOAT(3.400f, r.smoothedV[0]);
}

// --- Window average rounds to nearest mV, not floor (#70) ---

static void test_window_average_rounds_to_nearest_mv(void)
{
    CellSmoother sm;
    uint16_t seed[1] = {3000};
    uint16_t hi[1] = {3001};

    sm.update(seed, 1, 3);       // reseed: window = [3000,3000,3000]
    sm.update(hi, 1, 3);         // window = [3001,3000,3000] -> sum 9001
    CellSmoother::Result r = sm.update(hi, 1, 3); // window = [3001,3001,3000] -> sum 9002

    // 9002/3 = 3000.67, floor would give 3000mV; round-to-nearest gives 3001.
    TEST_ASSERT_EQUAL_FLOAT(3.001f, r.smoothedV[0]);
}

// --- Window change reseeds from the current reading and resets the index ---

static void test_window_change_reseeds_and_resets_index(void)
{
    CellSmoother sm;
    uint16_t a[2] = {3000, 3100};
    uint16_t b[2] = {3050, 3150};
    uint16_t c[2] = {3200, 3300};
    uint16_t d[2] = {3600, 3700};

    sm.update(a, 2, 5);                                   // reseed @ window 5
    CellSmoother::Result r = sm.update(b, 2, 5);           // no reseed, same window
    TEST_ASSERT_FALSE(r.reseeded);

    r = sm.update(c, 2, 10);                               // window changes -> reseed
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.200f, r.smoothedV[0]);
    TEST_ASSERT_EQUAL_FLOAT(3.300f, r.smoothedV[1]);

    // Index was reset to 0 by the reseed above: this call overwrites slot 0
    // only, so the average is (d + 9*c)/10, not some other ring position.
    r = sm.update(d, 2, 10);
    TEST_ASSERT_FALSE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT((3600 + 9 * 3200) / 10.0f / 1000.0f, r.smoothedV[0]);
    TEST_ASSERT_EQUAL_FLOAT((3700 + 9 * 3300) / 10.0f / 1000.0f, r.smoothedV[1]);
}

// --- Shrinking then growing the window again must not pull stale values back in ---

static void test_shrink_then_grow_does_not_pull_stale_values_back(void)
{
    CellSmoother sm;
    uint16_t seed[1] = {3000};
    uint16_t hi[1] = {3500};
    uint16_t shrinkSeed[1] = {3100};
    uint16_t growSeed[1] = {3200};

    sm.update(seed, 1, 20); // reseed @ window 20: all 20 slots = 3000
    for (int i = 0; i < 5; i++)
        sm.update(hi, 1, 20); // slots 0..4 now 3500, slots 5..19 still 3000

    // Shrink to window 3: must reseed from the current reading, not average
    // any of the window-20 history above.
    CellSmoother::Result r = sm.update(shrinkSeed, 1, 3);
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.100f, r.smoothedV[0]);

    // Grow back to window 20: must reseed fresh again, not resurrect the
    // 3000/3500 values still sitting in slots [3..19] from before the
    // shrink (the regression this test exists for).
    r = sm.update(growSeed, 1, 20);
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.200f, r.smoothedV[0]);
}

// --- Raw min/max/spread over 16 distinct cells ---

static void test_raw_min_max_spread_with_16_distinct_cells(void)
{
    CellSmoother sm;
    uint16_t raw[CellSmoother::MAX_CELLS];
    for (int i = 0; i < CellSmoother::MAX_CELLS; i++)
        raw[i] = 3000 + 10 * i; // 3000 .. 3150 mV

    CellSmoother::Result r = sm.update(raw, CellSmoother::MAX_CELLS, 1);

    TEST_ASSERT_EQUAL_FLOAT(3.00f, r.rawMinV);
    TEST_ASSERT_EQUAL_FLOAT(3.15f, r.rawMaxV);
    TEST_ASSERT_EQUAL_UINT16(150, r.rawSpreadMv);
    TEST_ASSERT_EQUAL_INT(16, r.cells);
}

// --- n < MAX_CELLS handled: only the first n cells are touched, avgV divides
// --- by cells actually read, not MAX_CELLS (#70) ---

static void test_n_less_than_max_cells_handled(void)
{
    CellSmoother sm;
    uint16_t raw[4] = {3100, 3200, 3050, 3150};

    CellSmoother::Result r = sm.update(raw, 4, 6);

    TEST_ASSERT_EQUAL_INT(4, r.cells);
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.10f, r.smoothedV[0]);
    TEST_ASSERT_EQUAL_FLOAT(3.20f, r.smoothedV[1]);
    TEST_ASSERT_EQUAL_FLOAT(3.05f, r.smoothedV[2]);
    TEST_ASSERT_EQUAL_FLOAT(3.15f, r.smoothedV[3]);
    // Untouched slots stay at their default-constructed value, not garbage.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.smoothedV[4]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.smoothedV[15]);

    TEST_ASSERT_EQUAL_FLOAT(3.05f, r.rawMinV);
    TEST_ASSERT_EQUAL_FLOAT(3.20f, r.rawMaxV);
    TEST_ASSERT_EQUAL_UINT16(150, r.rawSpreadMv);

    // avgV over the 4 cells actually read (was, before #70, a bug-for-bug
    // `/MAX_CELLS`, pinned "as-is" - now the real average of the 4 reads).
    TEST_ASSERT_EQUAL_FLOAT((3100 + 3200 + 3050 + 3150) / 4.0f / 1000.0f, r.avgV);
}

// --- n > MAX_CELLS is clamped, not read out of bounds ---

static void test_n_greater_than_max_cells_clamped(void)
{
    CellSmoother sm;
    uint16_t raw[20];
    for (int i = 0; i < 20; i++)
        raw[i] = 3000 + 10 * i;

    CellSmoother::Result r = sm.update(raw, 20, 4);

    TEST_ASSERT_EQUAL_INT(CellSmoother::MAX_CELLS, r.cells);
    TEST_ASSERT_EQUAL_FLOAT(3.15f, r.rawMaxV); // index 15, not one of the extra 4 entries
}

// --- n == 0: zeroed Result, no state touched (defensive; unreachable in
// --- firmware today since kPackCells is a fixed positive constant, #70) ---

static void test_zero_cells_returns_zeroed_result_without_touching_state(void)
{
    CellSmoother sm;
    uint16_t seed[1] = {3300};
    sm.update(seed, 1, 5); // establish state that a bad n=0 call must not disturb

    CellSmoother::Result r = sm.update(nullptr, 0, 5);
    TEST_ASSERT_EQUAL_INT(0, r.cells);
    TEST_ASSERT_FALSE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.minV);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.maxV);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.avgV);
    TEST_ASSERT_EQUAL_UINT16(0, r.rawSpreadMv);

    // Prior state is untouched: the next real reading isn't wrongly reseeded.
    r = sm.update(seed, 1, 5);
    TEST_ASSERT_FALSE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.300f, r.smoothedV[0]);
}

// --- windowSize is clamped to [1, MAX_SAMPLES] ---

static void test_window_size_clamped_to_valid_range(void)
{
    CellSmoother sm1;
    uint16_t v[1] = {3300};
    CellSmoother::Result r = sm1.update(v, 1, 0); // clamps to 1
    TEST_ASSERT_EQUAL_FLOAT(3.300f, r.smoothedV[0]);
    r = sm1.update(v, 1, 0);
    TEST_ASSERT_FALSE(r.reseeded); // window 0 and window 0 both clamp to 1: no change

    CellSmoother sm2;
    r = sm2.update(v, 1, 50); // clamps to MAX_SAMPLES
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.300f, r.smoothedV[0]);
    r = sm2.update(v, 1, CellSmoother::MAX_SAMPLES); // same clamped value: no reseed
    TEST_ASSERT_FALSE(r.reseeded);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_update_returns_raw_exactly_and_reseeds);
    RUN_TEST(test_constant_input_average_equals_it);
    RUN_TEST(test_step_input_converges_after_window_updates);
    RUN_TEST(test_window_average_rounds_to_nearest_mv);
    RUN_TEST(test_window_change_reseeds_and_resets_index);
    RUN_TEST(test_shrink_then_grow_does_not_pull_stale_values_back);
    RUN_TEST(test_raw_min_max_spread_with_16_distinct_cells);
    RUN_TEST(test_n_less_than_max_cells_handled);
    RUN_TEST(test_n_greater_than_max_cells_clamped);
    RUN_TEST(test_zero_cells_returns_zeroed_result_without_touching_state);
    RUN_TEST(test_window_size_clamped_to_valid_range);
    return UNITY_END();
}
