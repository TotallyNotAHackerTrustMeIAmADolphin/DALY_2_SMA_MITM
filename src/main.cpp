#include <Arduino.h>
#include <cstring>
#include <cmath>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <time.h>

#include "pin_config.h"
#include "SystemState.h"
#include "CellSmoother.h"
#include "StatusFrame.h"
#include "BmsEvents.h"
#include "DalyRS485.h"
#include "SMA_CAN.h"
#include "WebDashboard.h"
#include "SDLogger.h"
#include "Diagnostics.h"

// Bring in your Wi-Fi credentials AND network config (static IP, gateway,
// subnet, DNS) - all of it lives in this one gitignored file now, so a
// public checkout never reveals your home network layout. See
// secrets_example.h for the template.
#include "secrets.h"

// Single source of truth is CellSmoother.h (#31); kept as plain constants
// here too since canTask and other code below still reference MAX_CELLS
// well outside bmsTask/CellSmoother's own scope (pack voltage, CVL/DVL).
constexpr int MAX_CELLS = CellSmoother::MAX_CELLS;
constexpr int MAX_SAMPLES = CellSmoother::MAX_SAMPLES;
static_assert(MAX_SAMPLES == kMaxVSamples, "SystemConfig::validate() range for vSamples must match CellSmoother");

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

// Serializes netLog()'s SSE log channel and the SSE telemetry push, since
// netLog runs from bmsTask, canTask, loop() and the web server's task.
// AsyncEventSource locks internally since ESPAsyncWebServer 3.x; keeping it
// behind the same mutex also stops log lines from interleaving. Innermost
// lock: never take another one while holding it.
SemaphoreHandle_t netOutMutex;
// Set once WiFi and the web server are up. Before that, netLog() only
// writes to Serial and the SD card.
volatile bool netReady = false;

// Per-cell moving-average smoother (bmsTask only) - see CellSmoother.h.
CellSmoother cellSmoother;

// Written by handleUIAction() on the web server's task, read/written by
// canTask (via StatusFrame::Snapshot/Decision - see include/StatusFrame.h)
// - guarded by dataMutex like the rest of the cross-task state, so canTask
// always sees resetHoldStartTime and isResetting set together (previously
// unlocked, isResetting could be observed with a stale/zero
// resetHoldStartTime and cancel a fresh reset instantly). autoMaint used to
// live here too; it's now inside canTask's local StatusFrame::ControlState,
// since nothing outside canTask reads or writes it.
unsigned long resetHoldStartTime = 0;
bool manualMaintForce = false;
bool isResetting = false;

// BMS data freshness - guarded by dataMutex. Nothing is sent to the SMA
// until both have succeeded once (see canTask), and StatusFrame::decide()
// (include/StatusFrame.h) treats "never read" as stale (Glideslope::isFresh).
bool haveBasicInfo = false;
bool haveCellData = false;
unsigned long lastBasicInfoRead = 0;
unsigned long lastCellRead = 0;

// --- CENTRAL LOGGING ---
// printf format checking: a Setting<T> (or any wrong type) passed for a
// %d/%f is a compile warning instead of garbage in the log - varargs never
// apply Setting's conversion to T.
void netLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
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
}

