// Native unit tests for the WiFi event latch (#69): `pio test -e native`.
// Includes the real include/WifiEvents.h - no mirrored copy to keep in
// sync. These pin down the exact [WIFI] line semantics of the old volatile
// globals (drain() flags, disconnect reason, down-for duration, first
// connect vs reconnect) plus the two race fixes: check-then-clear on
// drain, and the setupNetwork()/event-task race on the first-connect flag.
// Mirrors test/test_bmsevents's/test_healthlog's style.

#include <unity.h>
#include "WifiEvents.h"

using WifiEvents::Kind;
using WifiEvents::Pending;
using WifiEvents::reasonName;
using WifiEvents::WifiEventLatch;

void setUp(void) {}
void tearDown(void) {}

// --- Disconnect / reconnect ordering, with down-for duration ---

void test_disconnect_then_reconnect_reports_down_duration(void)
{
    WifiEventLatch latch;

    // Boot straight to connected, like setupNetwork() winning the race.
    latch.onEvent(Kind::Connected, 0, 100);
    Pending p0 = latch.drain(100);
    TEST_ASSERT_TRUE(p0.firstConnect);

    latch.onEvent(Kind::Disconnected, 200 /*WIFI_REASON_NO_AP_FOUND*/, 1000);
    Pending p1 = latch.drain(1000);
    TEST_ASSERT_TRUE(p1.disconnect);
    TEST_ASSERT_EQUAL_UINT8(200, p1.disconnectReason);
    TEST_ASSERT_FALSE(p1.reconnect);

    latch.onEvent(Kind::Connected, 0, 8500);
    Pending p2 = latch.drain(8500);
    TEST_ASSERT_FALSE(p2.disconnect);
    TEST_ASSERT_FALSE(p2.firstConnect); // already connected once before
    TEST_ASSERT_TRUE(p2.reconnect);
    TEST_ASSERT_EQUAL_UINT32(7500, p2.downForMs);
}

void test_repeated_disconnect_while_still_down_is_quiet(void)
{
    WifiEventLatch latch;
    latch.onEvent(Kind::Connected, 0, 0);
    latch.drain(0);

    latch.onEvent(Kind::Disconnected, 1 /*unspecified*/, 100);
    TEST_ASSERT_TRUE(latch.drain(100).disconnect);

    // Retry attempts fire more DISCONNECTED events while already down -
    // none of them should produce another pending disconnect.
    latch.onEvent(Kind::Disconnected, 1, 200);
    latch.onEvent(Kind::Disconnected, 1, 300);
    Pending p = latch.drain(300);
    TEST_ASSERT_FALSE(p.disconnect);
    TEST_ASSERT_FALSE(p.reconnect);
}

// --- First-connect suppression, both event orders ---

void test_suppress_first_connect_before_event_arrives(void)
{
    // setupNetwork() polled WiFi.status() and won the race before the
    // GOT_IP event was even dispatched to onEvent().
    WifiEventLatch latch;
    latch.suppressFirstConnect();
    latch.onEvent(Kind::Connected, 0, 50);
    Pending p = latch.drain(50);
    TEST_ASSERT_FALSE(p.firstConnect); // already logged by setupNetwork()
}

void test_suppress_first_connect_after_event_arrives(void)
{
    // The event lands first (setupNetwork() was still polling), then
    // setupNetwork() sees WL_CONNECTED and calls suppressFirstConnect()
    // before drain() ever runs.
    WifiEventLatch latch;
    latch.onEvent(Kind::Connected, 0, 50);
    latch.suppressFirstConnect();
    Pending p = latch.drain(50);
    TEST_ASSERT_FALSE(p.firstConnect);
}

void test_first_connect_logs_once_when_setupnetwork_gives_up(void)
{
    // setupNetwork() gave up waiting (10s timeout) without ever calling
    // suppressFirstConnect() - the event arrives later and drain() must
    // still report it exactly once.
    WifiEventLatch latch;
    latch.onEvent(Kind::Connected, 0, 12000);
    TEST_ASSERT_TRUE(latch.drain(12000).firstConnect);
    // A later disconnect/reconnect must not resurrect the first-connect
    // report as a second one.
    latch.onEvent(Kind::Disconnected, 1, 13000);
    latch.drain(13000);
    latch.onEvent(Kind::Connected, 0, 14000);
    Pending p = latch.drain(14000);
    TEST_ASSERT_FALSE(p.firstConnect);
    TEST_ASSERT_TRUE(p.reconnect);
}

// --- Lost IP ---

void test_lost_ip_is_reported_and_consumed_once(void)
{
    WifiEventLatch latch;
    latch.onEvent(Kind::Connected, 0, 0);
    latch.drain(0);

    latch.onEvent(Kind::LostIp, 0, 500);
    Pending p1 = latch.drain(500);
    TEST_ASSERT_TRUE(p1.lostIp);

    Pending p2 = latch.drain(600);
    TEST_ASSERT_FALSE(p2.lostIp);
}

// --- Check-then-clear race: an event after drain's exchange isn't lost ---

void test_event_arriving_after_drain_shows_up_next_drain(void)
{
    WifiEventLatch latch;
    latch.onEvent(Kind::Connected, 0, 0);
    latch.drain(0);

    latch.onEvent(Kind::Disconnected, 1, 1000);
    Pending p1 = latch.drain(1000); // exchange(false) consumes the flag
    TEST_ASSERT_TRUE(p1.disconnect);

    // Simulates an event landing in the instant right after drain's
    // exchange returned but before drain() finishes running: it must set
    // the flag fresh, not be swallowed by the exchange that already ran.
    latch.onEvent(Kind::LostIp, 0, 1001);
    Pending p2 = latch.drain(1001);
    TEST_ASSERT_FALSE(p2.disconnect); // already consumed, not re-reported
    TEST_ASSERT_TRUE(p2.lostIp);      // new event, not lost
}

// --- reasonName ---

void test_reason_name_known_codes(void)
{
    TEST_ASSERT_EQUAL_STRING("unspecified", reasonName(1));
    TEST_ASSERT_EQUAL_STRING("beacon timeout (weak signal / interference)", reasonName(200));
    TEST_ASSERT_EQUAL_STRING("AP not found (out of range / AP down?)", reasonName(201));
    TEST_ASSERT_EQUAL_STRING("auth failed", reasonName(202));
    TEST_ASSERT_EQUAL_STRING("4-way handshake timeout (wrong password?)", reasonName(15));
}

void test_reason_name_unknown_code(void)
{
    TEST_ASSERT_EQUAL_STRING("see reason code", reasonName(123));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_disconnect_then_reconnect_reports_down_duration);
    RUN_TEST(test_repeated_disconnect_while_still_down_is_quiet);
    RUN_TEST(test_suppress_first_connect_before_event_arrives);
    RUN_TEST(test_suppress_first_connect_after_event_arrives);
    RUN_TEST(test_first_connect_logs_once_when_setupnetwork_gives_up);
    RUN_TEST(test_lost_ip_is_reported_and_consumed_once);
    RUN_TEST(test_event_arriving_after_drain_shows_up_next_drain);
    RUN_TEST(test_reason_name_known_codes);
    RUN_TEST(test_reason_name_unknown_code);
    return UNITY_END();
}
