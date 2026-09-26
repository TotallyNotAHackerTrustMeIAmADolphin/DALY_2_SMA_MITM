#include "SDLogger.h"
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <utility>
#include <algorithm>
#include "pin_config.h"
#include "esp_task_wdt.h"
#include "TelemetrySchema.h"
#include "CsvDecimation.h"
#include "TailTrim.h"
#include "LogFileOrder.h"

bool SDLogger::initialized = false;
QueueHandle_t SDLogger::logQueue = NULL;
SemaphoreHandle_t SDLogger::sdMutex_ = NULL;
SDDebugCallback SDLogger::debugCb = nullptr;

namespace
{
    enum class MsgType : char
    {
        Telemetry = 'T',
        Event = 'E',
    };

    struct LogMessage
    {
        MsgType type;
        char data[SdTuning::kMsgDataBytes];
    };

    // CsvDecimation::kLineBufSize (320) only needs to cover the fields
    // readGraphSeries() actually extracts (Timestamp..ReqI, the first
    // seven TelemetrySchema columns) - a full LogMessage::data row can run
    // longer (up to kMsgDataBytes) once the later columns (Cell1..16 etc.)
    // are appended, and CsvDecimation::Accumulator safely truncates a line
    // past its line buffer for field EXTRACTION only (see its own comment)
    // while still scanning the rest of the bytes for the terminating '\n'.
    // So this assumes kLineBufSize is enough to reach past ReqI (field
    // index 6) into the row, which is true today by a wide margin (that
    // prefix is well under 100 bytes); it is NOT required to reach the end
    // of a full kMsgDataBytes row.
    static_assert(CsvDecimation::kLineBufSize < SdTuning::kMsgDataBytes,
                  "CsvDecimation's line buffer is expected to be smaller than a "
                  "full telemetry row - readGraphSeries only reads the first few "
                  "columns, so this is fine, but re-check the comment above if "
                  "this assumption ever needs to flip");

    // Caps how much of a source file readGraphSeries() will scan, so a very
    // large (e.g. multi-week boot_ fallback) file can't hold sdMutex_ for an
    // unbounded two-pass scan.
    constexpr uint32_t kMaxGraphSourceBytes = 4 * 1024 * 1024;

    // Same idea for readTail(), but much tighter: unlike readGraphSeries
    // (which only needs to touch the card twice, briefly, per request),
    // readTail can't seek to near the end (file.seek() on a file reopened
    // for FILE_APPEND hundreds of times across reboots was observed to make
    // the following read() return 0 bytes - see readTail()'s comment), so
    // it must scan sequentially from byte 0 for the whole time it holds
    // sdMutex_. Bounding that scan to kMaxGraphSourceBytes (4MB) would let a
    // single request hold the mutex far longer than the SD writer task's
    // own 1s mutex-acquire timeout, dropping telemetry samples for the
    // whole scan. 512KB comfortably covers a full day's telemetry CSV
    // (observed ~400KB/day in practice) while keeping the worst case small;
    // beyond that, readTail() fails cleanly (like readGraphSeries does for
    // oversized files) rather than silently returning a stale tail.
    constexpr uint32_t kMaxTailSourceBytes = 512 * 1024;

    // readGraphSeries()'s own output-size cap, independent of whatever the
    // caller asks for.
    constexpr size_t kMaxGraphTargetPoints = 2000;

    // readGraphSeries()'s decimated-output chunk buffer: generous margin
    // over the largest single kSdReadChunkBytes-byte chunk's worth of
    // decimated output.
    constexpr size_t kGraphOutBufBytes = 2048;

    // Both readTail() and readGraphSeries() read the source file in chunks
    // this size. readTail() passes it to TailTrim::Trimmer's constructor
    // explicitly so its reserved slack always matches the real chunk size
    // (see the Trimmer constructor's own comment) - changing this constant
    // is now enough on its own, no matching edit needed in TailTrim.h.
    constexpr size_t kSdReadChunkBytes = 512;
}

bool SDLogger::isReady()
{
    return initialized;
}

void SDLogger::setDebugCallback(SDDebugCallback cb)
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

    logQueue = xQueueCreate(SdTuning::kQueueDepth, sizeof(LogMessage));
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

    xTaskCreatePinnedToCore(loggingTask, "SD_LogTask", SdTuning::kWriterTaskStackBytes, NULL,
                            SdTuning::kWriterTaskPriority, NULL, SdTuning::kWriterTaskCore);

    initialized = true;
    return true;
}

