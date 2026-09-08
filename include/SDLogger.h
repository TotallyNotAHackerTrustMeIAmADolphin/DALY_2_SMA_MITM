#pragma once
#include <Arduino.h>
#include <vector>
#include "SystemState.h"

// Background SD-card logger for BMS/SMA telemetry and system events.
// Writes happen on a single dedicated FreeRTOS task, fed by a queue, so
// callers on either core never block on (or contend for) the SPI/SD bus.
//
// Reads (log listing/viewing from the web UI) happen on the web server's
// own task and go straight to SD, guarded by sdMutex() so they can't
// interleave with the writer task's file access.
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

    // Lists bare filenames (no leading '/') of .csv/.log files on the card,
    // sorted ascending (oldest date first). Returns false if not ready.
    static bool listLogFiles(std::vector<String> &outNames, std::vector<uint32_t> &outSizes);

    // Reads up to maxBytes from the end of fileName (bare name, must be one
    // returned by listLogFiles) into outContent. Returns false if the file
    // can't be opened.
    static bool readTail(const String &fileName, String &outContent, size_t maxBytes = 65536);

    // Guards all direct (non-queued) SD/SPI access. Held briefly by the
    // writer task around each file write, and by web-route handlers around
    // each read.
    static SemaphoreHandle_t sdMutex();

private:
    static void loggingTask(void *parameter);
    static String currentLogPath(const char *extension);
    static void writeCSVHeaderIfMissing(const String &path);

    static bool initialized;
    static QueueHandle_t logQueue;
    static SemaphoreHandle_t sdMutex_;
};
