#pragma once
#include <Arduino.h>
#include "SystemState.h"

// Background SD-card logger for BMS/SMA telemetry and system events.
// All SD access happens on a single dedicated FreeRTOS task, fed by a
// queue, so callers on either core never block on (or contend for) the
// SPI/SD bus.
class SDLogger
{
public:
    // Mounts the card and starts the background writer task.
    // Returns false if no card is present / mount fails.
    static bool begin();

    static bool isReady();

    // Enqueues a telemetry snapshot for the CSV log. Safe to call from any task.
    static void logTelemetry(const DashboardData &data);

    // Enqueues a free-text event line for the .log file. Safe to call from any task.
    static void logEvent(const char *msg);

private:
    static void loggingTask(void *parameter);
    static String currentLogPath(const char *extension);
    static void writeCSVHeaderIfMissing(const String &path);

    static bool initialized;
    static QueueHandle_t logQueue;
};
