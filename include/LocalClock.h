#pragma once

#include <ctime>

// One clock-valid decision, one local-time read, used everywhere main.cpp
// and SDLogger.cpp need to know whether the wall clock is NTP-synced yet
// (#83): netLog(), setupNetwork()'s NTP wait, loop()'s first-health check,
// and SDLogger's row timestamp / log file path all used to run their own
// copy of time()+localtime_r()+a validity threshold, with two different
// thresholds between them (1971 vs 2001).
namespace LocalClock
{
    // Above this, the clock is NTP-synced rather than still reading the
    // RTC's un-synced epoch. The RTC reads 1970 (tm_year > 70 would already
    // pass) until NTP sets it, so a real clock is never caught between 1971
    // and this stricter 2001 threshold - picking it everywhere is a no-op
    // in practice, not a behaviour change.
    constexpr time_t kMinValidEpoch = 1000000000L;

    inline bool clockValid(time_t t) { return t > kMinValidEpoch; }

    // localtime_r() plus the validity check, in one call and one read of
    // the clock, so a caller's timestamp and any related decision (e.g.
    // which log file to write to) always agree even right at midnight.
    inline bool localNow(tm &out)
    {
        time_t now = time(nullptr);
        localtime_r(&now, &out);
        return clockValid(now);
    }
}
