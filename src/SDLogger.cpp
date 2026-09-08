#include "SDLogger.h"
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <utility>
#include "pin_config.h"

bool SDLogger::initialized = false;
QueueHandle_t SDLogger::logQueue = NULL;
SemaphoreHandle_t SDLogger::sdMutex_ = NULL;
SDDebugCallback SDLogger::debugCb = nullptr;

namespace
{
    struct LogMessage
    {
        char type; // 'T' telemetry, 'E' event
        uint8_t cellCount;
        char data[480];
    };

    // This app is always a 16S pack (MAX_CELLS in main.cpp) - keep the CSV
    // header's cell-column count matching what logTelemetry() actually
    // writes, rather than a padded upper bound.
    constexpr uint8_t kMaxLoggedCells = 16;

    // Caps how much of a source file readGraphSeries() will scan, so a very
    // large (e.g. multi-week boot_ fallback) file can't hold sdMutex_ for an
    // unbounded two-pass scan.
    constexpr uint32_t kMaxGraphSourceBytes = 4 * 1024 * 1024;
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

SemaphoreHandle_t SDLogger::sdMutex()
{
    return sdMutex_;
}

void SDLogger::logTelemetry(const DashboardData &data)
{
    if (!initialized)
        return;

    LogMessage msg;
    msg.type = 'T';
    msg.cellCount = (uint8_t)min((size_t)kMaxLoggedCells, data.cellVoltages.size());

    int written = snprintf(msg.data, sizeof(msg.data),
                            "%.2f,%.2f,%.1f,%.3f,%.3f,%.1f,%s,%d,%d,%d",
                            data.packVoltage, data.packCurrent, data.packSOC,
                            data.minCellVoltage, data.maxCellVoltage, data.requestedCurrent,
                            data.smaChargeMode.c_str(),
                            data.forceCharge ? 1 : 0,
                            data.maintenanceActive ? 1 : 0,
                            data.gridPresent ? 1 : 0);

    for (uint8_t i = 0; i < msg.cellCount && written > 0 && written < (int)sizeof(msg.data); i++)
    {
        int n = snprintf(msg.data + written, sizeof(msg.data) - written, ",%.3f", data.cellVoltages[i]);
        if (n < 0)
            break;
        written += n;
    }

    // BMS protection columns appended strictly after the cell columns -
    // readGraphSeries() hardcodes extraction of raw columns 0-6
    // (Timestamp..ReqI), so anything added here doesn't disturb that.
    if (written > 0 && written < (int)sizeof(msg.data))
    {
        int n = snprintf(msg.data + written, sizeof(msg.data) - written, ",%d,%d,%d,%d,%d,%d,%d",
                          data.chargeMosOn ? 1 : 0,
                          data.dischargeMosOn ? 1 : 0,
                          data.bmsProtectionActive ? 1 : 0,
                          data.cellOvervoltLevel1 ? 1 : 0,
                          data.cellOvervoltLevel2 ? 1 : 0,
                          data.packOvervoltLevel1 ? 1 : 0,
                          data.packOvervoltLevel2 ? 1 : 0);
        if (n > 0)
            written += n;
    }

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
    msg.type = 'E';
    msg.cellCount = 0;
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

    file.print("Timestamp,PackV,PackI,SOC,MinCellV,MaxCellV,ReqI,Mode,ForceCharge,MaintenanceActive,GridPresent");
    for (int i = 1; i <= kMaxLoggedCells; i++)
    {
        file.printf(",Cell%d", i);
    }
    file.print(",ChargeMOS,DischargeMOS,BmsProtection,CellOV1,CellOV2,PackOV1,PackOV2");
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

        if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue; // a reader is hogging the bus; drop this line rather than stall forever

        if (msg.type == 'T')
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
        else if (msg.type == 'E')
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

bool SDLogger::listLogFiles(std::vector<String> &outNames, std::vector<uint32_t> &outSizes)
{
    outNames.clear();
    outSizes.clear();

    if (!initialized)
        return false;

    if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(500)) != pdTRUE)
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
                {
                    outNames.push_back(name);
                    outSizes.push_back((uint32_t)file.size());
                }
            }
            file = root.openNextFile();
        }
        root.close();
    }

    xSemaphoreGive(sdMutex_);

    // Bubble sort is fine here: at most a few dozen daily files. boot_*
    // fallback files (written pre-NTP-sync) always sort before dated
    // YYYY-MM-DD files regardless of their boot ID, since a plain string
    // compare would otherwise put "boot_..." AFTER any digit-starting name
    // ('b' > '0'-'9') - which would wrongly rank a stale boot_ file as more
    // recent than a properly dated one and break "select most recent = last".
    auto isOlderName = [](const String &a, const String &b)
    {
        bool aBoot = a.startsWith("boot_");
        bool bBoot = b.startsWith("boot_");
        if (aBoot != bBoot)
            return aBoot; // boot_* always sorts first
        return a < b;
    };
    for (size_t i = 0; i < outNames.size(); i++)
    {
        for (size_t j = i + 1; j < outNames.size(); j++)
        {
            if (isOlderName(outNames[j], outNames[i]))
            {
                std::swap(outNames[i], outNames[j]);
                std::swap(outSizes[i], outSizes[j]);
            }
        }
    }

    return true;
}

