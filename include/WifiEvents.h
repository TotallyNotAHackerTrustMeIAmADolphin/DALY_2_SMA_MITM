#pragma once

// The WiFi event -> [WIFI] log-line latch between the WiFi event task
// (onEvent) and loop() (drain), kept Arduino/ESP-IDF-free so
// test/test_wifievents runs it natively (#69).
//
// - Every pending flag is a std::atomic<bool> consumed with exchange(false),
//   so an event landing mid-drain shows up in the next drain instead of
//   being cleared unread.
// - The first-connect line has two possible reporters, setupNetwork() and
//   drain(). firstConnectState_ moves never-connected -> pending-log ->
//   reported only by atomic compare_exchange/store, so it is logged exactly
//   once whatever the event order.
// - Payloads (reason, down-since) are written before the release store of
//   their flag and read after its acquire exchange.
//
// Benign race: a second disconnect/reconnect cycle between drain() taking
// the reconnect flag and reading downSince_ makes "was down for Ns" report
// the newer cycle. Cosmetic - never a lost or duplicated line.

#include <atomic>
#include <cstdint>

namespace WifiEvents
{

// What wifiEventHandler() actually distinguishes: a completed connection
// (GOT_IP), a disconnection, and losing the IP while still associated.
enum class Kind
{
  Connected,
  Disconnected,
  LostIp
};

// Decoded ESP-IDF WIFI_REASON_* codes for the [WIFI] Disconnected line,
// as literals so this header needs no ESP-IDF include; main.cpp
// static_asserts them against esp_wifi_types.h.
inline const char *reasonName(uint8_t reason)
{
  switch (reason)
  {
  case 1: return "unspecified";                                  // WIFI_REASON_UNSPECIFIED
  case 2: return "auth expired";                                 // WIFI_REASON_AUTH_EXPIRE
  case 4: return "association expired";                          // WIFI_REASON_ASSOC_EXPIRE
  case 8: return "we disconnected (assoc leave)";                // WIFI_REASON_ASSOC_LEAVE
  case 15: return "4-way handshake timeout (wrong password?)";   // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
  case 202: return "auth failed";                                // WIFI_REASON_AUTH_FAIL
  case 201: return "AP not found (out of range / AP down?)";     // WIFI_REASON_NO_AP_FOUND
  case 200: return "beacon timeout (weak signal / interference)"; // WIFI_REASON_BEACON_TIMEOUT
  case 14: return "MIC failure";                                 // WIFI_REASON_MIC_FAILURE
  case 47: return "kicked by AP";                                // WIFI_REASON_AP_INITIATED
  case 36: return "we disconnected (leaving)";                   // WIFI_REASON_STA_LEAVING
  default: return "see reason code";
  }
}

// What drainWifiEvents() needs to emit exactly today's log lines. Order
// matches the original: disconnect, lost-IP, first-connect, reconnect.
struct Pending
{
  bool disconnect = false;
  uint8_t disconnectReason = 0;

  bool lostIp = false;

  bool firstConnect = false;

  bool reconnect = false;
  uint32_t downForMs = 0; // only meaningful when reconnect is true
};

class WifiEventLatch
{
public:
  // Called from the WiFi event task (wifiEventHandler). Must stay
  // allocation- and lock-free, same requirement as before.
  void onEvent(Kind kind, uint8_t reason, uint32_t nowMs)
  {
    switch (kind)
    {
    case Kind::Disconnected:
    {
      // Atomic swap doubles as the "were we actually connected" gate: a
      // repeat DISCONNECTED event while already down (still retrying)
      // finds prevConnected == false and is a no-op, same as the
      // original `if (wifiConnected)` check.
      bool prevConnected = connected_.exchange(false, std::memory_order_acq_rel);
      if (prevConnected)
      {
        downSince_.store(nowMs, std::memory_order_relaxed);
        lastDisconnectReason_.store(reason, std::memory_order_relaxed);
        pendingDisconnect_.store(true, std::memory_order_release);
      }
      break;
    }
    case Kind::Connected:
    {
      bool everBefore = everConnected_.load(std::memory_order_relaxed);
      bool prevConnected = connected_.exchange(true, std::memory_order_acq_rel);
      everConnected_.store(true, std::memory_order_relaxed);
      if (everBefore && !prevConnected)
      {
        pendingReconnect_.store(true, std::memory_order_release);
      }
      else if (!everBefore)
      {
        // Move never-connected -> pending-log. If suppressFirstConnect()
        // already won this race and moved the state to "reported", this
        // compare_exchange simply fails and does nothing - no double log.
        uint8_t expected = kNeverConnected;
        firstConnectState_.compare_exchange_strong(expected, kPendingLog,
                                                     std::memory_order_acq_rel);
      }
      break;
    }
    case Kind::LostIp:
      pendingLostIp_.store(true, std::memory_order_release);
      break;
    }
  }

  // Called from loop() via drainWifiEvents(). Consumes every pending flag
  // exactly once (exchange/compare_exchange, never check-then-clear).
  Pending drain(uint32_t nowMs)
  {
    Pending p;

    if (pendingDisconnect_.exchange(false, std::memory_order_acq_rel))
    {
      p.disconnect = true;
      p.disconnectReason = lastDisconnectReason_.load(std::memory_order_relaxed);
    }

    if (pendingLostIp_.exchange(false, std::memory_order_acq_rel))
    {
      p.lostIp = true;
    }

    // Only a pending-log -> reported transition reports; already
    // never-connected or already-reported states leave this untouched.
    uint8_t expected = kPendingLog;
    if (firstConnectState_.compare_exchange_strong(expected, kReported,
                                                     std::memory_order_acq_rel))
    {
      p.firstConnect = true;
    }

    if (pendingReconnect_.exchange(false, std::memory_order_acq_rel))
    {
      p.reconnect = true;
      p.downForMs = nowMs - downSince_.load(std::memory_order_relaxed);
    }

    return p;
  }

  // Replaces setupNetwork()'s `wifiPendingFirstConnect = false` write.
  // setupNetwork() calls this once it has logged the connect line itself
  // (it won the race against the event/drain path); it moves the state
  // straight to "reported" regardless of the current state, so whether
  // the GOT_IP event's onEvent() call has landed yet or not, the eventual
  // outcome is the same: reported exactly once, by setupNetwork.
  void suppressFirstConnect()
  {
    firstConnectState_.store(kReported, std::memory_order_release);
  }

private:
  enum : uint8_t
  {
    kNeverConnected = 0,
    kPendingLog = 1,
    kReported = 2
  };

  std::atomic<bool> connected_{false};
  std::atomic<bool> everConnected_{false};
  std::atomic<uint32_t> downSince_{0};

  std::atomic<bool> pendingDisconnect_{false};
  std::atomic<uint8_t> lastDisconnectReason_{0};
  std::atomic<bool> pendingLostIp_{false};
  std::atomic<bool> pendingReconnect_{false};
  std::atomic<uint8_t> firstConnectState_{kNeverConnected};
};

} // namespace WifiEvents
