// Native unit tests for the per-cell moving-average smoother (#31):
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
    float raw[CellSmoother::MAX_CELLS];
    for (int i = 0; i < CellSmoother::MAX_CELLS; i++)
        // Built from an integer mV base (single division), not repeated
        // 0.01f addition: the latter drifts enough in binary32 that the mV
        // truncating cast ((uint16_t)(v*1000.0f)) can round a cell down by
        // 1mV, which this test would then wrongly read as a smoothing bug.
        raw[i] = (3300 + 10 * i) / 1000.0f; // 3.30 .. 3.45

    CellSmoother::Result r = sm.update(raw, CellSmoother::MAX_CELLS, 8);

    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_INT(16, r.cells);
    for (int i = 0; i < CellSmoother::MAX_CELLS; i++)
        TEST_ASSERT_EQUAL_FLOAT(raw[i], r.smoothedV[i]);

    TEST_ASSERT_EQUAL_FLOAT(3.30f, r.minV);
    TEST_ASSERT_EQUAL_FLOAT(3.45f, r.maxV);
    TEST_ASSERT_EQUAL_FLOAT(3.30f, r.rawMinV);
    TEST_ASSERT_EQUAL_FLOAT(3.45f, r.rawMaxV);
    TEST_ASSERT_EQUAL_UINT16(150, r.rawSpreadMv);

    // avgV mirrors bmsTask's original `sum / MAX_CELLS`: sum of 3.30..3.45
    // in 0.01 steps = 16*3.30 + 0.01*(0+..+15) = 52.8 + 1.2 = 54.0 -> /16 =
    // 3.375. Tolerance, not exact equality: the mV truncating cast
    // ((uint16_t)(v*1000.0f)) can drop a cell's fractional mV when 0.01f
    // isn't exactly representable in binary32, and 16 float32 additions
    // aren't bit-exact either - both real, expected float noise, not a bug.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.375f, r.avgV);
}

// --- Full window of constant input converges to (and stays at) that value ---

static void test_constant_input_average_equals_it(void)
{
    CellSmoother sm;
    float v[1] = {3.300f};

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
    float v0[1] = {3.000f};
    float v1[1] = {3.400f};

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

// --- Window change reseeds from the current reading and resets the index ---

static void test_window_change_reseeds_and_resets_index(void)
{
    CellSmoother sm;
    float a[2] = {3.000f, 3.100f};
    float b[2] = {3.050f, 3.150f};
    float c[2] = {3.200f, 3.300f};
    float d[2] = {3.600f, 3.700f};

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
    TEST_ASSERT_EQUAL_FLOAT((3.600f + 9.0f * 3.200f) / 10.0f, r.smoothedV[0]);
    TEST_ASSERT_EQUAL_FLOAT((3.700f + 9.0f * 3.300f) / 10.0f, r.smoothedV[1]);
}

// --- Shrinking then growing the window again must not pull stale values back in ---

static void test_shrink_then_grow_does_not_pull_stale_values_back(void)
{
    CellSmoother sm;
    float seed[1] = {3.000f};
    float hi[1] = {3.500f};
    float shrinkSeed[1] = {3.100f};
    float growSeed[1] = {3.200f};

    sm.update(seed, 1, 20); // reseed @ window 20: all 20 slots = 3.000
    for (int i = 0; i < 5; i++)
        sm.update(hi, 1, 20); // slots 0..4 now 3.500, slots 5..19 still 3.000

    // Shrink to window 3: must reseed from the current reading, not average
    // any of the window-20 history above.
    CellSmoother::Result r = sm.update(shrinkSeed, 1, 3);
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.100f, r.smoothedV[0]);

    // Grow back to window 20: must reseed fresh again, not resurrect the
    // 3.000/3.500 values still sitting in slots [3..19] from before the
    // shrink (the regression this test exists for).
    r = sm.update(growSeed, 1, 20);
    TEST_ASSERT_TRUE(r.reseeded);
    TEST_ASSERT_EQUAL_FLOAT(3.200f, r.smoothedV[0]);
}

// --- Raw min/max/spread over 16 distinct cells ---

static void test_raw_min_max_spread_with_16_distinct_cells(void)
{
    CellSmoother sm;
    float raw[CellSmoother::MAX_CELLS];
    for (int i = 0; i < CellSmoother::MAX_CELLS; i++)
        raw[i] = 3.00f + 0.01f * i; // 3.00 .. 3.15

    CellSmoother::Result r = sm.update(raw, CellSmoother::MAX_CELLS, 1);

    TEST_ASSERT_EQUAL_FLOAT(3.00f, r.rawMinV);
    TEST_ASSERT_EQUAL_FLOAT(3.15f, r.rawMaxV);
    TEST_ASSERT_EQUAL_UINT16(150, r.rawSpreadMv);
    TEST_ASSERT_EQUAL_INT(16, r.cells);
}

// --- n < MAX_CELLS handled: only the first n cells are touched ---

static void test_n_less_than_max_cells_handled(void)
{
    CellSmoother sm;
    float raw[4] = {3.10f, 3.20f, 3.05f, 3.15f};

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

    // avgV mirrors bmsTask's original `sum / MAX_CELLS` (not `/ cells`),
    // preserved as-is for a fewer-than-16 input - see CellSmoother.h.
    // Tolerance for the same float32 summation noise as the test above.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, (3.10f + 3.20f + 3.05f + 3.15f) / 16.0f, r.avgV);
}

// --- n > MAX_CELLS is clamped, not read out of bounds ---

static void test_n_greater_than_max_cells_clamped(void)
{
    CellSmoother sm;
    float raw[20];
    for (int i = 0; i < 20; i++)
        raw[i] = 3.00f + 0.01f * i;

    CellSmoother::Result r = sm.update(raw, 20, 4);

    TEST_ASSERT_EQUAL_INT(CellSmoother::MAX_CELLS, r.cells);
    TEST_ASSERT_EQUAL_FLOAT(3.15f, r.rawMaxV); // index 15, not one of the extra 4 entries
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_update_returns_raw_exactly_and_reseeds);
    RUN_TEST(test_constant_input_average_equals_it);
    RUN_TEST(test_step_input_converges_after_window_updates);
    RUN_TEST(test_window_change_reseeds_and_resets_index);
    RUN_TEST(test_shrink_then_grow_does_not_pull_stale_values_back);
    RUN_TEST(test_raw_min_max_spread_with_16_distinct_cells);
    RUN_TEST(test_n_less_than_max_cells_handled);
    RUN_TEST(test_n_greater_than_max_cells_clamped);
    return UNITY_END();
}