SemaphoreHandle_t SDLogger::sdMutex()
{
    return sdMutex_;
}

void SDLogger::logTelemetry(const DashboardData &data)
{
    if (!initialized)
        return;

    LogMessage msg;
    msg.type = MsgType::Telemetry;

    // Column order, formatting and the "omit a not-yet-read cell slot"
    // behavior all live in TelemetrySchema::formatRow() now (#32) - this
    // reproduces the pre-refactor row byte for byte (see its own comment).
    TelemetrySchema::formatRow(data, msg.data, sizeof(msg.data));

    // Queue is sized generously for the ~1 sample/10s telemetry rate; if a
    // write is genuinely stuck (e.g. card removed mid-session) we drop the
    // sample rather than block the caller.
    xQueueSend(logQueue, &msg, 0);
}

void SDLogger::logEvent(const char *msg_text)
{
    if (!initialized)
        return;

    LogMessage msg;
    msg.type = MsgType::Event;
    strncpy(msg.data, msg_text, sizeof(msg.data) - 1);
    msg.data[sizeof(msg.data) - 1] = '\0';

    xQueueSend(logQueue, &msg, 0);
}

String SDLogger::currentLogPath(const char *extension)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char fileName[32];
    if (timeinfo.tm_year > 70)
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
        return;

    // Column names/order come from TelemetrySchema::kColumns (#32), the
    // same table logTelemetry() formats each row from - this reproduces
    // the pre-refactor header string byte for byte.
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

        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        char timeStr[32];
        if (timeinfo.tm_year > 70)
        {
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        }
        else
        {
            snprintf(timeStr, sizeof(timeStr), "UP:%lu", millis() / 1000);
        }

        if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(SdTuning::kWriterLockTimeoutMs)) != pdTRUE)
            continue; // a reader is hogging the bus; drop this line rather than stall forever

        if (msg.type == MsgType::Telemetry)
        {
            String path = currentLogPath(".csv");
            writeCSVHeaderIfMissing(path);
            File file = SD.open(path, FILE_APPEND);
            if (file)
            {
                file.printf("%s,%s\n", timeStr, msg.data);
                file.close();
            }
        }
        else if (msg.type == MsgType::Event)
        {
            File file = SD.open(currentLogPath(".log"), FILE_APPEND);
            if (file)
            {
                file.printf("[%s] %s", timeStr, msg.data);
                file.close();
            }
        }

        xSemaphoreGive(sdMutex_);
    }
}

bool SDLogger::listLogFiles(std::vector<LogFileInfo> &outFiles)
{
    outFiles.clear();

    if (!initialized)
        return false;

    if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(SdTuning::kListLockTimeoutMs)) != pdTRUE)
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

    // The ordering rule itself (boot_* fallback files always sort first,
    // "select most recent = last") lives in LogFileOrder::isOlder() (#97),
    // pure and natively tested - a handful of files at most, so std::sort
    // over the untested hand-rolled bubble sort this replaced costs nothing.
    std::sort(outFiles.begin(), outFiles.end(),
              [](const LogFileInfo &a, const LogFileInfo &b)
              { return LogFileOrder::isOlder(a.name.c_str(), b.name.c_str()); });

    return true;
}

