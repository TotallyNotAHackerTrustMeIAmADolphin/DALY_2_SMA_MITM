#pragma once
#include <Arduino.h>
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "CoreDumpInfo.h"
#include "LogSink.h"

class AsyncWebServer; // registerRoutes() only takes a reference

// Boot-reason/core-dump diagnostics, the periodic health log, and the OTA
// rollback-confirmation safety net, callback-wired to netLog like
// DalyRS485/SMA_CAN/SDLogger/WebDashboard rather than calling Serial/netLog
// directly.
class Diagnostics
{
public:
    // Attach a logging sink (netLogLine). Call before any other method here.
    static void setDebugCallback(LogSink cb);

    // Reset reason / rollback state / stored core-dump summary; see
    // Diagnostics.cpp for why it's called where it is.
    static void logBootDiagnostics();

    // The one core-dump reader, shared by the boot log and
    // /api/coredump/summary. Callers never run concurrently.
    static void readCoreDump(CoreDumpInfo &out);

    // esp_core_dump_image_check()'s "nothing stored" results.
    static bool coreDumpAbsent(esp_err_t check) { return check == ESP_ERR_NOT_FOUND || check == ESP_ERR_INVALID_SIZE; }

    // Registers the two coredump routes; order matters, see the comment
    // beside the registrations in Diagnostics.cpp.
    static void registerRoutes(AsyncWebServer &server);

    // Heap/stack/WiFi RSSI sample; only logs when HealthLog::decide() says
    // something moved (include/HealthLog.h).
    static void logHealth();

    // OTA rollback-confirmation safety net - see verifyRollbackLater()
    // below. Call from loop() on its 1s Interval. Confirms the image
    // once both wifiUp and bmsUp have been true for kConfirmAfterMs of
    // uptime, and otherwise logs a one-shot "not confirmed" warning the
    // first time the image is still pending-verify after that deadline.
    static void confirmImageIfReady(bool wifiUp, bool bmsUp);

private:
    static LogSink debugCb;
};

// Defers Arduino's default of marking every new OTA image valid at
// startup, so Diagnostics::confirmImageIfReady() can require WiFi AND real
// BMS data flowing first - a crash-looping or BMS-link-broken build then
// rolls back instead of locking us out of OTA. Overrides a weak symbol in
// the Arduino core, so it must stay a free extern "C" function with this
// exact name, not a Diagnostics member.
extern "C" bool verifyRollbackLater();
