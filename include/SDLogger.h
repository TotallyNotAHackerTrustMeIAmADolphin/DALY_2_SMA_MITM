#pragma once
#include <Arduino.h>
#include <vector>
#include "SystemState.h"

// Matches DalyRS485/SMA_CAN's existing setDebugCallback pattern, so SD
// mount/init failures reach netLog() (Serial+web console) instead of
// only the USB serial port.
typedef void (*SDDebugCallback)(const char *msg);

// Named tuning constants for SDLogger.cpp, plus the ones WebDashboard's
// /api/logs/* routes need (the download route's own sdMutex() timeout).
// Grouped here (#100) instead of left as bare literals scattered through
// both files.
namespace SdTuning
{
    // logQueue depth and each LogMessage's payload size (32 * (1 + 480 +
    // padding) bytes of static queue RAM).
    constexpr uint32_t kQueueDepth = 32;
    constexpr size_t kMsgDataBytes = 480;

    // SD_LogTask.
    constexpr uint32_t kWriterTaskStackBytes = 8192;
    constexpr UBaseType_t kWriterTaskPriority = 1;
    constexpr BaseType_t kWriterTaskCore = 0;

    // sdMutex_ acquire timeouts, one per caller. The writer task's is
    // shortest since it runs every queued line and must not stall the
    // queue for long; readers hold it for a bounded scan (see
    // kMaxGraphSourceBytes/kMaxTailSourceBytes in SDLogger.cpp) so can
    // afford to wait longer for a competing reader/writer to finish.
    constexpr TickType_t kWriterLockTimeoutMs = 1000;
    constexpr TickType_t kListLockTimeoutMs = 500;
    constexpr TickType_t kTailLockTimeoutMs = 500;
    constexpr TickType_t kGraphLockTimeoutMs = 2000;
    // WebDashboard's /api/logs/download route, held for the whole transfer.
    constexpr TickType_t kDownloadLockTimeoutMs = 2000;
}

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
    // can't be opened. Keep maxBytes modest (a few KB, not tens of KB): the
    // caller typically copies outContent again into a single contiguous
    // buffer (e.g. AsyncWebServerResponse) - live-tested with an 80KB+ free
    // heap that still had no single ~65KB contiguous block, which made that
    // downstream copy silently produce empty content. 8KB is the current,
    // deliberately conservative default.
    static bool readTail(const String &fileName, String &outContent, size_t maxBytes = 8192);

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
