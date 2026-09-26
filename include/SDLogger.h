#pragma once
#include <Arduino.h>
#include <vector>
#include <atomic>
#include <memory>
#include "SystemState.h"
#include "MutexLock.h"

// Matches DalyRS485/SMA_CAN's existing setDebugCallback pattern, so SD
// mount/init failures reach netLog() (Serial+web console) instead of
// only the USB serial port.
typedef void (*SDDebugCallback)(const char *msg);

// Background SD-card logger for BMS/SMA telemetry and system events.
// Writes happen on a single dedicated FreeRTOS task, fed by a queue, so
// callers on either core never block on (or contend for) the SPI/SD bus.
// Reads go straight to SD under the same lock, so they can't interleave
// with the writer's file access.
class SDLogger
{
public:
    // One entry from listLogFiles(): a bare filename (no leading '/') and
    // its size in bytes.
    struct LogFileInfo
    {
        String name;
        uint32_t size;
    };

    // What readTail()/readGraphSeries() can return: Ok, a busy lock (or
    // the card never mounted), the file not opening, or the source file
    // over this function's own size cap.
    enum class ReadResult
    {
        Ok,
        Busy,
        NotFound,
        TooLarge,
    };

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

    // Lists .csv/.log files on the card, oldest first (see LogFileOrder.h).
    // False if not ready or busy - not the same as "no files yet".
    static bool listLogFiles(std::vector<LogFileInfo> &outFiles);

    // Reads up to maxBytes from the end of fileName (a name listLogFiles()
    // returned). Kept small (default 8KB): the caller copies it again into
    // one contiguous response buffer, and heap fragmentation can make a
    // much larger single allocation fail silently.
    static ReadResult readTail(const String &fileName, String &outContent, size_t maxBytes = 8192);

    // Decimates a telemetry CSV (a name listLogFiles() returned) down to
    // at most targetPoints rows of Timestamp,PackV,PackI,SOC,MinCellV,
    // MaxCellV,ReqI, so the output stays small regardless of source size.
    static ReadResult readGraphSeries(const String &fileName, size_t targetPoints, String &outCSV);

    // Holds the SD lock for a whole download and resolves fileName to its
    // SD path in outPath. Null if the lock couldn't be acquired. The
    // caller resets the shared_ptr when its connection closes, releasing
    // the lock exactly then.
    static std::shared_ptr<MutexLock> beginDownload(const String &fileName, String &outPath);

    // Snapshot of the writer's drop/failure counters, folded into the
    // periodic health log.
    struct Stats
    {
        uint32_t droppedQueueFull;
        uint32_t droppedLockTimeout;
        uint32_t writeFailures;
    };
    static Stats stats();

private:
    static void loggingTask(void *parameter);
    static String currentLogPath(const char *extension);
    static void writeCSVHeaderIfMissing(const String &path);
    static void logFailure(const char *msg);

    // Bare listLogFiles() name -> SD path.
    static String pathFor(const String &bareName);

    static bool initialized;
    static QueueHandle_t logQueue;
    static SemaphoreHandle_t sdMutex_;

    // Backing counters for stats().
    static std::atomic<uint32_t> droppedQueueFull_;
    static std::atomic<uint32_t> droppedLockTimeout_;
    static std::atomic<uint32_t> writeFailures_;
    static SDDebugCallback debugCb;
};
