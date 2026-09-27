#include "SDLogger.h"
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <algorithm>
#include "pin_config.h"
#include "esp_task_wdt.h"
#include "TelemetrySchema.h"
#include "CsvDecimation.h"
#include "LocalClock.h"

bool SDLogger::initialized = false;
QueueHandle_t SDLogger::logQueue = NULL;
SemaphoreHandle_t SDLogger::sdMutex_ = NULL;
LogSink SDLogger::debugCb = nullptr;
std::atomic<uint32_t> SDLogger::droppedQueueFull_{0};
std::atomic<uint32_t> SDLogger::droppedLockTimeout_{0};
std::atomic<uint32_t> SDLogger::writeFailures_{0};

namespace
{
    enum class MsgType : char
    {
        Telemetry = 'T',
        Event = 'E',
    };

    constexpr size_t kMsgDataBytes = 480;
    struct LogMessage
    {
        MsgType type;
        char data[kMsgDataBytes];
    };

    // Bounds sdMutex_ hold time for a two-pass scan of a huge source file.
    constexpr uint32_t kMaxGraphSourceBytes = 4 * 1024 * 1024;
    // readTail() can't seek (see its own comment) and scans from 0 for its
    // whole hold, so this cap is much tighter.
    constexpr uint32_t kMaxTailSourceBytes = 512 * 1024;
    constexpr size_t kMaxGraphTargetPoints = 2000;
    constexpr size_t kGraphOutBufBytes = 2048;
    constexpr size_t kSdReadChunkBytes = 512;

    constexpr TickType_t kWriterLockTimeout = pdMS_TO_TICKS(1000);
    constexpr TickType_t kListLockTimeout = pdMS_TO_TICKS(500);
    constexpr TickType_t kTailLockTimeout = pdMS_TO_TICKS(500);
    constexpr TickType_t kGraphLockTimeout = pdMS_TO_TICKS(2000);
    constexpr TickType_t kDownloadLockTimeout = pdMS_TO_TICKS(2000);

    // One open + size check; closes and reports why on failure.
    SDLogger::ReadResult openBounded(const String &path, uint32_t maxBytes, File &outFile, uint32_t &outSize)
    {
        outFile = SD.open(path, FILE_READ);
        if (!outFile)
            return SDLogger::ReadResult::NotFound;

        uint32_t size = outFile.size();
        if (size > maxBytes)
        {
            outFile.close();
            return SDLogger::ReadResult::TooLarge;
        }

        outSize = size;
        return SDLogger::ReadResult::Ok;
    }

    // All web routes run on async_tcp: if it already holds sdMutex_ (a
    // download in progress), fail now instead of waiting on itself (#116).
    TickType_t webLockTimeout(SemaphoreHandle_t mutex, TickType_t t)
    {
        if (xSemaphoreGetMutexHolder(mutex) == xTaskGetCurrentTaskHandle())
            return 0;
        return t;
    }

    // Reads file in kSdReadChunkBytes pieces up to limitBytes, calling
    // onChunk(data, len) per chunk and feeding the watchdog along the way.
    template <class F>
    void streamChunks(File &file, uint32_t limitBytes, F onChunk)
    {
        uint8_t buf[kSdReadChunkBytes];
        uint32_t bytesRead = 0;
        uint32_t chunkCount = 0;
        int n;
        while (bytesRead < limitBytes && (n = file.read(buf, sizeof(buf))) > 0)
        {
            onChunk(buf, (size_t)n);
            bytesRead += (uint32_t)n;
            if (++chunkCount % 8 == 0)
            {
                esp_task_wdt_reset();
                vTaskDelay(1);
            }
        }
    }
}

void SDLogger::setDebugCallback(LogSink cb)
{
    debugCb = cb;
}

void SDLogger::logFailure(const char *msg)
{
    if (debugCb)
        debugCb(msg);
    else
        Serial.println(msg);
}

bool SDLogger::begin()
{
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS))
    {
        logFailure("[SD] Card mount failed");
        return false;
    }

    if (SD.cardType() == CARD_NONE)
    {
        logFailure("[SD] No SD card attached");
        return false;
    }

    logQueue = xQueueCreate(32, sizeof(LogMessage));
    if (logQueue == NULL)
    {
        logFailure("[SD] Failed to create log queue");
        return false;
    }

    sdMutex_ = xSemaphoreCreateMutex();
    if (sdMutex_ == NULL)
    {
        logFailure("[SD] Failed to create SD mutex");
        return false;
    }

    xTaskCreatePinnedToCore(loggingTask, "SD_LogTask", 8192, NULL, 1, NULL, 0);

    initialized = true;
    return true;
}

SDLogger::Stats SDLogger::stats()
{
    return {
        droppedQueueFull_.load(std::memory_order_relaxed),
        droppedLockTimeout_.load(std::memory_order_relaxed),
        writeFailures_.load(std::memory_order_relaxed),
    };
}

