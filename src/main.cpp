#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>
#include <time.h>
#include "esp_system.h"
#include "esp_core_dump.h"
#include "esp_ota_ops.h"
#include "rom/rtc.h"

#include "pin_config.h"
#include "SystemState.h"
#include "Glideslope.h"
#include "DalyRS485.h"
#include "SMA_CAN.h"
#include "WebDashboard.h"
#include "SDLogger.h"

// Bring in your Wi-Fi credentials AND network config (static IP, gateway,
// subnet, DNS) - all of it lives in this one gitignored file now, so a
// public checkout never reveals your home network layout. See
// secrets_example.h for the template.
#include "secrets.h"

#define MAX_CELLS 16
#define MAX_SAMPLES 20

// --- GLOBAL INSTANCES ---
DalyRS485 bms(Serial2);
SMA_CAN inverter;
WebDashboard webUI(80);

SystemConfig cfg;
DashboardData currentData;
SemaphoreHandle_t dataMutex;

// Serializes netLog()'s network sinks (TelnetStream, SSE log channel) and
// the SSE telemetry push. TelnetStream is not safe to call from several
// tasks at once (its write() also accepts new clients), and netLog runs
// from bmsTask, canTask, loop() and the web server's task. AsyncEventSource
// locks internally since ESPAsyncWebServer 3.x; keeping it behind the same
// mutex also stops log lines from interleaving. Innermost lock: never take
// another one while holding it.
SemaphoreHandle_t netOutMutex;
// Set once WiFi, Telnet and the web server are up. Before that, netLog()
// only writes to Serial and the SD card.
volatile bool netReady = false;

TaskHandle_t bmsTaskHandle = NULL;
TaskHandle_t canTaskHandle = NULL;

// Global filter buffers (bmsTask only)
uint16_t cellBuffers[MAX_CELLS][MAX_SAMPLES] = {0};
int bufferIndex = 0;

// Written by handleUIAction() on the web server's task, read by canTask.
volatile unsigned long resetHoldStartTime = 0;
volatile bool manualMaintForce = false;
volatile bool isResetting = false;
bool autoMaint = false; // canTask only

// BMS data freshness - guarded by dataMutex. Nothing is sent to the SMA
// until both have succeeded once (see canTask), and the glideslope treats
// "never read" as stale (Glideslope::isFresh).
bool haveBasicInfo = false;
bool haveCellData = false;
unsigned long lastBasicInfoRead = 0;
unsigned long lastCellRead = 0;

// --- CENTRAL LOGGING ---
void netLog(const char *format, ...)
{
  char loc_res[256];
  va_list arg;
  va_start(arg, format);
  vsnprintf(loc_res, sizeof(loc_res), format, arg);
  va_end(arg);

  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  char final_res[350];
  if (timeinfo.tm_year > 70)
  {
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "[%d.%m.%Y %H:%M:%S] ", &timeinfo);
    snprintf(final_res, sizeof(final_res), "%s%s", timeStr, loc_res);
  }
  else
  {
    snprintf(final_res, sizeof(final_res), "[WAITING FOR NTP...] %s", loc_res);
  }

  Serial.print(final_res);
  if (netReady && xSemaphoreTake(netOutMutex, pdMS_TO_TICKS(50)) == pdTRUE)
  {
    if (TelnetStream.availableForWrite() > 0) {
      TelnetStream.print(final_res);
    }
    webUI.broadcastLog(final_res);
    xSemaphoreGive(netOutMutex);
  }
  SDLogger::logEvent(loc_res);
}

void libraryLogger(const char *msg) { netLog("%s", msg); }

// SSE telemetry push, serialized with netLog's network sinks (netOutMutex).
void pushTelemetry(const DashboardData &data)
{
  if (!netReady)
    return;
  if (xSemaphoreTake(netOutMutex, pdMS_TO_TICKS(50)) == pdTRUE)
  {
    webUI.broadcastTelemetry(data);
    xSemaphoreGive(netOutMutex);
  }
}

