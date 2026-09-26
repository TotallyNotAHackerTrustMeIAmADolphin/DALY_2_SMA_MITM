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

static void test_first_sample_is_baseline(void)
{
    State st;
    TEST_ASSERT_EQUAL(kBaseline, decide(st, steady(), 60000).reason);
}

static void test_steady_device_is_quiet(void)
{
    // The 2026-09-26 log: identical numbers every 10 min, free heap
    // wobbling by ~100 bytes. None of it should be logged.
    State st;
    decide(st, steady(), 0);
    for (uint32_t k = 1; k < 100; k++)
    {
        Sample s = steady();
        s.freeHeap += (k % 2) ? 88 : 0;
        TEST_ASSERT_EQUAL(kNone, decide(st, s, k * kTen).reason);
    }
}

static void test_heartbeat_after_a_day(void)
{
    State st;
    decide(st, steady(), 0);
    TEST_ASSERT_EQUAL(kNone, decide(st, steady(), kHeartbeatMs - 1).reason);
    TEST_ASSERT_EQUAL(kHeartbeat, decide(st, steady(), kHeartbeatMs).reason);
    // Heartbeat restarts the clock.
    TEST_ASSERT_EQUAL(kNone, decide(st, steady(), kHeartbeatMs + kTen).reason);
}

static void test_heartbeat_across_millis_wraparound(void)
{
    State st;
    decide(st, steady(), 0xFFFFFFFFu - 1000u);
    TEST_ASSERT_EQUAL(kNone, decide(st, steady(), 5000u).reason);
    TEST_ASSERT_EQUAL(kHeartbeat, decide(st, steady(), kHeartbeatMs - 1001u).reason);
}

static void test_heap_low_water_drop_boundary(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.minFreeHeap -= kHeapDropBytes - 1;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen).reason);
    s.minFreeHeap = steady().minFreeHeap - kHeapDropBytes;
    TEST_ASSERT_EQUAL(kHeapDrop, decide(st, s, 2 * kTen).reason);
}

static void test_slow_heap_drift_accumulates_against_last_logged(void)
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
        if (decide(st, s, k * kTen).reason == kHeapDrop)
            logged = k;
    }
    TEST_ASSERT_EQUAL(4, logged);
}

static void test_largest_block_shrink_boundary(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.maxBlock -= kBlockDropBytes - 1;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen).reason);
    s.maxBlock = steady().maxBlock - kBlockDropBytes;
    TEST_ASSERT_EQUAL(kBlockDrop, decide(st, s, 2 * kTen).reason);
}

static void test_largest_block_recovery_is_quiet(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.maxBlock += 20000;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen).reason);
}

static void test_stack_drop_boundary(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.stackLeft[kBms] -= kStackDropBytes - 1;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen).reason);
    s.stackLeft[kBms] = steady().stackLeft[kBms] - kStackDropBytes;
    TEST_ASSERT_EQUAL(kStackDrop, decide(st, s, 2 * kTen).reason);
    // Logged value is the new reference: no repeat.
    TEST_ASSERT_EQUAL(kNone, decide(st, s, 3 * kTen).reason);
}

static void test_stack_low_wins_reason_and_names_task(void)
{
    State st;
    Sample s = steady();
    s.stackLeft[kCan] = 600;
    decide(st, s, 0);
    s.stackLeft[kCan] = kStackLowBytes - 1; // 89 B drop: below drop threshold
    s.minFreeHeap -= kHeapDropBytes;        // also a heap drop
    Decision d = decide(st, s, kTen);
    TEST_ASSERT_EQUAL(kStackLow, d.reason);
    TEST_ASSERT_EQUAL(kCan, d.task);
    TEST_ASSERT_EQUAL(kNone, decide(st, s, 2 * kTen).reason);
}

static void test_stack_low_logs_every_further_drop(void)
{
    // Review finding: 500 -> 245 B is under the 256 B drop threshold but
    // eats half the remaining headroom. Below kStackLowBytes any drop logs.
    State st;
    Sample s = steady();
    s.stackLeft[kCan] = 500;
    decide(st, s, 0);
    s.stackLeft[kCan] = 499;
    TEST_ASSERT_EQUAL(kStackLow, decide(st, s, kTen).reason);
    s.stackLeft[kCan] = 245;
    TEST_ASSERT_EQUAL(kStackLow, decide(st, s, 2 * kTen).reason);
    TEST_ASSERT_EQUAL(kNone, decide(st, s, 3 * kTen).reason);
}

static void test_exhausted_stack_zero_is_stack_low(void)
{
    // Review finding: a found task at 0 bytes left used to look like
    // "task not found". kNoTask is the not-found marker now.
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.stackLeft[kBms] = 0;
    Decision d = decide(st, s, kTen);
    TEST_ASSERT_EQUAL(kStackLow, d.reason);
    TEST_ASSERT_EQUAL(kBms, d.task);
}