void SDLogger::logTelemetry(const DashboardData &data)
{
    if (!initialized)
        return;

    LogMessage msg;
    msg.type = MsgType::Telemetry;
    TelemetrySchema::formatRow(data, msg.data, sizeof(msg.data));

    if (xQueueSend(logQueue, &msg, 0) != pdTRUE)
        droppedQueueFull_.fetch_add(1, std::memory_order_relaxed);
}

void SDLogger::logEvent(const char *msg_text)
{
    if (!initialized)
        return;

    LogMessage msg;
    msg.type = MsgType::Event;
    strncpy(msg.data, msg_text, sizeof(msg.data) - 1);
    msg.data[sizeof(msg.data) - 1] = '\0';

    if (xQueueSend(logQueue, &msg, 0) != pdTRUE)
        droppedQueueFull_.fetch_add(1, std::memory_order_relaxed);
}

String SDLogger::currentLogPath(const tm &timeinfo, bool haveClock, const char *extension)
{
    char fileName[32];
    if (haveClock)
    {
        strftime(fileName, sizeof(fileName), "/%Y-%m-%d", &timeinfo);
    }
    else
    {
        // NTP hasn't synced yet: group everything from this boot into one file.
        static uint32_t bootId = millis();
        snprintf(fileName, sizeof(fileName), "/boot_%u", bootId);
    }

    strncat(fileName, extension, sizeof(fileName) - strlen(fileName) - 1);
    return String(fileName);
}

