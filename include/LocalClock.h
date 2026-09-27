#pragma once

#include <ctime>

// One clock-valid decision, one local-time read, shared by netLog(),
// setupNetwork()'s NTP wait, loop()'s first-health check, and SDLogger's
// row timestamp / log file path, so none of them can disagree about
// whether the wall clock is NTP-synced yet, or read it at slightly
// different instants across a midnight boundary.
namespace LocalClock
{
    // Above this, the clock is NTP-synced rather than still reading the
    // RTC's un-synced epoch (RTC default: 1970).
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
