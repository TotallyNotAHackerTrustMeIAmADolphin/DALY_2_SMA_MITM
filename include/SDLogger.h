#pragma once
#include <Arduino.h>
#include <vector>
#include "SystemState.h"

// Matches DalyRS485/SMA_CAN's existing setDebugCallback pattern, so SD
// mount/init failures reach netLog() (Serial+Telnet+web console) instead of
// only the USB serial port.
typedef void (*SDDebugCallback)(const char *msg);

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

    // Attach a logging function, called for mount/init failures. Set this
    // before begin() to have those failures reach netLog() too.
    static void setDebugCallback(SDDebugCallback cb);

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

    // Decimates a telemetry CSV (bare name, must be one returned by
    // listLogFiles) down to at most targetPoints rows, keeping only the
    // columns needed for graphing (Timestamp,PackV,PackI,SOC,MinCellV,
    // MaxCellV,ReqI), so the *output* stays small regardless of the source
    // file's size. The *input* is capped separately (see .cpp) so a very
    // large source file can't hold sdMutex_ for an unbounded scan - returns
    // false and leaves outCSV empty/error text if fileName exceeds that cap.
    static bool readGraphSeries(const String &fileName, size_t targetPoints, String &outCSV);

    // Guards all direct (non-queued) SD/SPI access. Held briefly by the
    // writer task around each file write, and by web-route handlers around
    // each read.
    static SemaphoreHandle_t sdMutex();

private:
    static void loggingTask(void *parameter);
    static String currentLogPath(const char *extension);
    static void writeCSVHeaderIfMissing(const String &path);
    static void logFailure(const char *msg);

    static bool initialized;
    static QueueHandle_t logQueue;
    static SemaphoreHandle_t sdMutex_;
    static SDDebugCallback debugCb;
};