void SDLogger::writeCSVHeaderIfMissing(const String &path)
{
    if (SD.exists(path))
        return;

    File file = SD.open(path, FILE_WRITE);
    if (!file)
    {
        writeFailures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    char header[512];
    TelemetrySchema::formatHeader(header, sizeof(header));
    file.print(header);
    file.println();
    file.close();
}

void SDLogger::loggingTask(void *parameter)
{
    LogMessage msg;
    while (true)
    {
        if (xQueueReceive(logQueue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        struct tm timeinfo;
        bool haveClock = LocalClock::localNow(timeinfo);

        char timeStr[32];
        if (haveClock)
        {
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        }
        else
        {
            snprintf(timeStr, sizeof(timeStr), "UP:%lu", millis() / 1000);
        }

        if (xSemaphoreTake(sdMutex_, kWriterLockTimeout) != pdTRUE)
        {
            // No netLog() here: that would recurse back through logEvent().
            droppedLockTimeout_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (msg.type == MsgType::Telemetry)
        {
            String path = currentLogPath(timeinfo, haveClock, ".csv");
            writeCSVHeaderIfMissing(path);
            File file = SD.open(path, FILE_APPEND);
            if (file)
            {
                file.printf("%s,%s\n", timeStr, msg.data);
                file.close();
            }
            else
            {
                writeFailures_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else if (msg.type == MsgType::Event)
        {
            File file = SD.open(currentLogPath(timeinfo, haveClock, ".log"), FILE_APPEND);
            if (file)
            {
                file.printf("[%s] %s", timeStr, msg.data);
                file.close();
            }
            else
            {
                writeFailures_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        xSemaphoreGive(sdMutex_);
    }
}

bool SDLogger::listLogFiles(std::vector<LogFileInfo> &outFiles)
{
    outFiles.clear();

    if (!initialized) // no card mounted: empty list, not a busy-card error
        return true;

    if (xSemaphoreTake(sdMutex_, webLockTimeout(sdMutex_, kListLockTimeout)) != pdTRUE)
        return false;

    File root = SD.open("/");
    if (root)
    {
        File file = root.openNextFile();
        while (file)
        {
            if (!file.isDirectory())
            {
                String name = String(file.name());
                if (name.startsWith("/"))
                    name.remove(0, 1);
                if (name.endsWith(".csv") || name.endsWith(".log"))
                    outFiles.push_back({name, (uint32_t)file.size()});
            }
            file = root.openNextFile();
        }
        root.close();
    }

    xSemaphoreGive(sdMutex_);

    // Ordering rule: boot_* files (written before NTP syncs, named by boot
    // ID not date) always sort first - a plain byte compare would otherwise
    // put them after any digit-starting date - then a byte compare within
    // either group ("select most recent = last").
    std::sort(outFiles.begin(), outFiles.end(),
              [](const LogFileInfo &a, const LogFileInfo &b)
              {
                  bool aBoot = a.name.startsWith("boot_");
                  bool bBoot = b.name.startsWith("boot_");
                  if (aBoot != bBoot)
                      return aBoot;
                  return a.name < b.name;
              });

    return true;
}

SDLogger::ReadResult SDLogger::readTail(const String &fileName, String &outContent, size_t maxBytes)
{
    outContent = "";

    if (!initialized)
        return ReadResult::Busy;

    MutexLock lock(sdMutex_, webLockTimeout(sdMutex_, kTailLockTimeout));
    if (!lock)
        return ReadResult::Busy;

    File file;
    uint32_t sourceSize = 0;
    ReadResult openResult = openBounded("/" + fileName, kMaxTailSourceBytes, file, sourceSize);
    if (openResult != ReadResult::Ok)
        return openResult;

    // The source size is already known, so the last maxBytes bytes can be
    // picked out in one sequential pass - no rolling front-erase needed.
    // Sequential reads only: seeking near EOF on a file reopened for
    // FILE_APPEND hundreds of times made the next read() return 0 bytes.
    size_t skip = (sourceSize > maxBytes) ? (sourceSize - maxBytes) : 0;
    outContent.reserve(maxBytes);
    uint32_t seen = 0;
    streamChunks(file, sourceSize, [&](const uint8_t *data, size_t len)
                 {
        size_t start = (seen < skip) ? std::min<size_t>(skip - seen, len) : 0;
        if (start < len)
            outContent.concat((const char *)data + start, (unsigned int)(len - start));
        seen += (uint32_t)len; });
    file.close();

    // The retained tail started mid-line: drop its leading partial line (up
    // to and including the first '\n'), unless that newline is the tail's
    // very last byte, in which case stripping it would empty the buffer.
    if (skip > 0)
    {
        int nl = outContent.indexOf('\n');
        if (nl >= 0 && (size_t)nl < outContent.length() - 1)
            outContent.remove(0, nl + 1);
    }

    return ReadResult::Ok;
}

SDLogger::ReadResult SDLogger::readGraphSeries(const String &fileName, size_t targetPoints, String &outCSV)
{
    // The Graphs page's JS parses this header by name; the indices below
    // are Timestamp..ReqI, columns()[0..6] (pinned by
    // test_index_first_seven_columns).
    outCSV = "Timestamp,PackV,PackI,SOC,MinCellV,MaxCellV,ReqI\n";

    if (!initialized)
        return ReadResult::Busy;

    static const size_t kFieldIndices[] = {0, 1, 2, 3, 4, 5, 6};
    constexpr size_t kFieldCount = sizeof(kFieldIndices) / sizeof(kFieldIndices[0]);

    targetPoints = std::max<size_t>(targetPoints, 1);
    targetPoints = std::min(targetPoints, kMaxGraphTargetPoints);

    MutexLock lock(sdMutex_, webLockTimeout(sdMutex_, kGraphLockTimeout));
    if (!lock)
        return ReadResult::Busy;

    String path = "/" + fileName;

    // Cheap size check before committing to a two-pass scan; the sample
    // pass below reuses this same handle instead of reopening.
    File file;
    uint32_t sourceSize = 0;
    ReadResult openResult = openBounded(path, kMaxGraphSourceBytes, file, sourceSize);
    if (openResult != ReadResult::Ok)
    {
        outCSV = "";
        return openResult;
    }

    // Estimate the row count from a short sample after the header instead
    // of a full-file scan - a telemetry row is fairly uniform width.
    constexpr uint32_t kSampleBytes = 4096;
    uint32_t dataBytes = 0;
    size_t skip = 1;
    {
        String header = file.readStringUntil('\n');
        uint32_t headerBytes = (uint32_t)header.length() + 1;
        dataBytes = (sourceSize > headerBytes) ? (sourceSize - headerBytes) : 0;

        uint8_t sampleBuf[kSdReadChunkBytes];
        uint32_t sampleBytesRead = 0;
        size_t sampleLines = 0;
        int n;
        while (sampleBytesRead < kSampleBytes &&
               (n = file.read(sampleBuf, std::min<size_t>(kSampleBytes - sampleBytesRead, sizeof(sampleBuf)))) > 0)
        {
            sampleBytesRead += (uint32_t)n;
            for (int i = 0; i < n; i++)
                if (sampleBuf[i] == '\n')
                    sampleLines++;
        }

        skip = CsvDecimation::estimateSkip(dataBytes, sampleBytesRead, sampleLines, targetPoints);
    }
    file.close();

    // Fresh handle, buffered pass: feeds decimated rows straight into outCSV.
    File f = SD.open(path, FILE_READ);
    if (!f)
    {
        outCSV = "";
        return ReadResult::NotFound;
    }

    outCSV.reserve(outCSV.length() + (targetPoints + 1) * 60);
    f.readStringUntil('\n'); // header, discarded

    CsvDecimation::Accumulator accum(kFieldIndices, kFieldCount, skip);
    char outBuf[kGraphOutBufBytes];
    streamChunks(f, dataBytes, [&accum, &outCSV, &outBuf](const uint8_t *data, size_t len)
                 {
        size_t outLen = 0;
        accum.feed(data, len, outBuf, sizeof(outBuf), &outLen);
        if (outLen > 0)
            outCSV.concat(outBuf, (unsigned int)outLen); });

    // A final row with no trailing newline (writer's last flush pending).
    size_t finalLen = 0;
    accum.finish(outBuf, sizeof(outBuf), &finalLen);
    if (finalLen > 0)
        outCSV.concat(outBuf, (unsigned int)finalLen);
    f.close();

    return ReadResult::Ok;
}

std::shared_ptr<MutexLock> SDLogger::beginDownload(const String &fileName, String &outPath)
{
    outPath = "";

    if (!initialized)
        return nullptr;

    auto lock = std::make_shared<MutexLock>(sdMutex_, webLockTimeout(sdMutex_, kDownloadLockTimeout));
    if (!*lock)
        return nullptr;

    outPath = "/" + fileName;
    return lock;
}
