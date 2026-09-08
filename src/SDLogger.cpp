#include "SDLogger.h"
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include "pin_config.h"

bool SDLogger::initialized = false;
QueueHandle_t SDLogger::logQueue = NULL;

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

    xTaskCreatePinnedToCore(loggingTask, "SD_LogTask", 8192, NULL, 1, NULL, 0);

    initialized = true;
    return true;
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
    }
}