// --- GLIDESLOPE LOGIC ---
// The math lives in include/Glideslope.h (natively unit-tested); these
// wrappers feed in live state. Caller must hold dataMutex.

// Both BMS reads must have succeeded at least once and be within
// cfg.bmsTimeout - the limits depend on the cell voltages, so fresh basic
// info alone must not keep them alive.
bool bmsDataFresh()
{
  unsigned long now = millis();
  return Glideslope::isFresh(haveBasicInfo, now, lastBasicInfoRead, cfg.bmsTimeout) &&
         Glideslope::isFresh(haveCellData, now, lastCellRead, cfg.bmsTimeout);
}

uint16_t calculateCCL(float maxCellV)
{
  return Glideslope::calculateCCL(cfg, maxCellV, bmsDataFresh(), currentData.maintenanceActive);
}

uint16_t calculateDCL(float minCellV)
{
  return Glideslope::calculateDCL(cfg, minCellV, bmsDataFresh(), currentData.maintenanceActive);
}

// --- UI EVENT HANDLER ---
void handleUIAction(const char *action)
{
  if (strcmp(action, "toggleMaint") == 0)
  {
    manualMaintForce = !manualMaintForce;
    netLog("[USER] Manual Force Charge: %s\n", manualMaintForce ? "ON" : "OFF");
  }
  else if (strcmp(action, "resetSMA") == 0)
  {
    isResetting = true;
    resetHoldStartTime = millis();
    netLog("[USER] Manual Cluster Reset Triggered.\n");
  }
  else if (strcmp(action, "configSaved") == 0)
  {
    netLog("[CONFIG] Settings updated and saved to NVS.\n");
  }
}

