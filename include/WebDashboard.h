#pragma once
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "SystemState.h"

// Define a callback type for button actions (like resetSMA or toggleMaint)
typedef void (*ActionCallback)(const char *action);

class WebDashboard
{
public:
    WebDashboard(uint16_t port = 80);

    // Initializes the server, loads NVS config, and attaches to WiFi
    void begin(SystemConfig &configOut);

    // Attach an action listener for the buttons
    void setActionCallback(ActionCallback cb);

    // Push a log line to the Web UI console
    void broadcastLog(const char *msg);

    // Push telemetry data to the Web UI dashboard
    void broadcastTelemetry(const DashboardData &data);

private:
    AsyncWebServer _server;
    AsyncEventSource _events;
    Preferences _prefs;
    SystemConfig *_cfg; // Pointer to the main app's config struct
    ActionCallback _actionCb;

    void loadConfig();
    void saveConfig(AsyncWebServerRequest *request);
    void setupRoutes();
};