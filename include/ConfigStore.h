#pragma once
#include "SystemConfig.h"

// NVS load/store for SystemConfig (#88) - not web code, so split out of
// WebDashboard.
namespace ConfigStore
{
    // Variadic, like netLog - log is a no-op if null.
    typedef void (*LogFn)(const char *fmt, ...);

    // Resets every setting to its default, then takes a stored NVS value
    // only if it has the expected type and set() accepts it. Call before
    // the BMS/CAN tasks start; needs no network.
    void load(SystemConfig &cfg, LogFn log);

    // Writes every setting present[] marks (parallel to cfg.all()) to NVS.
    void store(const SystemConfig &cfg, const bool present[SystemConfig::kNumSettings]);
}
