#pragma once
#include "SystemConfig.h"
#include "LogSink.h"

// NVS load/store for SystemConfig (#88) - not web code, so split out of
// WebDashboard.
namespace ConfigStore
{
    // Resets every setting to its default, then takes a stored NVS value
    // only if it has the expected type and set() accepts it. Call before
    // the BMS/CAN tasks start; needs no network.
    void load(SystemConfig &cfg, LogSink log);

    // Writes every setting present[] marks (parallel to cfg.all()) to NVS.
    void store(const SystemConfig &cfg, const bool present[SystemConfig::kNumSettings]);
}
