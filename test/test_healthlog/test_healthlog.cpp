// Native unit tests for when a health sample gets logged: `pio test -e native`.
// Includes the real include/HealthLog.h - no mirrored copy to keep in sync.

#include <unity.h>
#include "HealthLog.h"

using namespace HealthLog;

static const uint32_t kTen = 10UL * 60UL * 1000UL;

static Sample steady()
{
    Sample s;
    s.freeHeap = 142396;
    s.minFreeHeap = 51540;
    s.maxBlock = 94196;
    uint32_t stacks[kNumTasks] = {5276, 1872, 3680, 4752, 9976, 2848};
    for (int i = 0; i < kNumTasks; i++)
        s.stackLeft[i] = stacks[i];
    return s;
}

void setUp(void) {}
void tearDown(void) {}

void test_first_sample_is_baseline(void)
{
    State st;
    TEST_ASSERT_EQUAL(kBaseline, decide(st, steady(), 60000));
}

void test_steady_device_is_quiet(void)
{
    // The 2026-09-26 log: identical numbers every 10 min, free heap
    // wobbling by ~100 bytes. None of it should be logged.
    State st;
    decide(st, steady(), 0);
    for (uint32_t k = 1; k < 100; k++)
    {
        Sample s = steady();
        s.freeHeap += (k % 2) ? 88 : 0;
        TEST_ASSERT_EQUAL(kNone, decide(st, s, k * kTen));
    }
}

void test_heartbeat_after_a_day(void)
{
    State st;
    decide(st, steady(), 0);
    TEST_ASSERT_EQUAL(kNone, decide(st, steady(), kHeartbeatMs - 1));
    TEST_ASSERT_EQUAL(kHeartbeat, decide(st, steady(), kHeartbeatMs));
    // Heartbeat restarts the clock.
    TEST_ASSERT_EQUAL(kNone, decide(st, steady(), kHeartbeatMs + kTen));
}

void test_heartbeat_across_millis_wraparound(void)
{
    State st;
    decide(st, steady(), 0xFFFFFFFFu - 1000u);
    TEST_ASSERT_EQUAL(kNone, decide(st, steady(), 5000u));
    TEST_ASSERT_EQUAL(kHeartbeat, decide(st, steady(), kHeartbeatMs - 1001u));
}

void test_heap_low_water_drop_boundary(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.minFreeHeap -= kHeapDropBytes - 1;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen));
    s.minFreeHeap = steady().minFreeHeap - kHeapDropBytes;
    TEST_ASSERT_EQUAL(kHeapDrop, decide(st, s, 2 * kTen));
}

void test_slow_heap_drift_accumulates_against_last_logged(void)
{
    // 1 KB per sample never crosses 4 KB step-to-step, but it does
    // against the last logged value.
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    int logged = 0;
    for (int k = 1; k <= 4; k++)
    {
        s.minFreeHeap -= 1024;
        if (decide(st, s, k * kTen) == kHeapDrop)
            logged = k;
    }
    TEST_ASSERT_EQUAL(4, logged);
}

void test_largest_block_shrink_boundary(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.maxBlock -= kBlockDropBytes - 1;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen));
    s.maxBlock = steady().maxBlock - kBlockDropBytes;
    TEST_ASSERT_EQUAL(kBlockDrop, decide(st, s, 2 * kTen));
}

void test_largest_block_recovery_is_quiet(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.maxBlock += 20000;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen));
}

void test_stack_drop_boundary(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.stackLeft[kBms] -= kStackDropBytes - 1;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen));
    s.stackLeft[kBms] = steady().stackLeft[kBms] - kStackDropBytes;
    TEST_ASSERT_EQUAL(kStackDrop, decide(st, s, 2 * kTen));
    // Logged value is the new reference: no repeat.
    TEST_ASSERT_EQUAL(kNone, decide(st, s, 3 * kTen));
}

void test_stack_low_logged_once_and_wins_reason(void)
{
    State st;
    Sample s = steady();
    s.stackLeft[kCan] = 600;
    decide(st, s, 0);
    s.stackLeft[kCan] = kStackLowBytes - 1; // 89 B drop: below drop threshold
    s.minFreeHeap -= kHeapDropBytes;        // also a heap drop
    TEST_ASSERT_EQUAL(kStackLow, decide(st, s, kTen));
    TEST_ASSERT_EQUAL(kNone, decide(st, s, 2 * kTen));
}

void test_late_starting_task_adopts_first_reading(void)
{
    // async_tcp isn't running at the baseline (0 = not found). Its first
    // reading must not count as a drop from 0 or be ignored forever.
    State st;
    Sample s = steady();
    s.stackLeft[kAsyncTcp] = 0;
    decide(st, s, 0);
    s.stackLeft[kAsyncTcp] = 9976;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen));
    s.stackLeft[kAsyncTcp] = 9976 - kStackDropBytes;
    TEST_ASSERT_EQUAL(kStackDrop, decide(st, s, 2 * kTen));
}

void test_task_disappearing_is_not_a_drop(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.stackLeft[kSd] = 0;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_sample_is_baseline);
    RUN_TEST(test_steady_device_is_quiet);
    RUN_TEST(test_heartbeat_after_a_day);
    RUN_TEST(test_heartbeat_across_millis_wraparound);
    RUN_TEST(test_heap_low_water_drop_boundary);
    RUN_TEST(test_slow_heap_drift_accumulates_against_last_logged);
    RUN_TEST(test_largest_block_shrink_boundary);
    RUN_TEST(test_largest_block_recovery_is_quiet);
    RUN_TEST(test_stack_drop_boundary);
    RUN_TEST(test_stack_low_logged_once_and_wins_reason);
    RUN_TEST(test_late_starting_task_adopts_first_reading);
    RUN_TEST(test_task_disappearing_is_not_a_drop);
    return UNITY_END();
}