bool SDLogger::readTail(const String &fileName, String &outContent, size_t maxBytes)
{
    outContent = "";

    if (!initialized)
        return false;

    if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(SdTuning::kTailLockTimeoutMs)) != pdTRUE)
        return false;

    bool ok = false;

    // Cheap size-check first (matches readGraphSeries' pattern) - readTail
    // can't seek to near the end (see below), so it must scan sequentially
    // from byte 0 for its whole sdMutex_ hold. Rejecting outsized files
    // here keeps that hold bounded to kMaxTailSourceBytes worst-case,
    // instead of silently scanning (and holding the mutex for) however
    // large the file has grown.
    File sizeCheck = SD.open("/" + fileName, FILE_READ);
    bool opened = (bool)sizeCheck;
    uint32_t sourceSize = opened ? sizeCheck.size() : 0;
    if (opened)
        sizeCheck.close();
    if (!opened || sourceSize > kMaxTailSourceBytes)
    {
        xSemaphoreGive(sdMutex_);
        return false;
    }

    File file = SD.open("/" + fileName, FILE_READ);
    if (file)
    {
        // Read sequentially from the start rather than file.seek()-ing near
        // the end - live-tested against a file that's been reopened for
        // FILE_APPEND hundreds of times across many reboots, seek() to an
        // arbitrary offset near EOF consistently made the following read()
        // return 0 bytes immediately (empty tail, silently "successful").
        // Sequential reads from 0 are the one access pattern proven
        // reliable everywhere else in this file (readGraphSeries() below,
        // and the /api/logs/download route), so use that here too. The
        // rolling trim-to-last-maxBytes and leading-partial-line-drop logic
        // itself now lives in TailTrim::Trimmer (#43), pure and natively
        // tested - this loop is just SD I/O and the watchdog yield cadence.
        TailTrim::Trimmer trimmer(maxBytes, kSdReadChunkBytes);
        uint8_t buf[kSdReadChunkBytes];
        int n;
        uint32_t bytesScanned = 0;
        uint32_t chunkCount = 0;
        while (bytesScanned < sourceSize && (n = file.read(buf, sizeof(buf))) > 0)
        {
            trimmer.feed(buf, (size_t)n);
            bytesScanned += (uint32_t)n;
            if (++chunkCount % 8 == 0)
                { esp_task_wdt_reset(); vTaskDelay(1); }
        }
        trimmer.finish();
        outContent = String(trimmer.data(), (unsigned int)trimmer.length());
        file.close();
        ok = true;
    }

    xSemaphoreGive(sdMutex_);
    return ok;
}

