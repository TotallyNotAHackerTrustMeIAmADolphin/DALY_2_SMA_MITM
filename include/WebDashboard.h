#pragma once
#include <ESPAsyncWebServer.h>
#include "SystemState.h"

// Define a callback type for button actions (like resetSMA or toggleMaint)
typedef void (*ActionCallback)(const char *action);

// Debug/diagnostics callback, matching netLog()'s own variadic signature
// (src/main.cpp) so it can be wired straight to netLog with no formatting
// shim in between - unlike DalyRS485/SMA_CAN's DalyDebugCallback/
// SMADebugCallback, which take a single already-formatted string and go
// through the libraryLogger() wrapper instead.
typedef void (*WebDebugCallback)(const char *fmt, ...);

class WebDashboard
{
public:
    WebDashboard(uint16_t port = 80);

    // Registers routes and starts the server, using cfg for the /config
    // page and /save (ConfigStore::load() must have populated it already -
    // setup() calls that directly, before the BMS/CAN tasks start, since it
    // needs no network). Call after setupNetwork(): the async TCP stack
    // must be initialised (connected or not). Refuses to start if cfg is
    // null.
    void begin(SystemConfig *cfg);

    // Attach an action listener for the buttons
    void setActionCallback(ActionCallback cb);

    // Attach a debug/diagnostics sink (e.g. netLog) for WebDashboard's own
    // log lines - begin()'s null-cfg guard, /save's "busy" refusal, etc. -
    // instead of Serial.println. Call before begin() so nothing logs to
    // Serial-only in between.
    void setDebugCallback(WebDebugCallback cb);

    // Push a log line to the Web UI console
    void broadcastLog(const char *msg);

    // Push telemetry data to the Web UI dashboard
    void broadcastTelemetry(const DashboardData &data);

private:
    AsyncWebServer _server;
    AsyncEventSource _events;
    SystemConfig *_cfg = nullptr; // Pointer to the main app's config struct
    ActionCallback _actionCb = nullptr;
    WebDebugCallback _debugCb = nullptr;

    // One handler per route (#90) - setupRoutes() just wires each URI to
    // one of these. The stateless ones (page/API handlers below that touch
    // neither _cfg nor _actionCb) are static, so they're registered
    // directly with no capturing-lambda wrapper.
    static void handleIndex(AsyncWebServerRequest *request);
    void handleToggleMaint(AsyncWebServerRequest *request);
    void handleResetSMA(AsyncWebServerRequest *request);
    void handleConfigPage(AsyncWebServerRequest *request);
    void saveConfig(AsyncWebServerRequest *request);
    static void handleLogsPage(AsyncWebServerRequest *request);
    static void handleGraphsPage(AsyncWebServerRequest *request);
    static void handleLogList(AsyncWebServerRequest *request);
    static void handleLogContent(AsyncWebServerRequest *request);
    static void handleLogDownload(AsyncWebServerRequest *request);
    static void handleGraph(AsyncWebServerRequest *request);
    void setupRoutes();

    // Formats like printf and forwards to _debugCb (a no-op if unset). See
    // DalyRS485/SMA_CAN's identically-named helper for the pattern; unlike
    // theirs, this one hands the formatted text to _debugCb as a "%s" arg
    // (not as the format string itself) since _debugCb is itself variadic
    // and a log line containing a literal '%' must not be reinterpreted as
    // a format specifier by netLog's own vsnprintf.
    void debugLog(const char *format, ...) __attribute__((format(printf, 2, 3)));

    // Validates the "file" request param against SDLogger::listLogFiles(). On
    // success returns true with outName/outSize populated. On failure it sends
    // the error response itself (400 missing param, 404 unknown file) - callers
    // just do `if (!findLogFile(request, name, size)) return;`.
    static bool findLogFile(AsyncWebServerRequest *request, String &outName, uint32_t &outSize);

    // Picks a Content-Type by file extension (.csv -> text/csv, else text/plain).
    static const char *contentTypeForLogFile(const String &name);
};