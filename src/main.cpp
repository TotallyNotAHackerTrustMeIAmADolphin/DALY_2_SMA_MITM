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

// Single source of truth for the local time zone - used both by setup()'s
// early setenv("TZ", ...) (before NTP has run) and setupNetwork()'s
// configTzTime() (which drives the actual NTP sync), so the two can't drift
// apart.
constexpr const char *kTimeZone = "CET-1CEST,M3.5.0,M10.5.0/3";

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

// Written by handleUIAction() on the web server's task, read/written by
// canTask - guarded by dataMutex like the rest of the cross-task state, so
// canTask always sees resetHoldStartTime and isResetting set together
// (previously unlocked, isResetting could be observed with a stale/zero
// resetHoldStartTime and cancel a fresh reset instantly).
unsigned long resetHoldStartTime = 0;
bool manualMaintForce = false;
bool isResetting = false;
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

uint16_t calculateCCL(float smoothedMaxV, float rawMaxV)
{
  return Glideslope::calculateCCL(cfg, smoothedMaxV, rawMaxV, bmsDataFresh(), currentData.maintenanceActive);
}

uint16_t calculateDCL(float smoothedMinV, float rawMinV)
{
  return Glideslope::calculateDCL(cfg, smoothedMinV, rawMinV, bmsDataFresh(), currentData.maintenanceActive);
}

// --- UI EVENT HANDLER ---
void handleUIAction(const char *action)
{
  if (strcmp(action, "toggleMaint") == 0)
  {
    bool newState = false;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      manualMaintForce = !manualMaintForce;
      newState = manualMaintForce;
      xSemaphoreGive(dataMutex);
      netLog("[USER] Manual Force Charge: %s\n", newState ? "ON" : "OFF");
    }
    else
    {
      netLog("[USER] %s ignored: state busy\n", action);
    }
  }
  else if (strcmp(action, "resetSMA") == 0)
  {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      // 0 = "not armed yet": canTask starts the 5.5 s hold from the first
      // status frame it actually sends with the reset bit, so a request
      // made while the BMS is still silent isn't consumed by the wait.
      resetHoldStartTime = 0;
      isResetting = true;
      xSemaphoreGive(dataMutex);
      netLog("[USER] Manual Cluster Reset Triggered.\n");
    }
    else
    {
      netLog("[USER] %s ignored: state busy\n", action);
    }
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
      float rawMin = 10.0f;
      float rawMax = 0.0f;
      std::vector<float> smoothedCellVolts;
      smoothedCellVolts.reserve(MAX_CELLS);

      static int lastKnownVSamples = 12; // falls back to this if the lock is briefly contended
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        lastKnownVSamples = cfg.vSamples;
        xSemaphoreGive(dataMutex);
      }
      int windowSize = max(1, min(MAX_SAMPLES, lastKnownVSamples));

      // Fill the whole moving-average window from the current reading
      // whenever the window size changes - including the very first
      // reading, since lastWindowSize starts at -1 and never matches a
      // real windowSize. Without this, slots [windowSize..MAX_SAMPLES)
      // keep whatever was last written there; raising cfg.vSamples at
      // runtime would then average stale (e.g. boot-time) voltages back in
      // for a whole window. (It used to be pre-filled with cvMaxCharge,
      // which made the smoothed max start near the hard limit and swung
      // the CCL 500A -> trickle -> 500A after every boot.)
      static int lastWindowSize = -1;
      bool reseeded = false;
      if (windowSize != lastWindowSize)
      {
        for (int i = 0; i < (int)cellVolts.size() && i < MAX_CELLS; i++)
        {
          uint16_t mv = (uint16_t)(cellVolts[i] * 1000.0f);
          for (int j = 0; j < MAX_SAMPLES; j++)
            cellBuffers[i][j] = mv;
        }
        bufferIndex = 0;
        lastWindowSize = windowSize;
        reseeded = true;
      }
      // bmsTask isn't holding dataMutex here.
      if (reseeded)
        netLog("[BMS] Cell filter seeded from current reading (window %d samples)\n", windowSize);

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

        // Raw (unsmoothed) min/max from this latest read - drives the
        // glideslope hard cutoff/alarm gate (#9), independent of the
        // smoothed values above which drive the taper.
        if (cellVolts[i] < rawMin) rawMin = cellVolts[i];
        if (cellVolts[i] > rawMax) rawMax = cellVolts[i];
      }

      DashboardData broadcastCopy;
      bool shouldBroadcast = false;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentData.avgCellVoltage = sum / MAX_CELLS;
        currentData.minCellVoltage = localMin;
        currentData.maxCellVoltage = localMax;
        currentData.minCellVoltageRaw = rawMin;
        currentData.maxCellVoltageRaw = rawMax;
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

