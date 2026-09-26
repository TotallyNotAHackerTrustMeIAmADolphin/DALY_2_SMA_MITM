#pragma once
#include <ESPAsyncWebServer.h>
#include "SystemState.h"
#include "LogSink.h"

// Define a callback type for button actions (like resetSMA or toggleMaint)
typedef void (*ActionCallback)(const char *action);

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

    // Attach a debug/diagnostics sink (e.g. netLogLine) for WebDashboard's
    // own log lines - begin()'s null-cfg guard, /save's "busy" refusal,
    // etc. - instead of Serial.println. Call before begin() so nothing
    // logs to Serial-only in between.
    void setDebugCallback(LogSink cb);

    // Push a log line to the Web UI console
    void broadcastLog(const char *msg);

    // Push telemetry data to the Web UI dashboard
    void broadcastTelemetry(const DashboardData &data);

private:
    AsyncWebServer _server;
    AsyncEventSource _events;
    SystemConfig *_cfg = nullptr; // Pointer to the main app's config struct
    ActionCallback _actionCb = nullptr;
    LogSink _debugCb = nullptr;

    // One handler per route (#90); the stateless ones are static.
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

    // Validates the "file" request param against SDLogger::listLogFiles(). On
    // success returns true with outName/outSize populated. On failure it sends
    // the error response itself (400 missing param, 404 unknown file) - callers
    // just do `if (!findLogFile(request, name, size)) return;`.
    static bool findLogFile(AsyncWebServerRequest *request, String &outName, uint32_t &outSize);

    // Picks a Content-Type by file extension (.csv -> text/csv, else text/plain).
    static const char *contentTypeForLogFile(const String &name);
};