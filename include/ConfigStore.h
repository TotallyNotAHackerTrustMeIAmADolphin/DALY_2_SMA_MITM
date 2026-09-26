#pragma once
#include "SystemConfig.h"

// NVS load/store for SystemConfig (#88) - split out of WebDashboard, since
// none of it is web code. One namespace key ("bms-bridge") and one
// kind->NVS-type mapping, shared by load() and store().
namespace ConfigStore
{
    // Pre-formatted single-string log sink, same pattern as DalyRS485/
    // SMA_CAN's debug callback (main.cpp's libraryLogger fits this).
    typedef void (*LogFn)(const char *msg);

    // Resets every setting to its default, then takes a stored NVS value
    // only if it has the expected NVS type and set() accepts it; logs
    // (via log, if non-null) a mismatch, an out-of-range stored value, or a
    // loaded config that fails SystemConfig::validate(). Needs no network -
    // call before the BMS/CAN tasks start.
    void load(SystemConfig &cfg, LogFn log);

    // Writes every setting whose index is true in present[] (parallel to
    // cfg.all()) to NVS. Opens and closes its own Preferences handle, so
    // callers don't hold one across other work.
    void store(const SystemConfig &cfg, const bool present[SystemConfig::kNumSettings]);
}
