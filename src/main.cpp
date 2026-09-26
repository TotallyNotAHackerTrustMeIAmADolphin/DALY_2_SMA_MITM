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
#include "WifiEvents.h"

// Bring in your Wi-Fi credentials AND network config (static IP, gateway,
// subnet, DNS) - all of it lives in this one gitignored file now, so a
// public checkout never reveals your home network layout. See
// secrets_example.h for the template.
#include "secrets.h"

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

// Cross-task control state, guarded by dataMutex like currentData/cfg -
// see StatusFrame::BmsLink / UiCommands for who writes what.
StatusFrame::BmsLink bmsLink;
StatusFrame::UiCommands uiCommands;

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
      uiCommands.manualMaintForce = !uiCommands.manualMaintForce;
      newState = uiCommands.manualMaintForce;
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
      // status frame it actually sends with the reset (DVL 0, #63), so a request
      // made while the BMS is still silent isn't consumed by the wait.
      uiCommands.resetHoldStartMs = 0;
      uiCommands.resetRequested = true;
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
        bmsLink.lastBasicInfoMs = millis();
        bmsLink.haveBasicInfo = true;
        xSemaphoreGive(dataMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    std::vector<float> cellVolts;
    if (bms.readCellVoltages(kPackCells, cellVolts))
    {
      static int lastKnownVSamples = 12; // falls back to this if the lock is briefly contended
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        lastKnownVSamples = cfg.vSamples;
        xSemaphoreGive(dataMutex);
      }
      int windowSize = max(1, min(CellSmoother::MAX_SAMPLES, lastKnownVSamples));

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
        bmsLink.lastCellMs = millis();
        bmsLink.haveCellData = true;

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
// the AP is unreachable). The actual state machine (#69) lives in
// include/WifiEvents.h, pure and natively testable; main.cpp is a thin
// adapter: map ESP events into the latch, and turn drain() results into the
// same [WIFI] lines as before.
WifiEvents::WifiEventLatch wifiEvents;

// WifiEvents::reasonName() spells these out as literals.
static_assert(WIFI_REASON_UNSPECIFIED == 1 && WIFI_REASON_AUTH_EXPIRE == 2 &&
                  WIFI_REASON_ASSOC_EXPIRE == 4 && WIFI_REASON_ASSOC_LEAVE == 8 &&
                  WIFI_REASON_MIC_FAILURE == 14 && WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT == 15 &&
                  WIFI_REASON_STA_LEAVING == 36 && WIFI_REASON_AP_INITIATED == 47 &&
                  WIFI_REASON_BEACON_TIMEOUT == 200 && WIFI_REASON_NO_AP_FOUND == 201 &&
                  WIFI_REASON_AUTH_FAIL == 202,
              "WIFI_REASON_* values changed - update WifiEvents::reasonName()");

void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info)
{
  // Runs on the WiFi event task (arduino_events, 4KB stack) - must stay
  // allocation- and lock-free (no netLog(), no WiFi.localIP()); netLog()
  // takes netOutMutex and can push to SSE.
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    wifiEvents.onEvent(WifiEvents::Kind::Disconnected, info.wifi_sta_disconnected.reason, millis());
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
  {
    wifiEvents.onEvent(WifiEvents::Kind::Connected, 0, millis());
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP)
  {
    wifiEvents.onEvent(WifiEvents::Kind::LostIp, 0, millis());
  }
}

// Emits the [WIFI] log lines the latch could only flag, from loop() instead
// of the WiFi event task. Order: disconnect, lost-IP, first-connect,
// reconnect - matches the order those conditions actually occur in.
void drainWifiEvents()
{
  WifiEvents::Pending pending = wifiEvents.drain(millis());

  if (pending.disconnect)
  {
    netLog("[WIFI] Disconnected (reason %u: %s)\n", (unsigned)pending.disconnectReason,
           WifiEvents::reasonName(pending.disconnectReason));
  }

  if (pending.lostIp)
  {
    netLog("[WIFI] Lost IP address (still associated)\n");
  }

  if (pending.firstConnect)
  {
    netLog("[WIFI] Connected, IP %s\n", WiFi.localIP().toString().c_str());
  }
  if (pending.reconnect)
  {
    netLog("[WIFI] Reconnected (%s), RSSI %d dBm, was down for %lus\n",
           WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned long)(pending.downForMs / 1000));
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
    wifiEvents.suppressFirstConnect(); // logged here, don't repeat it from drainWifiEvents()
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
// Writes a Decision back into the shared state. Caller holds dataMutex.
static void applyDecision(const StatusFrame::Decision &dec)
{
  // Written back regardless of sendFrames (a no-op copy-back when decide()
  // didn't touch them).
  uiCommands.resetRequested = dec.isResetting;
  uiCommands.resetHoldStartMs = dec.resetHoldStartMs;

  if (dec.sendFrames)
  {
    currentData.maintenanceActive = dec.values.maintenanceActive;
    currentData.forceCharge = dec.values.maintenanceActive;
    currentData.isResetting = dec.isResetting;
    currentData.derateFactor = dec.derateFactor;
    currentData.requestedCurrent = dec.values.ccl / 10.0f;
  }
}

// The log lines for whichever one-shot events decide() flagged. Called
// outside the lock.
static void logDecisionEvents(const StatusFrame::Decision &dec, unsigned long now)
{
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
        dec = StatusFrame::decide(cfg, StatusFrame::snapshotFrom(currentData, bmsLink, uiCommands, now), ctrl);
        applyDecision(dec);
        xSemaphoreGive(dataMutex);

        // Transmit outside the lock (#74): dec is this task's own copy, and
        // inverter is only ever touched by canTask.
        if (dec.sendFrames)
          inverter.sendStatus(dec.values);
      }

      logDecisionEvents(dec, now);
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
      if (bmsLink.ready())
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

  // OTA rollback confirm (#75): bmsUp needs dataMutex, which canTask
  // waits on for only 10-20 ms, so it is gathered only while the check can
  // still act (after 2 min, until confirmed) and then at most once a
  // second - not on every ~1 ms loop() pass for the whole uptime. The
  // one-shot "not confirmed" warning and the rollback cancel live in
  // Diagnostics::confirmImageIfReady().
  static unsigned long lastConfirmCheck = 0;
  if (Diagnostics::confirmCheckDue() && millis() - lastConfirmCheck >= 1000)
  {
    lastConfirmCheck = millis();
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    bool bmsUp = false;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      bmsUp = bmsLink.ready();
      xSemaphoreGive(dataMutex);
    }
    // If the mutex take fails, bmsUp stays false for this pass and the
    // check is simply retried a second later.
    Diagnostics::confirmImageIfReady(wifiUp, bmsUp);
  }

  static unsigned long lastHealth = 0;
  if (millis() - lastHealth > 10UL * 60UL * 1000UL)
  {
    lastHealth = millis();
    Diagnostics::logHealth();
  }

  delay(1);
}