// --- CORE 0: BMS BACKGROUND TASK ---
void bmsTask(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Persistent decision state for the edge-triggered SOC/MOSFET/alarm
  // events below - replaces the three function-local `static`s this used
  // to hold (#44, see include/BmsEvents.h).
  BmsEvents::State bmsEventState;

  while (true)
  {
    DalyBasicInfo info;
    if (bms.readBasicInfo(info))
    {
      // A Daly BMS recalibrates SOC to 100% on a "charge full" condition
      // (or occasionally jumps for other reasons); flag that as an event so
      // an abrupt CCL drop at the SMA can be lined up against it. Logged
      // outside dataMutex, before the mutex-protected store below.
      BmsEvents::Events ev = BmsEvents::decide(bmsEventState, &info, nullptr, nullptr);
      if (ev.socJumped)
        netLog("[BMS] SOC jumped %.1f -> %.1f %% (Daly recalibration?)\n", ev.socFrom, ev.socTo);

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
      static int lastKnownVSamples = 12; // falls back to this if the lock is briefly contended
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        lastKnownVSamples = cfg.vSamples;
        xSemaphoreGive(dataMutex);
      }
      int windowSize = max(1, min(MAX_SAMPLES, lastKnownVSamples));

      // The moving-average/reseed-on-window-change logic (and the
      // documented boot-swing regression it guards against - CCL
      // 500A -> trickle -> 500A after boot from stale ring-buffer slots)
      // now lives in CellSmoother.h, with its own native test.
      CellSmoother::Result r = cellSmoother.update(cellVolts.data(), (int)cellVolts.size(), windowSize);

      // bmsTask isn't holding dataMutex here.
      if (r.reseeded)
        netLog("[BMS] Cell filter seeded from current reading (window %d samples)\n", windowSize);

      DashboardData broadcastCopy;
      bool shouldBroadcast = false;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentData.avgCellVoltage = r.avgV;
        currentData.minCellVoltage = r.minV;
        currentData.maxCellVoltage = r.maxV;
        currentData.minCellVoltageRaw = r.rawMinV;
        currentData.maxCellVoltageRaw = r.rawMaxV;
        // Raw spread (#24), from the same unsmoothed read as rawMin/rawMax
        // above - drives Glideslope::spreadFactor() in canTask.
        currentData.cellSpreadRawMv = r.rawSpreadMv;
        currentData.cellVoltages.assign(r.smoothedV, r.smoothedV + r.cells);
        lastCellRead = millis();
        haveCellData = true;

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
    DalyMosfetStatus mosStatus;
    if (bms.readMosfetStatus(mosStatus))
    {
      BmsEvents::Events ev = BmsEvents::decide(bmsEventState, nullptr, &mosStatus, nullptr);
      if (ev.chargeMosChanged)
        netLog("[BMS] Charge MOSFET %s\n", ev.chargeMosOn ? "ON" : "OFF - protection or BMS-initiated cutoff");
      if (ev.dischargeMosChanged)
        netLog("[BMS] Discharge MOSFET %s\n", ev.dischargeMosOn ? "ON" : "OFF - protection or BMS-initiated cutoff");

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
      // Edge-triggered, one line per changed bit (undefined bits still log,
      // by byte.bit position, so an unexpected fault isn't silently
      // swallowed) - baseline flag so the first read after boot doesn't log
      // every bit as "SET"/"CLEARED" from an all-zero starting point.
      BmsEvents::Events ev = BmsEvents::decide(bmsEventState, nullptr, nullptr, &alarmStatus);
      for (int i = 0; i < ev.alarmBitCount; i++)
      {
        const BmsEvents::AlarmBitEvent &e = ev.alarmBits[i];
        if (e.name)
          netLog("[BMS] Alarm: %s %s\n", e.name, e.set ? "SET" : "CLEARED");
        else
          netLog("[BMS] Alarm: bit %d.%d %s\n", e.byteIndex, e.bitIndex, e.set ? "SET" : "CLEARED");
      }
      if (ev.faultCodeChanged)
        netLog("[BMS] Fault code %u -> %u\n", ev.faultCodeFrom, ev.faultCodeTo);

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
  StatusFrame::ControlState ctrl;

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
      StatusFrame::Decision dec;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        StatusFrame::Snapshot snap;
        snap.nowMs = now;
        snap.haveBasicInfo = haveBasicInfo;
        snap.haveCellData = haveCellData;
        snap.lastBasicInfoReadMs = lastBasicInfoRead;
        snap.lastCellReadMs = lastCellRead;
        snap.packVoltage = currentData.packVoltage;
        snap.packCurrent = currentData.packCurrent;
        snap.packSOC = currentData.packSOC;
        snap.packTemp = currentData.packTemp;
        snap.maxCellSmoothedV = currentData.maxCellVoltage;
        snap.maxCellRawV = currentData.maxCellVoltageRaw;
        snap.minCellSmoothedV = currentData.minCellVoltage;
        snap.minCellRawV = currentData.minCellVoltageRaw;
        snap.cellSpreadMv = currentData.cellSpreadRawMv;
        snap.manualMaintForce = manualMaintForce;
        snap.resetRequested = isResetting;
        snap.resetHoldStartMs = resetHoldStartTime;

        dec = StatusFrame::decide(cfg, snap, ctrl);

        // Written back regardless of sendFrames (a no-op copy-back when
        // decide() didn't touch them - see its comment).
        isResetting = dec.isResetting;
        resetHoldStartTime = dec.resetHoldStartMs;

        if (dec.sendFrames)
        {
          currentData.maintenanceActive = dec.maintenanceActive;
          currentData.forceCharge = dec.values.forceCharge;
          currentData.isResetting = dec.isResetting;
          currentData.derateFactor = dec.derateFactor;
          currentData.requestedCurrent = dec.values.ccl / 10.0f;

          SMATxData tx;
          tx.packVoltage = dec.values.packVoltage;
          tx.packCurrent = dec.values.packCurrent;
          tx.packTemp = dec.values.packTemp;
          tx.packSOC = dec.values.packSOC;
          tx.maintenanceActive = dec.values.maintenanceActive;
          tx.isResetting = dec.values.isResetting;
          tx.ccl = dec.values.ccl;
          tx.dcl = dec.values.dcl;
          tx.cvl = dec.values.cvl;
          tx.dvl = dec.values.dvl;

          inverter.sendStatus(tx);
        }
        xSemaphoreGive(dataMutex);
      }

      // Log lines for whichever events decide() flagged, outside the lock -
      // same wording as before the StatusFrame extraction (#29).
      if (dec.events.firstFrames)
        netLog("[CAN] First BMS data at %lu ms uptime - SMA frames enabled.\n", now);
      if (dec.events.resetFinished)
        netLog("[SYS] Recovery cycle finished.\n");
      if (dec.events.wentStale)
        netLog("[BMS] Data stale (both reads older than %d s) - CCL/DCL forced to 0 A\n", dec.events.bmsTimeoutS);
      if (dec.events.freshAgain)
        netLog("[BMS] Data fresh again - limits restored\n");
      if (dec.events.deratingStarted)
        netLog("[BMS] Cell spread %u mV - limits derated to %u %%\n",
               (unsigned)dec.events.spreadMv, (unsigned)dec.events.deratePercent);
      if (dec.events.deratingEnded)
        netLog("[BMS] Cell spread %u mV - derating ended\n", (unsigned)dec.events.spreadMv);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
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
  webUI.setDebugCallback(netLog);

  SDLogger::setDebugCallback(libraryLogger);
  bool sdOk = SDLogger::begin();
  netLog(sdOk ? "[SYS] SD card logging initialized.\n"
              : "[SYS] SD card logging unavailable (no card or mount failed).\n");

  // After SDLogger::begin(), not before: loadConfig() logs any
  // "[CFG] Loaded config fails validation" lines (#56), and SDLogger drops
  // events until it is initialised - on the headless device the SD .log is
  // the only place those would be seen.
  webUI.loadConfig(cfg);

  // Reset reason / rollback state / core dump summary, right after the SD
  // log exists to receive it - not deferred to loop(), so a reset within
  // the first 60s of a cold boot (or a crash loop) still gets logged.
  Diagnostics::setDebugCallback(netLog);
  Diagnostics::logBootDiagnostics();

  currentData.packTemp = 220; // no temperature sensor is read - fixed 22.0C goes to the SMA
  currentData.smaChargeMode = "Unknown";
  currentData.minCellVoltageRaw = 0;
  currentData.maxCellVoltageRaw = 0;
  currentData.cellSpreadRawMv = 0;
  currentData.derateFactor = 1.0f; // no derating until canTask's first cycle computes the real factor

  // BMS and CAN come up before the network: setupNetwork() can block for up
  // to ~15s (WiFi + NTP), and the SMA should get frames as soon as real BMS
  // data exists (canTask gates on that), not after WiFi.
  bms.setDebugCallback(libraryLogger);
  bms.begin(RS485_RX, RS485_TX, RS485_SE, RS485_EN, PIN_5V_EN);

  inverter.setDebugCallback(libraryLogger);
  inverter.begin((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, (gpio_num_t)CAN_SE);

  // No handle output needed here - Diagnostics::logHealth() looks these up
  // by name (xTaskGetHandle("BMS_Task")/("CAN_Task")) instead.
  xTaskCreatePinnedToCore(bmsTask, "BMS_Task", 6144, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(canTask, "CAN_Task", 6144, NULL, 2, NULL, 1);

  setupNetwork();

  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("BMS-Bridge");
  ArduinoOTA.begin();

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

  // Diagnostics::logBootDiagnostics() itself now runs from setup(), right
  // after SD init, so even a reset in the first 60s (or a crash loop) gets
  // logged. This just gets one logHealth() sample in once the clock/BMS
  // data settle, ahead of the regular 10-minute cadence below.
  static bool firstHealthDone = false;
  if (!firstHealthDone && (time(nullptr) > 1000000000L || millis() > 60000))
  {
    firstHealthDone = true;
    Diagnostics::logHealth();
  }

  // wifiUp/bmsUp is the same predicate this block always evaluated inline;
  // bmsUp needs dataMutex, so it's still read here. The millis() gate, the
  // one-shot "not confirmed" warning and the actual rollback-cancel call
  // now live in Diagnostics::confirmImageIfReady().
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  bool bmsUp = false;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    bmsUp = haveBasicInfo && haveCellData;
    xSemaphoreGive(dataMutex);
  }
  // If the mutex take fails, bmsUp stays false for this pass and the
  // check is simply retried next loop() iteration.
  Diagnostics::confirmImageIfReady(wifiUp, bmsUp);

  static unsigned long lastHealth = 0;
  if (millis() - lastHealth > 10UL * 60UL * 1000UL)
  {
    lastHealth = millis();
    Diagnostics::logHealth();
  }

  delay(1);
}