// --- WIFI EVENT LOGGING ---
// Arduino's auto-reconnect handles the actual recovery silently; this just
// makes drops/recoveries visible in the SD log, edge-triggered like the
// Daly MOSFET/alarm logging (one line per transition, not per retry while
// the AP is unreachable).
const char *wifiDisconnectReasonName(uint8_t reason)
{
  switch (reason)
  {
  case WIFI_REASON_UNSPECIFIED: return "unspecified";
  case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
  case WIFI_REASON_ASSOC_EXPIRE: return "association expired";
  case WIFI_REASON_ASSOC_LEAVE: return "we disconnected (assoc leave)";
  case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (wrong password?)";
  case WIFI_REASON_AUTH_FAIL: return "auth failed";
  case WIFI_REASON_NO_AP_FOUND: return "AP not found (out of range / AP down?)";
  case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout (weak signal / interference)";
  case WIFI_REASON_MIC_FAILURE: return "MIC failure";
  case WIFI_REASON_AP_INITIATED: return "kicked by AP";
  case WIFI_REASON_STA_LEAVING: return "we disconnected (leaving)";
  default: return "see reason code";
  }
}

// Written only by the WiFi event task (arduino_events, 4KB stack), read only
// by loop() via drainWifiEvents() below - exactly one writer and one reader,
// never touched by bmsTask/canTask, so no dataMutex is needed; volatile is
// enough to keep the compiler from caching stale values across that
// producer/consumer boundary. The event handler itself must stay allocation-
// and lock-free (no netLog(), no WiFi.localIP()) - it runs on the WiFi
// event task, and netLog() takes netOutMutex and can push to SSE.
volatile bool wifiConnected = false;
volatile bool wifiEverConnected = false;
volatile uint32_t wifiDownSince = 0;
volatile bool wifiPendingDisconnect = false;
volatile bool wifiPendingReconnect = false;
volatile bool wifiPendingFirstConnect = false; // connect that arrived after setupNetwork() gave up waiting
volatile bool wifiPendingLostIp = false;
volatile uint8_t wifiLastDisconnectReason = 0;

void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info)
{
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    if (wifiConnected)
    {
      wifiConnected = false;
      wifiDownSince = millis();
      wifiLastDisconnectReason = info.wifi_sta_disconnected.reason;
      wifiPendingDisconnect = true;
    }
    // else: still trying to (re)connect - don't log every retry.
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
  {
    if (wifiEverConnected && !wifiConnected)
      wifiPendingReconnect = true;
    else if (!wifiEverConnected)
      wifiPendingFirstConnect = true; // cleared by setupNetwork() if it logged the connect itself
    wifiConnected = true;
    wifiEverConnected = true;
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP)
  {
    wifiPendingLostIp = true;
  }
}