bool SDLogger::readTail(const String &fileName, String &outContent, size_t maxBytes)
{
    outContent = "";

    if (!initialized)
        return false;

    if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(500)) != pdTRUE)
        return false;

    bool ok = false;
    File file = SD.open("/" + fileName, FILE_READ);
    if (file)
    {
        size_t size = file.size();
        if (size > maxBytes)
        {
            file.seek(size - maxBytes);
            // Skip the (likely truncated) first line so content starts cleanly.
            file.readStringUntil('\n');
        }

        outContent.reserve(min(size, maxBytes) + 1);
        uint8_t buf[512];
        int n;
        while ((n = file.read(buf, sizeof(buf))) > 0)
        {
            outContent.concat((const char *)buf, n);
        }
        file.close();
        ok = true;
    }

    xSemaphoreGive(sdMutex_);
    return ok;
}

namespace
{
    // Returns the idx'th comma-separated field of line (0-based), or "" past the end.
    String csvField(const String &line, int idx)
    {
        int start = 0;
        for (int i = 0; i < idx; i++)
        {
            int comma = line.indexOf(',', start);
            if (comma < 0)
                return "";
            start = comma + 1;
        }
        int comma = line.indexOf(',', start);
        return comma < 0 ? line.substring(start) : line.substring(start, comma);
    }
}

bool SDLogger::readGraphSeries(const String &fileName, size_t targetPoints, String &outCSV)
{
    outCSV = "Timestamp,PackV,PackI,SOC,MinCellV,MaxCellV,ReqI\n";

    if (!initialized)
        return false;

    if (targetPoints == 0)
        targetPoints = 1;
    if (targetPoints > 2000)
        targetPoints = 2000; // keep worst-case output bounded regardless of caller

    if (xSemaphoreTake(sdMutex_, pdMS_TO_TICKS(2000)) != pdTRUE)
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

    // Pass 1: count data rows (total newlines, minus the header line) so we
    // can pick a skip interval - a plain byte scan, no line objects allocated.
    size_t totalLines = 0;
    {
        File f = SD.open(path, FILE_READ);
        if (f)
        {
            uint8_t buf[512];
            int n;
            while ((n = f.read(buf, sizeof(buf))) > 0)
            {
                for (int i = 0; i < n; i++)
                {
                    if (buf[i] == '\n')
                        totalLines++;
                }
            }
            f.close();
            if (totalLines > 0)
                totalLines--; // header line
            ok = true;
        }
    }

    // Pass 2: re-read, keeping every Nth data row, extracting only the
    // columns needed for graphing.
    if (ok)
    {
        size_t skip = (totalLines > targetPoints) ? (totalLines / targetPoints) : 1;

        File f = SD.open(path, FILE_READ);
        if (f)
        {
            outCSV.reserve(outCSV.length() + (targetPoints + 1) * 60);
            f.readStringUntil('\n'); // header

            // Bounded by totalLines (from pass 1, over the same mutex-held,
            // now-immutable file content) as well as f.available() - if
            // readStringUntil() ever returns without actually advancing the
            // file position (an EOF/empty-line edge case observed in
            // practice), f.available() can stay truthy forever and this
            // would otherwise spin forever while holding sdMutex_, wedging
            // every other SD route behind it.
            size_t lineIdx = 0;
            while (f.available() && lineIdx <= totalLines)
            {
                String line = f.readStringUntil('\n');
                if (line.length() == 0)
                {
                    lineIdx++;
                    continue;
                }

                if (lineIdx % skip == 0)
                {
                    outCSV += csvField(line, 0) + "," + csvField(line, 1) + "," +
                              csvField(line, 2) + "," + csvField(line, 3) + "," +
                              csvField(line, 4) + "," + csvField(line, 5) + "," +
                              csvField(line, 6) + "\n";
                }
                lineIdx++;
            }
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
