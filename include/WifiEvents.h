#pragma once

// WiFi event -> [WIFI] log-line latch shared between the WiFi event task
// (onEvent) and loop() (drain), kept Arduino/ESP-IDF-free so
// test/test_wifievents runs it natively (#69). Every pending flag is a
// std::atomic consumed via exchange/compare_exchange, never plain
// check-then-clear, since onEvent() and drain()/suppressFirstConnect() run
// on different tasks and can genuinely race.

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
  case 1: return "unspecified";                                   // WIFI_REASON_UNSPECIFIED
  case 2: return "auth expired";                                  // WIFI_REASON_AUTH_EXPIRE
  case 4: return "association expired";                           // WIFI_REASON_ASSOC_EXPIRE
  case 8: return "we disconnected (assoc leave)";                 // WIFI_REASON_ASSOC_LEAVE
  case 15: return "4-way handshake timeout (wrong password?)";    // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
  case 202: return "auth failed";                                 // WIFI_REASON_AUTH_FAIL
  case 201: return "AP not found (out of range / AP down?)";      // WIFI_REASON_NO_AP_FOUND
  case 200: return "beacon timeout (weak signal / interference)"; // WIFI_REASON_BEACON_TIMEOUT
  case 14: return "MIC failure";                                  // WIFI_REASON_MIC_FAILURE
  case 47: return "kicked by AP";                                 // WIFI_REASON_AP_INITIATED
  case 36: return "we disconnected (leaving)";                    // WIFI_REASON_STA_LEAVING
  default: return "see reason code";
  }
}

// What drainWifiEvents() needs to emit today's log lines.
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
  // WiFi event task. Must stay allocation- and lock-free.
  void onEvent(Kind kind, uint8_t reason, uint32_t nowMs)
  {
    switch (kind)
    {
    case Kind::Disconnected:
    {
      // exchange doubles as the "were we actually connected" gate: a
      // repeat DISCONNECTED while already down is a no-op.
      if (connected_.exchange(false, std::memory_order_acq_rel))
      {
        downSince_.store(nowMs, std::memory_order_relaxed);
        lastDisconnectReason_.store(reason, std::memory_order_relaxed);
        pendingDisconnect_.store(true, std::memory_order_release);
      }
      break;
    }
    case Kind::Connected:
    {
      bool everBefore = everConnected_.exchange(true, std::memory_order_acq_rel);
      bool prevConnected = connected_.exchange(true, std::memory_order_acq_rel);
      if (everBefore && !prevConnected)
        pendingReconnect_.store(true, std::memory_order_release);
      else if (!everBefore)
        pendingFirstConnect_.store(true, std::memory_order_release);
      break;
    }
    case Kind::LostIp:
      pendingLostIp_.store(true, std::memory_order_release);
      break;
    }
  }

  // loop(), via drainWifiEvents(). Consumes every pending flag exactly once.
  Pending drain(uint32_t nowMs)
  {
    Pending p;

    if (pendingDisconnect_.exchange(false, std::memory_order_acq_rel))
    {
      p.disconnect = true;
      p.disconnectReason = lastDisconnectReason_.load(std::memory_order_relaxed);
    }

    if (pendingLostIp_.exchange(false, std::memory_order_acq_rel))
      p.lostIp = true;

    // Only report if we win the never-reported -> reported transition;
    // a suppressFirstConnect() that already won it makes this a no-op.
    if (pendingFirstConnect_.exchange(false, std::memory_order_acq_rel) &&
        !reported_.exchange(true, std::memory_order_acq_rel))
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

  // setupNetwork() calls this once it has logged the first-connect line
  // itself, having won the race against the event/drain path, so the
  // eventual outcome is "reported exactly once" either way.
  void suppressFirstConnect()
  {
    reported_.store(true, std::memory_order_release);
  }

private:
  std::atomic<bool> connected_{false};
  std::atomic<bool> everConnected_{false};
  std::atomic<uint32_t> downSince_{0};

  std::atomic<bool> pendingDisconnect_{false};
  std::atomic<uint8_t> lastDisconnectReason_{0};
  std::atomic<bool> pendingLostIp_{false};
  std::atomic<bool> pendingReconnect_{false};
  std::atomic<bool> pendingFirstConnect_{false};
  std::atomic<bool> reported_{false};
};

} // namespace WifiEvents
