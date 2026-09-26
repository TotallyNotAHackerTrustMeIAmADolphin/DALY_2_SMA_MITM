// Native unit tests for the OTA rollback-confirmation decision (#58/#57):
// `pio test -e native`. Includes the real include/RollbackConfirm.h - no
// mirrored copy to keep in sync. These tests pin down the exact semantics
// of the old inline Diagnostics::confirmImageIfReady() code before it was
// extracted, so a future change to decide() that silently changes when the
// image gets confirmed (or when the warning fires) fails here first.
// Mirrors test/test_bmsevents's style.

#include <unity.h>
#include "RollbackConfirm.h"

using RollbackConfirm::Action;
using RollbackConfirm::decide;
using RollbackConfirm::kConfirmAfterMs;
using RollbackConfirm::State;

void setUp(void) {}
void tearDown(void) {}

// --- Boundary: nowMs == kConfirmAfterMs is a no-op for every input combo ---

static void test_boundary_at_deadline_is_noop_wifi_and_bms_up(void)
{
    State st;
    Action a = decide(st, true, true, kConfirmAfterMs, false);
    TEST_ASSERT_FALSE(a.confirmNow);
    TEST_ASSERT_FALSE(a.logNotConfirmed);
    TEST_ASSERT_FALSE(st.imageConfirmed);
    TEST_ASSERT_FALSE(st.unconfirmedWarned);
}

static void test_boundary_at_deadline_is_noop_wifi_down(void)
{
    State st;
    Action a = decide(st, false, true, kConfirmAfterMs, true);
    TEST_ASSERT_FALSE(a.confirmNow);
    TEST_ASSERT_FALSE(a.logNotConfirmed);
    TEST_ASSERT_FALSE(st.imageConfirmed);
    TEST_ASSERT_FALSE(st.unconfirmedWarned);
}

static void test_boundary_at_deadline_is_noop_bms_down(void)
{
    State st;
    Action a = decide(st, true, false, kConfirmAfterMs, true);
    TEST_ASSERT_FALSE(a.confirmNow);
    TEST_ASSERT_FALSE(a.logNotConfirmed);
    TEST_ASSERT_FALSE(st.imageConfirmed);
    TEST_ASSERT_FALSE(st.unconfirmedWarned);
}

static void test_boundary_at_deadline_is_noop_both_down(void)
{
    State st;
    Action a = decide(st, false, false, kConfirmAfterMs, true);
    TEST_ASSERT_FALSE(a.confirmNow);
    TEST_ASSERT_FALSE(a.logNotConfirmed);
    TEST_ASSERT_FALSE(st.imageConfirmed);
    TEST_ASSERT_FALSE(st.unconfirmedWarned);
}

// --- Confirm path + idempotent second call ---

static void test_confirm_when_wifi_and_bms_up_past_deadline(void)
{
    State st;
    Action a = decide(st, true, true, kConfirmAfterMs + 1, false);
    TEST_ASSERT_TRUE(a.confirmNow);
    TEST_ASSERT_FALSE(a.logNotConfirmed);
    TEST_ASSERT_TRUE(st.imageConfirmed);
}

static void test_confirm_is_idempotent_on_second_call(void)
{
    State st;
    decide(st, true, true, kConfirmAfterMs + 1, false);

    Action a2 = decide(st, true, true, kConfirmAfterMs + 1000, false);
    TEST_ASSERT_FALSE(a2.confirmNow);
    TEST_ASSERT_FALSE(a2.logNotConfirmed);
}

// --- Pending-verify warn path + idempotent second call ---

static void test_warn_when_pending_verify_and_not_ready(void)
{
    State st;
    Action a = decide(st, false, true, kConfirmAfterMs + 1, true);
    TEST_ASSERT_FALSE(a.confirmNow);
    TEST_ASSERT_TRUE(a.logNotConfirmed);
    TEST_ASSERT_TRUE(st.unconfirmedWarned);
    TEST_ASSERT_FALSE(st.imageConfirmed);
}

static void test_warn_is_idempotent_on_second_call(void)
{
    State st;
    decide(st, false, true, kConfirmAfterMs + 1, true);

    Action a2 = decide(st, false, true, kConfirmAfterMs + 1000, true);
    TEST_ASSERT_FALSE(a2.confirmNow);
    TEST_ASSERT_FALSE(a2.logNotConfirmed);
    TEST_ASSERT_TRUE(st.unconfirmedWarned);
}

// --- Not-pending path: sets unconfirmedWarned with no log line ---

static void test_not_pending_sets_warned_flag_without_logging(void)
{
    State st;
    Action a = decide(st, false, true, kConfirmAfterMs + 1, false);
    TEST_ASSERT_FALSE(a.confirmNow);
    TEST_ASSERT_FALSE(a.logNotConfirmed);
    TEST_ASSERT_TRUE(st.unconfirmedWarned);
    TEST_ASSERT_FALSE(st.imageConfirmed);
}

static void test_not_pending_second_call_is_noop_even_if_now_pending(void)
{
    State st;
    decide(st, false, true, kConfirmAfterMs + 1, false);

    // Even if the caller now (incorrectly, or on a later real pending-verify
    // read) passes imagePendingVerify=true, the warn branch has already run
    // once and must not fire again.
    Action a2 = decide(st, false, true, kConfirmAfterMs + 1000, true);
    TEST_ASSERT_FALSE(a2.confirmNow);
    TEST_ASSERT_FALSE(a2.logNotConfirmed);
}

// --- Confirm still possible after the warning already fired ---

static void test_confirm_still_possible_after_warning_fired(void)
{
    State st;
    // First call: not ready, pending-verify -> warns.
    Action a1 = decide(st, false, false, kConfirmAfterMs + 1, true);
    TEST_ASSERT_TRUE(a1.logNotConfirmed);
    TEST_ASSERT_TRUE(st.unconfirmedWarned);
    TEST_ASSERT_FALSE(st.imageConfirmed);

    // Later call: WiFi + BMS both come up -> still confirms, even though
    // unconfirmedWarned is already true (the wifiUp&&bmsUp branch is
    // checked before unconfirmedWarned).
    Action a2 = decide(st, true, true, kConfirmAfterMs + 5000, false);
    TEST_ASSERT_TRUE(a2.confirmNow);
    TEST_ASSERT_FALSE(a2.logNotConfirmed);
    TEST_ASSERT_TRUE(st.imageConfirmed);
}

int main(int, char **)
{
    UNITY_BEGIN();

    RUN_TEST(test_boundary_at_deadline_is_noop_wifi_and_bms_up);
    RUN_TEST(test_boundary_at_deadline_is_noop_wifi_down);
    RUN_TEST(test_boundary_at_deadline_is_noop_bms_down);
    RUN_TEST(test_boundary_at_deadline_is_noop_both_down);

    RUN_TEST(test_confirm_when_wifi_and_bms_up_past_deadline);
    RUN_TEST(test_confirm_is_idempotent_on_second_call);

    RUN_TEST(test_warn_when_pending_verify_and_not_ready);
    RUN_TEST(test_warn_is_idempotent_on_second_call);

    RUN_TEST(test_not_pending_sets_warned_flag_without_logging);
    RUN_TEST(test_not_pending_second_call_is_noop_even_if_now_pending);

    RUN_TEST(test_confirm_still_possible_after_warning_fired);

    return UNITY_END();
}
