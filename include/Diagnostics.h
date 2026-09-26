#pragma once
#include <Arduino.h>
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "CoreDumpInfo.h"

class AsyncWebServer; // registerRoutes() only takes a reference

// Debug/diagnostics callback, matching netLog()'s own variadic signature
// (src/main.cpp) - same pattern as WebDashboard's WebDebugCallback, so it
// can be wired straight to netLog with setDebugCallback(netLog) below, no
// formatting shim needed at the call site.
typedef void (*DiagDebugCallback)(const char *fmt, ...);

// Boot-reason/core-dump diagnostics, the periodic health log, and the OTA
// rollback-confirmation safety net - moved out of src/main.cpp (#21) as a
// self-contained unit, callback-wired to netLog like DalyRS485/SMA_CAN/
// SDLogger/WebDashboard rather than calling Serial/netLog directly.
class Diagnostics
{
public:
    // Attach a logging sink (netLog). Call before logBootDiagnostics() /
    // logHealth() / confirmImageIfReady() so nothing here logs to
    // Serial-only in between.
    static void setDebugCallback(DiagDebugCallback cb);

    static const char *resetReasonName(esp_reset_reason_t r);
    static const char *otaStateName(esp_ota_img_states_t s);

    // Reset reason / rollback state / stored core-dump summary. Called
    // once from setup(), right after SD init, so even a reset within the
    // first 60s of a cold boot (or a crash loop) still gets logged - it
    // does not wait for loop()'s clock-ready gate.
    static void logBootDiagnostics();

    // The one core-dump reader (#91), shared by the boot log and
    // /api/coredump/summary. Callers: setup() (before the web server
    // starts) and the async_tcp task, never concurrently.
    static void readCoreDump(CoreDumpInfo &out);

    // esp_core_dump_image_check()'s "nothing stored" results.
    static bool coreDumpAbsent(esp_err_t check) { return check == ESP_ERR_NOT_FOUND || check == ESP_ERR_INVALID_SIZE; }

    // The running image's ELF sha256, first 16 hex chars.
    static void runningElfSha(char (&out)[17]);

    // Registers the two coredump routes (#90); order matters, see the
    // comment beside the registrations in Diagnostics.cpp.
    static void registerRoutes(AsyncWebServer &server);

    // Heap free/min/max-block, per-task stack high-water marks and WiFi
    // RSSI. Looks up every task (including BMS_Task/CAN_Task) by name via
    // xTaskGetHandle, so no task handles need to be passed in. Called once
    // early (as soon as the clock/BMS data settle) and every 10 minutes
    // after that, but only LOGS when HealthLog::decide() says something
    // moved (heap/stack shrank past a threshold) or a day has passed - see
    // include/HealthLog.h. The first call always logs a baseline.
    static void logHealth();

    // True while confirmImageIfReady() can still act on wifiUp/bmsUp:
    // after kConfirmAfterMs and until the image is confirmed. loop() only
    // gathers bmsUp (a dataMutex take) while this is true (#75).
    static bool confirmCheckDue();

    // OTA rollback-confirmation safety net - see verifyRollbackLater()
    // below. Call from loop() while confirmCheckDue() with the wifiUp/bmsUp
    // predicate (bmsUp needs dataMutex, so that part stays in main.cpp).
    // Confirms the image once
    // both have been true for kConfirmAfterMs of uptime (cancelling the
    // rollback), and otherwise logs a one-shot "not confirmed" warning the
    // first time the image is still pending-verify after that deadline.
    static void confirmImageIfReady(bool wifiUp, bool bmsUp);

private:
    // Formats like printf and forwards to debugCb (a no-op if unset) as a
    // single "%s" argument - see WebDashboard::debugLog's identical pattern
    // for why: debugCb is itself variadic (netLog), and a log line
    // containing a literal '%' must not be reinterpreted as one of its own
    // format specifiers.
    static void debugLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

    static DiagDebugCallback debugCb;
};

// The bootloader supports app rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE),
// but Arduino's default marks every new image valid right at startup, which
// defeats it. Returning true here defers that: Diagnostics::confirmImageIfReady()
// (called from loop()) confirms the image only after kConfirmAfterMs of
// uptime with WiFi connected AND real BMS data flowing - WiFi alone would
// confirm a build with a broken RS485/CAN path. If a freshly OTA'd image
// crashes (or is reset) before that, the bootloader falls back to the
// previous firmware - so a crash-looping (or BMS-link-broken) build can't
// lock us out of OTA.
//
// Overrides a weak symbol in the Arduino core, so it must stay a free
// extern "C" function with this exact name - it cannot be a Diagnostics
// member (extern "C" linkage doesn't apply to class members).
extern "C" bool verifyRollbackLater();