// --- CORE 0: BMS BACKGROUND TASK ---
void bmsTask(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(2000));

  while (true)
  {
    DalyBasicInfo info;
    if (bms.readBasicInfo(info))
    {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        currentData.packVoltage = info.packVoltage;
        currentData.packCurrent = info.packCurrent;
        currentData.packSOC = info.packSOC;
        lastBasicInfoRead = millis();
        haveBasicInfo = true;
        xSemaphoreGive(dataMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    std::vector<float> cellVolts;
    if (bms.readCellVoltages(MAX_CELLS, cellVolts))
    {
      float sum = 0;
      float localMin = 10.0f;
      float localMax = 0.0f;
      std::vector<float> smoothedCellVolts;
      smoothedCellVolts.reserve(MAX_CELLS);

      static int lastKnownVSamples = 12; // falls back to this if the lock is briefly contended
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        lastKnownVSamples = cfg.vSamples;
        xSemaphoreGive(dataMutex);
      }
      int windowSize = max(1, min(MAX_SAMPLES, lastKnownVSamples));

      // Fill the whole moving-average window from the first real reading,
      // so the filter starts at the actual cell voltages. (It used to be
      // pre-filled with cvMaxCharge, which made the smoothed max start near
      // the hard limit and swung the CCL 500A -> trickle -> 500A after
      // every boot.)
      static bool filterSeeded = false;
      if (!filterSeeded)
      {
        for (int i = 0; i < (int)cellVolts.size() && i < MAX_CELLS; i++)
        {
          uint16_t mv = (uint16_t)(cellVolts[i] * 1000.0f);
          for (int j = 0; j < MAX_SAMPLES; j++)
            cellBuffers[i][j] = mv;
        }
        filterSeeded = true;
      }

      for (int i = 0; i < (int)cellVolts.size() && i < MAX_CELLS; i++)
      {
        // Update per-cell buffer
        cellBuffers[i][bufferIndex] = (uint16_t)(cellVolts[i] * 1000.0f);
        
        // Calculate smoothed average for this cell
        uint32_t cellSumMV = 0;
        for (int j = 0; j < windowSize; j++) {
          cellSumMV += cellBuffers[i][j];
        }
        
        float smoothedV = (float)(cellSumMV / windowSize) / 1000.0f;
        smoothedCellVolts.push_back(smoothedV);
        
        sum += smoothedV;
        if (smoothedV < localMin) localMin = smoothedV;
        if (smoothedV > localMax) localMax = smoothedV;
      }

      DashboardData broadcastCopy;
      bool shouldBroadcast = false;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentData.avgCellVoltage = sum / MAX_CELLS;
        currentData.minCellVoltage = localMin;
        currentData.maxCellVoltage = localMax;
        currentData.cellVoltages = smoothedCellVolts;
        lastCellRead = millis();
        haveCellData = true;

        // Increment circular buffer index AFTER processing all cells
        bufferIndex = (bufferIndex + 1) % windowSize;

        // Copy for broadcast outside mutex
        broadcastCopy = currentData;
        shouldBroadcast = true;
        
        xSemaphoreGive(dataMutex);
      }

      if (shouldBroadcast) {
        pushTelemetry(broadcastCopy);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    // BMS's own hardware protection state - polled at the same 2s cadence
    // as the rest of this loop. Edge-triggered logging only (not every
    // poll) so a stuck-on alarm doesn't spam the log queue; this is what
    // lets a future SMA "battery voltage out of range" fault be lined up
    // against the BMS's own MOSFET/alarm timeline to the second.
    static bool lastChargeMosOn = true;
    static bool lastDischargeMosOn = true;
    static bool lastCellOV1 = false, lastCellOV2 = false;
    static bool lastPackOV1 = false, lastPackOV2 = false;
    static bool haveMosfetBaseline = false;
    static bool haveAlarmBaseline = false;

    DalyMosfetStatus mosStatus;
    if (bms.readMosfetStatus(mosStatus))
    {
      if (haveMosfetBaseline)
      {
        if (mosStatus.chargeMosOn != lastChargeMosOn)
          netLog("[BMS] Charge MOSFET %s\n", mosStatus.chargeMosOn ? "ON" : "OFF - protection or BMS-initiated cutoff");
        if (mosStatus.dischargeMosOn != lastDischargeMosOn)
          netLog("[BMS] Discharge MOSFET %s\n", mosStatus.dischargeMosOn ? "ON" : "OFF - protection or BMS-initiated cutoff");
      }
      lastChargeMosOn = mosStatus.chargeMosOn;
      lastDischargeMosOn = mosStatus.dischargeMosOn;
      haveMosfetBaseline = true;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        currentData.chargeMosOn = mosStatus.chargeMosOn;
        currentData.dischargeMosOn = mosStatus.dischargeMosOn;
        xSemaphoreGive(dataMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    DalyAlarmStatus alarmStatus;
    if (bms.readAlarmStatus(alarmStatus))
    {
      if (haveAlarmBaseline)
      {
        if (alarmStatus.cellOvervoltLevel1 != lastCellOV1)
          netLog("[BMS] Alarm: Cell overvoltage Level 1 %s\n", alarmStatus.cellOvervoltLevel1 ? "SET" : "CLEARED");
        if (alarmStatus.cellOvervoltLevel2 != lastCellOV2)
          netLog("[BMS] Alarm: Cell overvoltage Level 2 %s\n", alarmStatus.cellOvervoltLevel2 ? "SET" : "CLEARED");
        if (alarmStatus.packOvervoltLevel1 != lastPackOV1)
          netLog("[BMS] Alarm: Pack overvoltage Level 1 %s\n", alarmStatus.packOvervoltLevel1 ? "SET" : "CLEARED");
        if (alarmStatus.packOvervoltLevel2 != lastPackOV2)
          netLog("[BMS] Alarm: Pack overvoltage Level 2 %s\n", alarmStatus.packOvervoltLevel2 ? "SET" : "CLEARED");
      }
      lastCellOV1 = alarmStatus.cellOvervoltLevel1;
      lastCellOV2 = alarmStatus.cellOvervoltLevel2;
      lastPackOV1 = alarmStatus.packOvervoltLevel1;
      lastPackOV2 = alarmStatus.packOvervoltLevel2;
      haveAlarmBaseline = true;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        currentData.bmsProtectionActive = alarmStatus.anyProtectionActive;
        currentData.cellOvervoltLevel1 = alarmStatus.cellOvervoltLevel1;
        currentData.cellOvervoltLevel2 = alarmStatus.cellOvervoltLevel2;
        currentData.packOvervoltLevel1 = alarmStatus.packOvervoltLevel1;
        currentData.packOvervoltLevel2 = alarmStatus.packOvervoltLevel2;
        xSemaphoreGive(dataMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void setupNetwork()
{
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS))
  {
    Serial.println("STA Failed to configure");
  }
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi Connected!");
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "ptbtime1.ptb.de");
    
    // Wait for NTP sync (up to 5 seconds)
    Serial.print("Waiting for NTP");
    startAttempt = millis();
    while (time(nullptr) < 1000000000L && millis() - startAttempt < 5000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println(time(nullptr) > 1000000000L ? " OK" : " Timeout");
  }
  else
  {
    Serial.println("\nWiFi Connection Failed. Continuing in Offline Mode...");
  }
}

// --- CORE 1: SMA CAN TASK ---
// Runs in its own task rather than loop(), so CAN starts before setup()'s
// WiFi/NTP wait (up to ~15s) and isn't held up by OTA handling in loop().
void canTask(void *pvParameters)
{
  unsigned long lastCanCheck = 0;
  unsigned long lastSmaTx = 0;
  bool framesEnabled = false;

  while (true)
  {
    unsigned long now = millis();

    if (now - lastCanCheck > 50)
    {
      lastCanCheck = now;
      inverter.checkBusHealth();
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        inverter.readMessages(currentData);
        xSemaphoreGive(dataMutex);
      }
    }

    if (isResetting && (now - resetHoldStartTime > 5500))
    {
      isResetting = false;
      netLog("[SYS] Recovery cycle finished.\n");
    }

    if (now - lastSmaTx > 250)
    {
      lastSmaTx = now;
      bool firstFrames = false;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        // Send nothing until the BMS has delivered basic info AND cell
        // voltages once. Before that currentData holds no real values, and
        // the SMA already rides through a few seconds of CAN silence on
        // every reboot - better than made-up SOC/voltage/limits.
        if (haveBasicInfo && haveCellData)
        {
          if (!autoMaint && currentData.packVoltage > 0 && currentData.packVoltage < (cfg.cvMaintStart * MAX_CELLS))
            autoMaint = true;
          else if (autoMaint && currentData.packVoltage > (cfg.cvMaintStop * MAX_CELLS))
            autoMaint = false;

          currentData.maintenanceActive = manualMaintForce || autoMaint;
          currentData.forceCharge = currentData.maintenanceActive;
          currentData.isResetting = isResetting;

          SMATxData tx;
          tx.packVoltage = currentData.packVoltage;
          tx.packCurrent = currentData.packCurrent;
          tx.packTemp = currentData.packTemp;
          tx.packSOC = currentData.packSOC;
          tx.maintenanceActive = currentData.maintenanceActive;
          tx.isResetting = currentData.isResetting;

          tx.ccl = calculateCCL(currentData.maxCellVoltage);
          currentData.requestedCurrent = tx.ccl / 10.0f;
          tx.dcl = calculateDCL(currentData.minCellVoltage);
          tx.cvl = currentData.maintenanceActive ? 560 : (uint16_t)(cfg.cvMaxCharge * MAX_CELLS * 10);
          tx.dvl = (uint16_t)(cfg.cvMinDischarge * MAX_CELLS * 10);

          inverter.sendStatus(tx);

          if (!framesEnabled)
          {
            framesEnabled = true;
            firstFrames = true;
          }
        }
        xSemaphoreGive(dataMutex);
      }

      if (firstFrames)
        netLog("[CAN] First BMS data at %lu ms uptime - SMA frames enabled.\n", now);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- OTA ROLLBACK SAFETY NET ---
// The bootloader supports app rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE),
// but Arduino's default marks every new image valid right at startup, which
// defeats it. Returning true here defers that: loop() confirms the image only
// after kConfirmAfterMs of uptime with WiFi connected. If a freshly OTA'd
// image crashes (or is reset) before then, the bootloader falls back to the
// previous firmware - so a crash-looping build can't lock us out of OTA.
extern "C" bool verifyRollbackLater() { return true; }
constexpr unsigned long kConfirmAfterMs = 2UL * 60UL * 1000UL;

const char *otaStateName(esp_ota_img_states_t s)
{
  switch (s)
  {
  case ESP_OTA_IMG_NEW: return "new";
  case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify (rollback armed)";
  case ESP_OTA_IMG_VALID: return "valid";
  case ESP_OTA_IMG_INVALID: return "invalid";
  case ESP_OTA_IMG_ABORTED: return "aborted";
  default: return "undefined (no rollback info, e.g. USB-flashed)";
  }
}

// --- DIAGNOSTICS ---
const char *resetReasonName(esp_reset_reason_t r)
{
  switch (r)
  {
  case ESP_RST_POWERON: return "power-on";
  case ESP_RST_EXT: return "external pin";
  case ESP_RST_SW: return "software restart (e.g. OTA)";
  case ESP_RST_PANIC: return "PANIC (crash)";
  case ESP_RST_INT_WDT: return "interrupt watchdog";
  case ESP_RST_TASK_WDT: return "task watchdog";
  case ESP_RST_WDT: return "other watchdog";
  case ESP_RST_DEEPSLEEP: return "deep sleep wake";
  case ESP_RST_BROWNOUT: return "BROWNOUT (supply dip)";
  case ESP_RST_SDIO: return "SDIO";
  default: return "unknown";
  }
}

void logHealth()
{
  TaskHandle_t asyncTcp = xTaskGetHandle("async_tcp");
  TaskHandle_t sdTask = xTaskGetHandle("SD_LogTask");
  // On ESP-IDF the stack high-water mark is in bytes.
  netLog("[DIAG] Heap free %u, min %u, max block %u | stack left: loop %u, bms %u, can %u, sd %u, async_tcp %u\n",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
         (unsigned)uxTaskGetStackHighWaterMark(NULL),
         bmsTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(bmsTaskHandle) : 0u,
         canTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(canTaskHandle) : 0u,
         sdTask ? (unsigned)uxTaskGetStackHighWaterMark(sdTask) : 0u,
         asyncTcp ? (unsigned)uxTaskGetStackHighWaterMark(asyncTcp) : 0u);
}

// Why did we (re)boot, and what does the last stored core dump say? Logged
// once the clock is set (so it lands in the dated SD .log file).
void logBootDiagnostics()
{
  esp_reset_reason_t reason = esp_reset_reason();
  netLog("[DIAG] Reset reason: %s (%d), core0 %d, core1 %d\n",
         resetReasonName(reason), (int)reason,
         (int)rtc_get_reset_reason(0), (int)rtc_get_reset_reason(1));

  char elfSha[17] = {0};
  esp_ota_get_app_elf_sha256(elfSha, sizeof(elfSha));
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &otaState);
  netLog("[DIAG] Running firmware ELF sha256: %s, partition %s, image state: %s\n",
         elfSha, running ? running->label : "?", otaStateName(otaState));

  // A dump stays in flash until the next crash overwrites it, so it can be
  // from an earlier crash than the one that caused this boot - compare its
  // ELF sha with the running one, and the reset reason above.
  static esp_core_dump_summary_t summary;
  if (esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(&summary) == ESP_OK)
  {
    netLog("[DIAG] Stored core dump: task '%s', PC 0x%08x, cause %u, vaddr 0x%08x, ELF %s\n",
           summary.exc_task, (unsigned)summary.exc_pc,
           (unsigned)summary.ex_info.exc_cause, (unsigned)summary.ex_info.exc_vaddr,
           (const char *)summary.app_elf_sha256);

    char bt[200];
    size_t len = 0;
    bt[0] = '\0';
    for (uint32_t i = 0; i < summary.exc_bt_info.depth && i < 16 && len < sizeof(bt); i++)
      len += snprintf(bt + len, sizeof(bt) - len, " 0x%08x", (unsigned)summary.exc_bt_info.bt[i]);
    netLog("[DIAG] Backtrace%s:%s\n", summary.exc_bt_info.corrupted ? " (corrupted)" : "", bt);
    netLog("[DIAG] Full dump: GET /api/coredump\n");
  }
  else
  {
    netLog("[DIAG] No core dump stored.\n");
  }

  logHealth();
}

void setup()
{
  Serial.begin(115200);
  Serial.println("\nStarting LilyGO T-CAN485 BMS Bridge...");

  dataMutex = xSemaphoreCreateMutex();
  netOutMutex = xSemaphoreCreateMutex();

  // Config and SD need no network - load them before the BMS/CAN tasks,
  // which need the setpoints (and a place to log) right away.
  webUI.setActionCallback(handleUIAction);
  webUI.loadConfig(cfg);

  SDLogger::setDebugCallback(libraryLogger);
  bool sdOk = SDLogger::begin();

  currentData.packTemp = 220; // no temperature sensor is read - fixed 22.0C goes to the SMA
  currentData.smaChargeMode = "Unknown";

  // BMS and CAN come up before the network: setupNetwork() can block for up
  // to ~15s (WiFi + NTP), and the SMA should get frames as soon as real BMS
  // data exists (canTask gates on that), not after WiFi.
  bms.setDebugCallback(libraryLogger);
  bms.begin(RS485_RX, RS485_TX, RS485_SE, RS485_EN, PIN_5V_EN);

  inverter.setDebugCallback(libraryLogger);
  inverter.begin((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, (gpio_num_t)CAN_SE);

  xTaskCreatePinnedToCore(bmsTask, "BMS_Task", 6144, NULL, 1, &bmsTaskHandle, 0);
  xTaskCreatePinnedToCore(canTask, "CAN_Task", 6144, NULL, 2, &canTaskHandle, 1);

  setupNetwork();

  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("BMS-Bridge");
  ArduinoOTA.begin();

  TelnetStream.begin();
  webUI.begin();
  netReady = true;

  netLog(sdOk ? "[SYS] SD card logging initialized.\n"
              : "[SYS] SD card logging unavailable (no card or mount failed).\n");
  netLog("[SYS] Boot sequence complete. Multithreading Active.\n");
}

void loop()
{
  ArduinoOTA.handle();

  static unsigned long lastSdLog = 0;
  if (millis() - lastSdLog > 10000)
  {
    lastSdLog = millis();
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      // No placeholder rows before the first real BMS data.
      if (haveBasicInfo && haveCellData)
        SDLogger::logTelemetry(currentData);
      xSemaphoreGive(dataMutex);
    }
  }

  static bool bootDiagDone = false;
  if (!bootDiagDone && (time(nullptr) > 1000000000L || millis() > 60000))
  {
    bootDiagDone = true;
    logBootDiagnostics();
  }

  static bool imageConfirmed = false;
  if (!imageConfirmed && millis() > kConfirmAfterMs && WiFi.status() == WL_CONNECTED)
  {
    imageConfirmed = true;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    netLog("[SYS] Firmware confirmed after %lus with WiFi up (rollback cancelled): %s\n",
           kConfirmAfterMs / 1000, esp_err_to_name(err));
  }

  static unsigned long lastHealth = 0;
  if (millis() - lastHealth > 10UL * 60UL * 1000UL)
  {
    lastHealth = millis();
    logHealth();
  }

  delay(1);
}