// Emits the [WIFI] log lines wifiEventHandler() could only flag, from
// loop() instead of the WiFi event task. Order: disconnect, lost-IP,
// reconnect - matches the order those conditions actually occur in.
void drainWifiEvents()
{
  if (wifiPendingDisconnect)
  {
    wifiPendingDisconnect = false;
    uint8_t reason = wifiLastDisconnectReason;
    netLog("[WIFI] Disconnected (reason %u: %s)\n", (unsigned)reason, wifiDisconnectReasonName(reason));
  }

  if (wifiPendingLostIp)
  {
    wifiPendingLostIp = false;
    netLog("[WIFI] Lost IP address (still associated)\n");
  }

  if (wifiPendingFirstConnect)
  {
    wifiPendingFirstConnect = false;
    netLog("[WIFI] Connected, IP %s\n", WiFi.localIP().toString().c_str());
  }
  if (wifiPendingReconnect)
  {
    wifiPendingReconnect = false;
    unsigned long downMs = millis() - wifiDownSince;
    netLog("[WIFI] Reconnected (%s), RSSI %d dBm, was down for %lus\n",
           WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), downMs / 1000);
  }
}

void setupNetwork()
{
  WiFi.onEvent(wifiEventHandler);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS))
  {
    netLog("[WIFI] STA config (static IP) failed - falling back to DHCP\n");
  }
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000)
  {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiPendingFirstConnect = false; // logged here, don't repeat it from drainWifiEvents()
    netLog("[WIFI] Connected, IP %s\n", WiFi.localIP().toString().c_str());

    configTzTime(kTimeZone, "pool.ntp.org", "ptbtime1.ptb.de");

    // Wait for NTP sync (up to 5 seconds)
    netLog("[SYS] Waiting for NTP sync...\n");
    startAttempt = millis();
    while (time(nullptr) < 1000000000L && millis() - startAttempt < 5000) {
      delay(500);
    }
    netLog(time(nullptr) > 1000000000L ? "[SYS] NTP sync OK\n" : "[SYS] NTP sync timed out\n");
  }
  else
  {
    netLog("[WIFI] Not connected after 10s - continuing in offline mode (will keep retrying in the background)\n");
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

    if (now - lastSmaTx > 250)
    {
      lastSmaTx = now;
      bool firstFrames = false;
      bool resetFinished = false;
      bool fresh = false;
      int bmsTimeoutCopy = 0;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        // Send nothing until the BMS has delivered basic info AND cell
        // voltages once. Before that currentData holds no real values, and
        // the SMA already rides through a few seconds of CAN silence on
        // every reboot - better than made-up SOC/voltage/limits.
        if (haveBasicInfo && haveCellData)
        {
          // The reset hold is measured from the first frame sent with the
          // reset bit (handleUIAction() arms it with resetHoldStartTime = 0
          // under this same mutex), not from the click, so a request made
          // while no frames go out still gets its full 5.5 s on the bus.
          if (isResetting)
          {
            if (resetHoldStartTime == 0)
              resetHoldStartTime = now ? now : 1;
            else if (now - resetHoldStartTime > 5500)
            {
              isResetting = false;
              resetFinished = true;
            }
          }

          if (!autoMaint && currentData.packVoltage > 0 && currentData.packVoltage < (cfg.cvMaintStart * MAX_CELLS))
            autoMaint = true;
          else if (autoMaint && currentData.packVoltage > (cfg.cvMaintStop * MAX_CELLS))
            autoMaint = false;

          currentData.maintenanceActive = manualMaintForce || autoMaint;
          currentData.forceCharge = currentData.maintenanceActive;
          currentData.isResetting = isResetting;

          // Same freshness check calculateCCL()/calculateDCL() use below -
          // captured here so the edge-triggered log after this lock can
          // report a stale->0A transition without re-deriving it unlocked.
          fresh = bmsDataFresh();
          bmsTimeoutCopy = cfg.bmsTimeout;

          SMATxData tx;
          tx.packVoltage = currentData.packVoltage;
          tx.packCurrent = currentData.packCurrent;
          tx.packTemp = currentData.packTemp;
          tx.packSOC = currentData.packSOC;
          tx.maintenanceActive = currentData.maintenanceActive;
          tx.isResetting = currentData.isResetting;

          tx.ccl = calculateCCL(currentData.maxCellVoltage, currentData.maxCellVoltageRaw);
          currentData.requestedCurrent = tx.ccl / 10.0f;
          tx.dcl = calculateDCL(currentData.minCellVoltage, currentData.minCellVoltageRaw);
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
      if (resetFinished)
        netLog("[SYS] Recovery cycle finished.\n");

      // Edge-triggered, and only meaningful once real BMS data has started
      // flowing (framesEnabled) - staleBaseline gates the "fresh again" log
      // so the very first-ever fresh reading isn't reported as a recovery.
      static bool staleBaseline = false;
      static bool wasFresh = false;
      if (framesEnabled)
      {
        if (wasFresh && !fresh)
        {
          netLog("[BMS] Data stale (both reads older than %d s) - CCL/DCL forced to 0 A\n", bmsTimeoutCopy);
          staleBaseline = true;
        }
        else if (!wasFresh && fresh && staleBaseline)
        {
          netLog("[BMS] Data fresh again - limits restored\n");
        }
        wasFresh = fresh;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- OTA ROLLBACK SAFETY NET ---
// The bootloader supports app rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE),
// but Arduino's default marks every new image valid right at startup, which
// defeats it. Returning true here defers that: loop() confirms the image
// only after kConfirmAfterMs of uptime with WiFi connected AND real BMS data
// flowing (haveBasicInfo && haveCellData) - WiFi alone would confirm a build
// with a broken RS485/CAN path. If a freshly OTA'd image crashes (or is
// reset) before that, the bootloader falls back to the previous firmware -
// so a crash-looping (or BMS-link-broken) build can't lock us out of OTA.
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
  TaskHandle_t evt = xTaskGetHandle("arduino_events");

  char rssi[16];
  if (WiFi.status() == WL_CONNECTED)
    snprintf(rssi, sizeof(rssi), "%d dBm", (int)WiFi.RSSI());
  else
    strlcpy(rssi, "n/a", sizeof(rssi));

  // On ESP-IDF the stack high-water mark is in bytes.
  netLog("[DIAG] Heap free %u, min %u, max block %u | stack left: loop %u, bms %u, can %u, sd %u, async_tcp %u, events %u | WiFi RSSI %s\n",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
         (unsigned)uxTaskGetStackHighWaterMark(NULL),
         bmsTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(bmsTaskHandle) : 0u,
         canTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(canTaskHandle) : 0u,
         sdTask ? (unsigned)uxTaskGetStackHighWaterMark(sdTask) : 0u,
         asyncTcp ? (unsigned)uxTaskGetStackHighWaterMark(asyncTcp) : 0u,
         evt ? (unsigned)uxTaskGetStackHighWaterMark(evt) : 0u,
         rssi);
}

// Why did we (re)boot, and what does the last stored core dump say? Called
// directly from setup() right after SD init, so even a reset within the
// first 60s of a cold boot (or a crash loop) still gets its reason onto the
// SD log - it no longer waits for loop()'s clock-ready gate.
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

  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  if (bad)
  {
    netLog("[DIAG] ROLLED BACK: partition %s is invalid/aborted - running the previous firmware\n", bad->label);
  }

  if (otaState == ESP_OTA_IMG_PENDING_VERIFY)
  {
    netLog("[DIAG] Image pending verification: OTA is refused until it is confirmed (needs >= 120 s uptime with WiFi up and BMS data); a reset before that boots the previous firmware.\n");
  }

  // A dump stays in flash until the next crash overwrites it, so it can be
  // from an earlier crash than the one that caused this boot - compare its
  // ELF sha with the running one, and the reset reason above.
  static esp_core_dump_summary_t summary;
  esp_err_t chk = esp_core_dump_image_check();
  if (chk == ESP_OK && esp_core_dump_get_summary(&summary) == ESP_OK)
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
  else if (chk == ESP_ERR_NOT_FOUND || chk == ESP_ERR_INVALID_SIZE)
  {
    netLog("[DIAG] No core dump stored.\n");
  }
  else
  {
    netLog("[DIAG] Core dump present but unreadable (%s).\n", esp_err_to_name(chk));
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("\nStarting LilyGO T-CAN485 BMS Bridge...");

  // Set the time zone before anything can log a timestamp. The ESP32 RTC
  // keeps its time across a software/panic reset, so time(nullptr) can
  // already be valid (tm_year > 70) right after such a reboot, well before
  // setupNetwork()'s configTzTime() runs - without this, those early lines
  // (e.g. the CAN driver start) were stamped in UTC while everything after
  // WiFi connects used local time, so the same boot showed two different
  // clocks depending on how far setup() had gotten. configTzTime() (called
  // later, once WiFi is up) still does the actual NTP sync.
  setenv("TZ", kTimeZone, 1);
  tzset();

  dataMutex = xSemaphoreCreateMutex();
  netOutMutex = xSemaphoreCreateMutex();

  // Config and SD need no network - load them before the BMS/CAN tasks,
  // which need the setpoints (and a place to log) right away.
  webUI.setActionCallback(handleUIAction);
  webUI.loadConfig(cfg);

  SDLogger::setDebugCallback(libraryLogger);
  bool sdOk = SDLogger::begin();
  netLog(sdOk ? "[SYS] SD card logging initialized.\n"
              : "[SYS] SD card logging unavailable (no card or mount failed).\n");

  // Reset reason / rollback state / core dump summary, right after the SD
  // log exists to receive it - not deferred to loop(), so a reset within
  // the first 60s of a cold boot (or a crash loop) still gets logged.
  logBootDiagnostics();

  currentData.packTemp = 220; // no temperature sensor is read - fixed 22.0C goes to the SMA
  currentData.smaChargeMode = "Unknown";
  currentData.minCellVoltageRaw = 0;
  currentData.maxCellVoltageRaw = 0;

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

  netLog("[SYS] Boot sequence complete. Multithreading Active.\n");
}

void loop()
{
  ArduinoOTA.handle();
  drainWifiEvents();

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

  // logBootDiagnostics() itself now runs from setup(), right after SD init,
  // so even a reset in the first 60s (or a crash loop) gets logged. This
  // just gets one logHealth() sample in once the clock/BMS data settle,
  // ahead of the regular 10-minute cadence below.
  static bool firstHealthDone = false;
  if (!firstHealthDone && (time(nullptr) > 1000000000L || millis() > 60000))
  {
    firstHealthDone = true;
    logHealth();
  }

  static bool imageConfirmed = false;
  static bool unconfirmedWarned = false;
  if (!imageConfirmed && millis() > kConfirmAfterMs)
  {
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    bool bmsUp = false;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      bmsUp = haveBasicInfo && haveCellData;
      xSemaphoreGive(dataMutex);
    }
    // If the mutex take fails, bmsUp stays false for this pass and the
    // check is simply retried next loop() iteration.

    if (wifiUp && bmsUp)
    {
      imageConfirmed = true;
      esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      netLog("[SYS] Firmware confirmed after %lus uptime with WiFi up and BMS data flowing (rollback cancelled): %s\n",
             millis() / 1000, esp_err_to_name(err));
    }
    else if (!unconfirmedWarned)
    {
      esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
      esp_ota_get_state_partition(esp_ota_get_running_partition(), &st);
      if (st == ESP_OTA_IMG_PENDING_VERIFY)
      {
        unconfirmedWarned = true;
        netLog("[SYS] Firmware NOT confirmed at %lus: %s%s- a reset now boots the previous firmware\n",
               millis() / 1000, wifiUp ? "" : "WiFi down ", bmsUp ? "" : "no BMS data ");
      }
    }
  }

  static unsigned long lastHealth = 0;
  if (millis() - lastHealth > 10UL * 60UL * 1000UL)
  {
    lastHealth = millis();
    logHealth();
  }

  delay(1);
}