bool SDLogger::readGraphSeries(const String &fileName, size_t targetPoints, String &outCSV)
{
    outCSV = "Timestamp,PackV,PackI,SOC,MinCellV,MaxCellV,ReqI\n";

    if (!initialized)
        return false;

    // Column positions in a full telemetry row, looked up by name once
    // (#32) instead of hardcoding 0-6 - these happen to still be 0-6 today
    // since Timestamp..ReqI are the first seven TelemetrySchema columns,
    // but a source row is read by position here regardless of any later
    // reordering upstream in the table. Handed to CsvDecimation::Accumulator
    // below, which never interprets them itself (#43).
    const size_t fieldIndices[7] = {
        (size_t)TelemetrySchema::index("Timestamp"),
        (size_t)TelemetrySchema::index("PackV"),
        (size_t)TelemetrySchema::index("PackI"),
        (size_t)TelemetrySchema::index("SOC"),
        (size_t)TelemetrySchema::index("MinCellV"),
        (size_t)TelemetrySchema::index("MaxCellV"),
        (size_t)TelemetrySchema::index("ReqI"),
    };

    if (targetPoints == 0)
        targetPoints = 1;
    if (targetPoints > kMaxGraphTargetPoints)
        targetPoints = kMaxGraphTargetPoints; // keep worst-case output bounded regardless of caller

    if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(SdTuning::kGraphLockTimeoutMs)) != pdTRUE)
        return false;

    String path = "/" + fileName;
    bool ok = false;

    // Cheap check (just a directory-entry size, no read) before committing
    // to a two-pass scan of the whole file - bounds the mutex hold time
    // regardless of how large the source file has grown.
    File sizeCheck = SD.open(path, FILE_READ);
    bool opened = (bool)sizeCheck;
    uint32_t sourceSize = opened ? sizeCheck.size() : 0;
    if (opened)
        sizeCheck.close();
    if (!opened || sourceSize > kMaxGraphSourceBytes)
    {
        xSemaphoreGive(sdMutex_);
        outCSV = "";
        return false;
    }

    // Estimate the data-row count from a small sample instead of a full-file
    // scan (#40 - this used to be "pass 1", an exact-count byte scan of the
    // *whole* file just to pick a skip interval). targetPoints tops out at
    // 2000 and a telemetry row is a fairly uniform width, so an estimate
    // from a short sample right after the header gives the same skip
    // interval in practice, without the full pass. Re-opens the file (this
    // route already accepted multiple short opens for the same reason the
    // size-check above does) rather than seeking, matching the existing
    // pattern in this function and avoiding any seek-after-partial-read
    // question entirely.
    constexpr uint32_t kSampleBytes = 4096;
    size_t estimatedLines = 1;
    uint32_t dataBytes = 0;
    {
        File f = SD.open(path, FILE_READ);
        if (!f)
        {
            xSemaphoreGive(sdMutex_);
            outCSV = "";
            return false;
        }
        String header = f.readStringUntil('\n');
        uint32_t headerBytes = (uint32_t)header.length() + 1; // + the '\n' it consumed
        dataBytes = (sourceSize > headerBytes) ? (sourceSize - headerBytes) : 0;

        uint8_t sampleBuf[512];
        uint32_t sampleBytesRead = 0;
        size_t sampleLines = 0;
        int n;
        while (sampleBytesRead < kSampleBytes &&
               (n = f.read(sampleBuf, (size_t)((kSampleBytes - sampleBytesRead) < sizeof(sampleBuf) ? (kSampleBytes - sampleBytesRead) : sizeof(sampleBuf)))) > 0)
        {
            sampleBytesRead += (uint32_t)n;
            for (int i = 0; i < n; i++)
                if (sampleBuf[i] == '\n')
                    sampleLines++;
        }
        f.close();

        // sampleLines==0 means the sample didn't even contain one full row
        // (a huge single line, or a file barely bigger than its header) -
        // estimatedLines stays at its 1 default rather than dividing by
        // zero, which makes skip below come out to 1 (keep every row).
        if (sampleLines > 0 && sampleBytesRead > 0)
        {
            float avgLineLen = (float)sampleBytesRead / (float)sampleLines;
            estimatedLines = (size_t)((float)dataBytes / avgLineLen);
            if (estimatedLines == 0)
                estimatedLines = 1;
        }
        ok = true;
    }

    // Single buffered pass: re-open and read the whole file in 512-byte
    // chunks (like the old pass 1's byte scan - fast block reads, not the
    // old pass 2's one-Stream-call-per-line readStringUntil(), which was
    // the dominant cost: ~30s measured on a 1.7MB/~24k-row day on-device,
    // see #40). Line-accumulation and skip-decimation themselves now live
    // in CsvDecimation::Accumulator (#43), pure and natively tested - this
    // loop is just SD I/O, feeding it chunks and appending whatever it
    // wrote for that chunk onto outCSV, plus the watchdog yield cadence.
    if (ok)
    {
        size_t skip = (estimatedLines > targetPoints) ? (estimatedLines / targetPoints) : 1;
        if (skip == 0)
            skip = 1;

        File f = SD.open(path, FILE_READ);
        if (f)
        {
            outCSV.reserve(outCSV.length() + (targetPoints + 1) * 60);
            f.readStringUntil('\n'); // header, discarded (fresh open, same as the sample pass above)

            CsvDecimation::Accumulator accum(fieldIndices, 7, skip);
            uint8_t buf[kSdReadChunkBytes];
            char outBuf[kGraphOutBufBytes]; // generous margin over the largest single chunk's worth of decimated output
            uint32_t bytesRead = 0;
            uint32_t chunkCount = 0;
            int n;

            while (bytesRead < dataBytes && (n = f.read(buf, sizeof(buf))) > 0)
            {
                bytesRead += (uint32_t)n;
                size_t outLen = 0;
                accum.feed(buf, (size_t)n, outBuf, sizeof(outBuf), &outLen);
                if (outLen > 0)
                    outCSV.concat(outBuf, (unsigned int)outLen);
                // Same watchdog reasoning as the old pass 1 (this route runs
                // synchronously on the AsyncTCP task; a hung/slow scan here
                // previously starved its watchdog feed and crashed the
                // device - `task_wdt: ... async_tcp` -> abort() -> reboot).
                if (++chunkCount % 8 == 0)
                    { esp_task_wdt_reset(); vTaskDelay(1); }
            }
            // A final row without a trailing newline (e.g. the writer
            // task's last flush hadn't landed yet) - flush it too, same as
            // the old f.available()-driven loop did.
            size_t finalLen = 0;
            accum.finish(outBuf, sizeof(outBuf), &finalLen);
            if (finalLen > 0)
                outCSV.concat(outBuf, (unsigned int)finalLen);
            f.close();
        }
        else
        {
            ok = false;
        }
    }

    xSemaphoreGive(sdMutex_);
    return ok;
}
