#include "SDLogger.h"
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <utility>
#include "pin_config.h"

bool SDLogger::initialized = false;
QueueHandle_t SDLogger::logQueue = NULL;
SemaphoreHandle_t SDLogger::sdMutex_ = NULL;

namespace
{
    struct LogMessage
    {
        char type; // 'T' telemetry, 'E' event
        uint8_t cellCount;
        char data[480];
    };

    constexpr uint8_t kMaxLoggedCells = 32;
}

bool SDLogger::isReady()
{
    return initialized;
}

bool SDLogger::begin()
{
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS))
    {
        Serial.println("[SD] Card mount failed");
        return false;
    }

    if (SD.cardType() == CARD_NONE)
    {
        Serial.println("[SD] No SD card attached");
        return false;
    }

    logQueue = xQueueCreate(32, sizeof(LogMessage));
    if (logQueue == NULL)
    {
        Serial.println("[SD] Failed to create log queue");
        return false;
    }

    sdMutex_ = xSemaphoreCreateMutex();
    if (sdMutex_ == NULL)
    {
        Serial.println("[SD] Failed to create SD mutex");
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

    // Bubble sort is fine here: at most a few dozen daily files.
    for (size_t i = 0; i < outNames.size(); i++)
    {
        for (size_t j = i + 1; j < outNames.size(); j++)
        {
            if (outNames[j] < outNames[i])
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
