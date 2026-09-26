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

// --- Up to and including kConfirmAfterMs, decide() is a no-op for every
// input combination ---

static void test_noop_until_deadline_for_every_input(void)
{
    const unsigned long times[] = {0, kConfirmAfterMs - 1, kConfirmAfterMs};
    for (unsigned long nowMs : times)
        for (int in = 0; in < 8; in++)
        {
            bool wifiUp = in & 1, bmsUp = in & 2, pending = in & 4;
            State st;
            Action a = decide(st, wifiUp, bmsUp, nowMs, pending);
            TEST_ASSERT_FALSE(a.confirmNow);
            TEST_ASSERT_FALSE(a.logNotConfirmed);
            TEST_ASSERT_FALSE(st.imageConfirmed);
            TEST_ASSERT_FALSE(st.unconfirmedWarned);
        }
}

// --- needsPendingVerify() is true exactly when decide() reads
// imagePendingVerify, i.e. when its answer changes the Action (#105) ---

static void test_needs_pending_verify_matches_decide(void)
{
    const unsigned long times[] = {0, kConfirmAfterMs, kConfirmAfterMs + 1, kConfirmAfterMs + 60000};
    for (unsigned long nowMs : times)
        for (int in = 0; in < 16; in++)
        {
            State st;
            st.imageConfirmed = in & 1;
            st.unconfirmedWarned = in & 2;
            bool wifiUp = in & 4, bmsUp = in & 8;

            State withPending = st, withoutPending = st;
            Action aTrue = decide(withPending, wifiUp, bmsUp, nowMs, true);
            Action aFalse = decide(withoutPending, wifiUp, bmsUp, nowMs, false);
            bool readsIt = aTrue.logNotConfirmed != aFalse.logNotConfirmed;

            TEST_ASSERT_EQUAL(readsIt, RollbackConfirm::needsPendingVerify(st, wifiUp, bmsUp, nowMs));
        }
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

// --- needsInputs(): lets loop() skip the dataMutex take for bmsUp (#75) ---

static void test_needs_inputs_false_until_after_deadline(void)
{
    State st;
    TEST_ASSERT_FALSE(RollbackConfirm::needsInputs(st, 0));
    TEST_ASSERT_FALSE(RollbackConfirm::needsInputs(st, kConfirmAfterMs));
    TEST_ASSERT_TRUE(RollbackConfirm::needsInputs(st, kConfirmAfterMs + 1));
}

static void test_needs_inputs_true_after_warning_false_after_confirm(void)
{
    // After the one-shot warning the image can still be confirmed later,
    // so the inputs are still needed; once confirmed, never again.
    State st;
    decide(st, false, false, kConfirmAfterMs + 1000, true);
    TEST_ASSERT_TRUE(st.unconfirmedWarned);
    TEST_ASSERT_TRUE(RollbackConfirm::needsInputs(st, kConfirmAfterMs + 2000));
    decide(st, true, true, kConfirmAfterMs + 3000, false);
    TEST_ASSERT_FALSE(RollbackConfirm::needsInputs(st, kConfirmAfterMs + 4000));
}

int main(int, char **)
{
    UNITY_BEGIN();

    RUN_TEST(test_noop_until_deadline_for_every_input);
    RUN_TEST(test_needs_pending_verify_matches_decide);

    RUN_TEST(test_confirm_when_wifi_and_bms_up_past_deadline);
    RUN_TEST(test_confirm_is_idempotent_on_second_call);

    RUN_TEST(test_warn_when_pending_verify_and_not_ready);
    RUN_TEST(test_warn_is_idempotent_on_second_call);

    RUN_TEST(test_not_pending_sets_warned_flag_without_logging);
    RUN_TEST(test_not_pending_second_call_is_noop_even_if_now_pending);

    RUN_TEST(test_confirm_still_possible_after_warning_fired);

    RUN_TEST(test_needs_inputs_false_until_after_deadline);
    RUN_TEST(test_needs_inputs_true_after_warning_false_after_confirm);

    return UNITY_END();
}
