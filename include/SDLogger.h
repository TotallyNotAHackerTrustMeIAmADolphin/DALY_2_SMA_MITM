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
// own task and go straight to SD, guarded by sdMutex_ (private - see
// beginDownload() for the one case a caller outside this class needs to
// hold it) so they can't interleave with the writer task's file access.
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

    // What readTail()/readGraphSeries() can report (#98), mapped by
    // WebDashboard to a status code: Ok -> 200, Busy -> 503 (sdMutex_
    // timeout, or the card was never mounted - try again shortly),
    // NotFound -> 404 (the file didn't open - normally caught earlier by
    // findLogFile()'s own listing check, but a bare "can't be opened"
    // failure could also mean it was deleted in the race between listing
    // and reading), TooLarge -> 413 (over the function's own source-size
    // cap - see kMaxTailSourceBytes/kMaxGraphSourceBytes in the .cpp).
    enum class ReadResult
    {
        Ok,
        Busy,
        NotFound,
        TooLarge,
    };

    // A held lease on sdMutex_ for the duration of a full-file download
    // (/api/logs/download), acquired by beginDownload(). Move-only RAII:
    // held() reports whether the lock was actually acquired (false on
    // timeout, in which case there's nothing to release); the destructor
    // releases it exactly once, whichever of the caller's own exit paths
    // runs. AsyncWebServer's onDisconnect callback is a std::function, so
    // it must be copyable - wrap the lease in a std::shared_ptr to move it
    // into that callback (see beginDownload()'s own comment).
    class DownloadLease
    {
    public:
        DownloadLease() = default;
        ~DownloadLease() { release(); }

        DownloadLease(DownloadLease &&other) noexcept : mutex_(other.mutex_) { other.mutex_ = nullptr; }
        DownloadLease &operator=(DownloadLease &&other) noexcept
        {
            if (this != &other)
            {
                release();
                mutex_ = other.mutex_;
                other.mutex_ = nullptr;
            }
            return *this;
        }
        DownloadLease(const DownloadLease &) = delete;
        DownloadLease &operator=(const DownloadLease &) = delete;

        bool held() const { return mutex_ != nullptr; }

        // Releases the lock now, if held, instead of waiting for the
        // destructor - so a caller holding this via std::shared_ptr (see
        // beginDownload()'s comment) can drop it exactly when its
        // onDisconnect fires, not merely whenever that shared_ptr's last
        // reference happens to go away.
        void release()
        {
            if (mutex_)
            {
                xSemaphoreGive(mutex_);
                mutex_ = nullptr;
            }
        }

    private:
        friend class SDLogger;
        explicit DownloadLease(SemaphoreHandle_t mutex) : mutex_(mutex) {}
        SemaphoreHandle_t mutex_ = nullptr;
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

    // Lists .csv/.log files on the card, sorted ascending (oldest date
    // first; see include/LogFileOrder.h for the exact ordering, including
    // where boot_* fallback files land). Returns false if not ready or the
    // card is busy (sdMutex_ timeout) - not the same as "no files yet".
    static bool listLogFiles(std::vector<LogFileInfo> &outFiles);

    // Reads up to maxBytes from the end of fileName (bare name, must be one
    // returned by listLogFiles) into outContent. Keep maxBytes modest (a
    // few KB, not tens of KB): the caller typically copies outContent
    // again into a single contiguous buffer (e.g. AsyncWebServerResponse)
    // - live-tested with an 80KB+ free heap that still had no single
    // ~65KB contiguous block, which made that downstream copy silently
    // produce empty content. 8KB is the current, deliberately conservative
    // default. See ReadResult for what a non-Ok return means, including
    // TooLarge above this function's own source-size cap (independent of
    // maxBytes, which only bounds the *retained* tail, not the source file
    // readTail() is willing to scan).
    static ReadResult readTail(const String &fileName, String &outContent, size_t maxBytes = 8192);

    // Decimates a telemetry CSV (bare name, must be one returned by
    // listLogFiles) down to at most targetPoints rows, keeping only the
    // columns needed for graphing (Timestamp,PackV,PackI,SOC,MinCellV,
    // MaxCellV,ReqI), so the *output* stays small regardless of the source
    // file's size. The *input* is capped separately (see .cpp) so a very
    // large source file can't hold sdMutex_ for an unbounded scan - see
    // ReadResult for what a non-Ok return means (outCSV is left empty in
    // every non-Ok case).
    static ReadResult readGraphSeries(const String &fileName, size_t targetPoints, String &outCSV);

    // Acquires sdMutex_ for the whole duration of a full-file download and
    // resolves fileName (bare name, must be one returned by listLogFiles)
    // to its SD path in outPath - the one place outside this class that
    // needs to hold sdMutex_ across an entire request (WebDashboard's
    // /api/logs/download route just hands outPath to request->send(SD,
    // ...) and holds onto the returned lease until the response finishes
    // or the client disconnects). Check the returned lease's held() before
    // using outPath - it's left empty if the lock couldn't be acquired
    // within SdTuning::kDownloadLockTimeoutMs.
    //
    // Returned by value (move-only): the caller wraps it in a
    // std::shared_ptr to move it into an AsyncWebServer onDisconnect
    // callback, since those are std::function and must be copyable, then
    // calls release() (or resets the shared_ptr) inside that callback so
    // the lock drops exactly when the connection closes rather than
    // whenever the shared_ptr's last reference happens to be destroyed.
    static DownloadLease beginDownload(const String &fileName, String &outPath);

private:
    static void loggingTask(void *parameter);
    static String currentLogPath(const char *extension);
    static void writeCSVHeaderIfMissing(const String &path);
    static void logFailure(const char *msg);

    // The one place the bare-name -> SD path rule ("/" + name) is spelled
    // out (#98) - every direct-SD-access function below goes through it,
    // instead of repeating "/" + fileName at each call site.
    static String pathFor(const String &bareName);

    static bool initialized;
    static QueueHandle_t logQueue;
    static SemaphoreHandle_t sdMutex_;
    static SDDebugCallback debugCb;
};