static void test_late_starting_task_adopts_first_reading(void)
{
    // async_tcp isn't running at the baseline. Its first healthy reading
    // is adopted quietly, and later drops are measured from it.
    State st;
    Sample s = steady();
    s.stackLeft[kAsyncTcp] = kNoTask;
    decide(st, s, 0);
    s.stackLeft[kAsyncTcp] = 9976;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen).reason);
    s.stackLeft[kAsyncTcp] = 9976 - kStackDropBytes;
    TEST_ASSERT_EQUAL(kStackDrop, decide(st, s, 2 * kTen).reason);
}

static void test_late_starting_task_already_low_is_flagged(void)
{
    State st;
    Sample s = steady();
    s.stackLeft[kAsyncTcp] = kNoTask;
    decide(st, s, 0);
    s.stackLeft[kAsyncTcp] = 400;
    Decision d = decide(st, s, kTen);
    TEST_ASSERT_EQUAL(kStackLow, d.reason);
    TEST_ASSERT_EQUAL(kAsyncTcp, d.task);
}

static void test_task_disappearing_is_logged(void)
{
    // Review finding: a task that exits used to go silent.
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.stackLeft[kSd] = kNoTask;
    Decision d = decide(st, s, kTen);
    TEST_ASSERT_EQUAL(kTaskGone, d.reason);
    TEST_ASSERT_EQUAL(kSd, d.task);
    TEST_ASSERT_EQUAL(kNone, decide(st, s, 2 * kTen).reason);
}

// --- SD drop/failure counters (#99): any increase logs, once ---

static void test_sd_drops_increase_triggers_once(void)
{
    State st;
    decide(st, steady(), 0);
    Sample s = steady();
    s.sdDroppedQueueFull += 1;
    Decision d = decide(st, s, kTen);
    TEST_ASSERT_EQUAL(kSdDrops, d.reason);
    // Logged value is the new reference: no repeat with the same counters.
    TEST_ASSERT_EQUAL(kNone, decide(st, s, 2 * kTen).reason);
}

static void test_sd_drops_each_counter_triggers_independently(void)
{
    State st;
    decide(st, steady(), 0);

    Sample s1 = steady();
    s1.sdDroppedLockTimeout += 1;
    TEST_ASSERT_EQUAL(kSdDrops, decide(st, s1, kTen).reason);

    Sample s2 = steady();
    s2.sdDroppedLockTimeout = s1.sdDroppedLockTimeout; // already-seen value
    s2.sdWriteFailures += 1;
    TEST_ASSERT_EQUAL(kSdDrops, decide(st, s2, 2 * kTen).reason);
}

static void test_sd_drops_no_increase_no_trigger(void)
{
    // A steady device with some already-nonzero SD counters from earlier
    // in its uptime must not re-log every cycle just because they're > 0.
    State st;
    Sample baseline = steady();
    baseline.sdDroppedQueueFull = 3;
    baseline.sdDroppedLockTimeout = 1;
    baseline.sdWriteFailures = 2;
    decide(st, baseline, 0);

    for (uint32_t k = 1; k < 10; k++)
        TEST_ASSERT_EQUAL(kNone, decide(st, baseline, k * kTen).reason);
}

static void test_sd_drops_baseline_captures_nonzero_counters(void)
{
    // The very first sample (kBaseline) must adopt whatever the counters
    // already are as the reference, not silently assume 0 - otherwise the
    // very next identical sample would wrongly look like an increase.
    State st;
    Sample s = steady();
    s.sdDroppedQueueFull = 5;
    s.sdDroppedLockTimeout = 2;
    s.sdWriteFailures = 7;
    TEST_ASSERT_EQUAL(kBaseline, decide(st, s, 0).reason);
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen).reason);
}

static void test_block_reference_ratchets_up(void)
{
    // Review finding: a baseline taken while buffers were held (60 KB)
    // must not hide a later 39 KB loss from the 94 KB steady state.
    State st;
    Sample s = steady();
    s.maxBlock = 60000;
    decide(st, s, 0);
    s.maxBlock = 94196;
    TEST_ASSERT_EQUAL(kNone, decide(st, s, kTen).reason);
    s.maxBlock = 55000;
    TEST_ASSERT_EQUAL(kBlockDrop, decide(st, s, 2 * kTen).reason);
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
    RUN_TEST(test_stack_low_wins_reason_and_names_task);
    RUN_TEST(test_stack_low_logs_every_further_drop);
    RUN_TEST(test_exhausted_stack_zero_is_stack_low);
    RUN_TEST(test_late_starting_task_adopts_first_reading);
    RUN_TEST(test_late_starting_task_already_low_is_flagged);
    RUN_TEST(test_task_disappearing_is_logged);
    RUN_TEST(test_sd_drops_increase_triggers_once);
    RUN_TEST(test_sd_drops_each_counter_triggers_independently);
    RUN_TEST(test_sd_drops_no_increase_no_trigger);
    RUN_TEST(test_sd_drops_baseline_captures_nonzero_counters);
    RUN_TEST(test_block_reference_ratchets_up);
    return UNITY_END();
}
