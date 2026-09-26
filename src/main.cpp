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
#include "ConfigStore.h"
#include "SDLogger.h"
#include "Diagnostics.h"
#include "WifiEvents.h"
#include "MutexLock.h"
#include "Interval.h"
#include "LocalClock.h"

static_assert(kPackCells <= DalyFrames::kMaxCollectorCells, "DalyRS485 can't collect every cell of the pack");

// Wi-Fi credentials AND network config (static IP, gateway, subnet, DNS) -
// all in this one gitignored file, so a public checkout never reveals your
// home network layout. See secrets_example.h for the template.
#include "secrets.h"

// Single source of truth for the local time zone, used by both setup()'s
// early setenv("TZ", ...) and setupNetwork()'s configTzTime(), so the two
// can't drift apart.
constexpr const char *kTimeZone = "CET-1CEST,M3.5.0,M10.5.0/3";

// --- GLOBAL INSTANCES ---
DalyRS485 bms(Serial2);
SMA_CAN inverter;
WebDashboard webUI(80);

SystemConfig cfg;
DashboardData currentData;
SemaphoreHandle_t dataMutex;

// Serializes netLog()'s SSE log channel and the SSE telemetry push, since
// netLog runs from bmsTask, canTask, loop() and the web server's task, and
// keeps log lines from interleaving. Innermost lock: never take another
// one while holding it.
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

// How long each dataMutex/netOutMutex user waits before giving up. Every
// holder keeps the lock for microseconds (copies, no I/O), so these only
// bound the worst case; what a timeout costs differs per site:
// canTask skips a tick (the next one is 250 ms later), bmsTask drops one
// reading (logged, see LockDropLog), loop() skips a check, netLog drops
// the SSE copy of a line (Serial and SD still get it).
constexpr TickType_t kNetOutLockTimeout = pdMS_TO_TICKS(50);
constexpr TickType_t kUiLockTimeout = pdMS_TO_TICKS(50);
constexpr TickType_t kBmsStoreLockTimeout = pdMS_TO_TICKS(50);
constexpr TickType_t kBmsCellStoreLockTimeout = pdMS_TO_TICKS(100);
constexpr TickType_t kBmsCfgReadLockTimeout = pdMS_TO_TICKS(20);
constexpr TickType_t kCanRxLockTimeout = pdMS_TO_TICKS(10);
constexpr TickType_t kCanTickLockTimeout = pdMS_TO_TICKS(20);
constexpr TickType_t kLoopLockTimeout = pdMS_TO_TICKS(20);

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

  struct tm timeinfo;
  bool haveClock = LocalClock::localNow(timeinfo);

  char final_res[350];
  if (haveClock)
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
  if (netReady)
  {
    if (MutexLock lock{netOutMutex, kNetOutLockTimeout})
      webUI.broadcastLog(final_res);
  }
  SDLogger::logEvent(loc_res);
}

void libraryLogger(const char *msg) { netLog("%s", msg); }

// SSE telemetry push, serialized with netLog's network sinks (netOutMutex).
void pushTelemetry(const DashboardData &data)
{
  if (!netReady)
    return;
  if (MutexLock lock{netOutMutex, kNetOutLockTimeout})
    webUI.broadcastTelemetry(data);
}

// --- UI EVENT HANDLER ---
void handleUIAction(const char *action)
{
  if (strcmp(action, "toggleMaint") == 0)
  {
    bool applied = false, newState = false;
    if (MutexLock lock{dataMutex, kUiLockTimeout})
    {
      uiCommands.manualMaintForce = !uiCommands.manualMaintForce;
      newState = uiCommands.manualMaintForce;
      applied = true;
    }
    if (applied)
      netLog("[USER] Manual Force Charge: %s\n", newState ? "ON" : "OFF");
    else
      netLog("[USER] %s ignored: state busy\n", action);
  }
  else if (strcmp(action, "resetSMA") == 0)
  {
    bool applied = false;
    if (MutexLock lock{dataMutex, kUiLockTimeout})
    {
      // 0 = "not armed yet": canTask starts the hold from the first status
      // frame it actually sends with the reset (DVL 0), so a request made
      // while the BMS is still silent isn't consumed by the wait.
      uiCommands.resetHoldStartMs = 0;
      uiCommands.resetRequested = true;
      applied = true;
    }
    if (applied)
      netLog("[USER] Manual Cluster Reset Triggered.\n");
    else
      netLog("[USER] %s ignored: state busy\n", action);
  }
}

// Periodic work, in ms (Interval fires once more than the period passed).
constexpr uint32_t kCanRxPeriodMs = 50;          // canTask: bus health + RX drain
constexpr uint32_t kSmaTxPeriodMs = 250;         // canTask: status frames to the SMA
constexpr uint32_t kSdTelemetryPeriodMs = 10000; // loop(): one CSV row
constexpr uint32_t kHealthPeriodMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kFirstHealthDeadlineMs = 60000; // first health sample even without NTP
constexpr uint32_t kConfirmCheckPeriodMs = 1000;   // OTA confirm, while it can still act

// --- CORE 0: BMS BACKGROUND TASK ---

// A BMS reading bmsTask couldn't store because dataMutex was busy is lost,
// and enough of them in a row let the data go stale (0 A). Logged
// edge-triggered: once when drops start, once with the total on recovery.
struct LockDropLog
{
  bool dropping = false;
  uint32_t dropped = 0;

  void note(bool stored, const char *what)
  {
    if (!stored)
    {
      if (!dropping)
        netLog("[BMS] dataMutex busy - %s reading dropped\n", what);
      dropping = true;
      dropped++;
    }
    else if (dropping)
    {
      netLog("[BMS] dataMutex free again - %lu reading(s) dropped\n", (unsigned long)dropped);
      dropping = false;
      dropped = 0;
    }
  }
};

// bmsTask's cadence: one cycle is the four Daly reads below with
// kInterFrameGapMs between them and kBmsCycleIdleMs after the last, about
// 2.4 s including the reads themselves.
constexpr uint32_t kBmsStartupDelayMs = 2000;
constexpr uint32_t kInterFrameGapMs = 100;
constexpr uint32_t kBmsCycleIdleMs = 2000;

// Everything bmsTask keeps between cycles.
struct BmsPollState
{
  // Edge-triggered SOC/MOSFET/alarm decisions - see include/BmsEvents.h.
  BmsEvents::State events;
  LockDropLog lockDrops;
  // cfg.vSamples as last read under the lock; kept if the lock is briefly
  // contended. Starts at the setting's default (def() never changes, so
  // reading it needs no lock).
  int vSamples = (int)cfg.vSamples.def();
};

// Each poll: read one Daly frame, decide/log its events outside the lock,
// then store it into currentData/bmsLink under dataMutex.

static void pollBasicInfo(BmsPollState &st)
{
  DalyBasicInfo info;
  if (!bms.readBasicInfo(info))
    return;

  // A Daly BMS recalibrates SOC to 100% on a "charge full" condition (or
  // occasionally jumps for other reasons); flag that as an event so an
  // abrupt CCL drop at the SMA can be lined up against it.
  BmsEvents::Events ev = BmsEvents::decide(st.events, &info, nullptr, nullptr);
  if (ev.socJumped)
    netLog("[BMS] SOC jumped %.1f -> %.1f %% (Daly recalibration?)\n", ev.socFrom, ev.socTo);

  bool stored = false;
  if (MutexLock lock{dataMutex, kBmsStoreLockTimeout})
  {
    currentData.packVoltage = info.packVoltage;
    currentData.packCurrent = info.packCurrent;
    currentData.packSOC = info.packSOC;
    bmsLink.lastBasicInfoMs = millis();
    bmsLink.haveBasicInfo = true;
    stored = true;
  }
  st.lockDrops.note(stored, "basic info");
}

static void pollCells(BmsPollState &st)
{
  std::vector<float> cellVolts;
  if (!bms.readCellVoltages(kPackCells, cellVolts))
    return;

  if (MutexLock lock{dataMutex, kBmsCfgReadLockTimeout})
    st.vSamples = cfg.vSamples;

  // Moving average with reseed-on-window-change (CellSmoother.h, which also
  // clamps the window to its buffer).
  CellSmoother::Result r = cellSmoother.update(cellVolts.data(), (int)cellVolts.size(), st.vSamples);
  if (r.reseeded)
    netLog("[BMS] Cell filter seeded from current reading (window %d samples)\n", st.vSamples);

  DashboardData broadcastCopy;
  bool stored = false;
  if (MutexLock lock{dataMutex, kBmsCellStoreLockTimeout})
  {
    currentData.avgCellVoltage = r.avgV;
    currentData.minCellVoltage = r.minV;
    currentData.maxCellVoltage = r.maxV;
    currentData.minCellVoltageRaw = r.rawMinV;
    currentData.maxCellVoltageRaw = r.rawMaxV;
    // Raw spread (#24), from the same unsmoothed read as rawMin/rawMax -
    // drives Glideslope::spreadFactor().
    currentData.cellSpreadRawMv = r.rawSpreadMv;
    currentData.cellVoltages.assign(r.smoothedV, r.smoothedV + r.cells);
    bmsLink.lastCellMs = millis();
    bmsLink.haveCellData = true;
    broadcastCopy = currentData; // pushed outside the lock
    stored = true;
  }
  st.lockDrops.note(stored, "cell voltage");

  if (stored)
    pushTelemetry(broadcastCopy);
}

// The BMS's own hardware protection state. Edge-triggered logging only, so
// a stuck-on alarm doesn't spam the log queue, but still lines up an SMA
// fault against the BMS's own MOSFET/alarm timeline to the second.
static void pollMosfet(BmsPollState &st)
{
  DalyMosfetStatus mos;
  if (!bms.readMosfetStatus(mos))
    return;

  BmsEvents::Events ev = BmsEvents::decide(st.events, nullptr, &mos, nullptr);
  if (ev.chargeMosChanged)
    netLog("[BMS] Charge MOSFET %s\n", ev.chargeMosOn ? "ON" : "OFF - protection or BMS-initiated cutoff");
  if (ev.dischargeMosChanged)
    netLog("[BMS] Discharge MOSFET %s\n", ev.dischargeMosOn ? "ON" : "OFF - protection or BMS-initiated cutoff");

  bool stored = false;
  if (MutexLock lock{dataMutex, kBmsStoreLockTimeout})
  {
    currentData.chargeMosOn = mos.chargeMosOn;
    currentData.dischargeMosOn = mos.dischargeMosOn;
    stored = true;
  }
  st.lockDrops.note(stored, "MOSFET status");
}

static void pollAlarms(BmsPollState &st)
{
  DalyAlarmStatus alarm;
  if (!bms.readAlarmStatus(alarm))
    return;

  // One line per changed bit (undefined bits still log, by byte.bit, so an
  // unexpected fault isn't swallowed); the first read after boot is the
  // baseline and logs nothing.
  BmsEvents::Events ev = BmsEvents::decide(st.events, nullptr, nullptr, &alarm);
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

  bool stored = false;
  if (MutexLock lock{dataMutex, kBmsStoreLockTimeout})
  {
    currentData.bmsProtectionActive = alarm.anyProtectionActive;
    currentData.cellOvervoltLevel1 = alarm.cellOvervoltLevel1;
    currentData.cellOvervoltLevel2 = alarm.cellOvervoltLevel2;
    currentData.packOvervoltLevel1 = alarm.packOvervoltLevel1;
    currentData.packOvervoltLevel2 = alarm.packOvervoltLevel2;
    stored = true;
  }
  st.lockDrops.note(stored, "alarm status");
}

void bmsTask(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(kBmsStartupDelayMs));

  BmsPollState st;
  while (true)
  {
    pollBasicInfo(st);
    vTaskDelay(pdMS_TO_TICKS(kInterFrameGapMs));
    pollCells(st);
    vTaskDelay(pdMS_TO_TICKS(kInterFrameGapMs));
    pollMosfet(st);
    vTaskDelay(pdMS_TO_TICKS(kInterFrameGapMs));
    pollAlarms(st);
    vTaskDelay(pdMS_TO_TICKS(kBmsCycleIdleMs));
  }
}

// --- WIFI EVENT LOGGING ---
// Arduino's auto-reconnect handles the actual recovery silently; this just
// makes drops/recoveries visible in the SD log, edge-triggered (one line
// per transition, not per retry while the AP is unreachable). The state
// machine lives in include/WifiEvents.h, pure and natively testable;
// main.cpp maps ESP events into the latch and logs whatever drain() flags.
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
    while (!LocalClock::clockValid(time(nullptr)) && millis() - startAttempt < 5000) {
      delay(500);
    }
    netLog(LocalClock::clockValid(time(nullptr)) ? "[SYS] NTP sync OK\n" : "[SYS] NTP sync timed out\n");
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
  Interval canRx(kCanRxPeriodMs);
  Interval smaTx(kSmaTxPeriodMs);
  StatusFrame::ControlState ctrl;

  while (true)
  {
    unsigned long now = millis();

    if (canRx.due(now))
    {
      inverter.checkBusHealth();
      if (MutexLock lock{dataMutex, kCanRxLockTimeout})
        inverter.readMessages(currentData);
    }

    if (smaTx.due(now))
    {
      StatusFrame::Decision dec;

      if (MutexLock lock{dataMutex, kCanTickLockTimeout})
      {
        dec = StatusFrame::decide(cfg, StatusFrame::snapshotFrom(currentData, bmsLink, uiCommands, now), ctrl);
        applyDecision(dec);
      }

      // Transmit outside the lock: dec is this task's own copy, and
      // inverter is only ever touched by canTask.
      if (dec.sendFrames)
        inverter.sendStatus(dec.values);

      logDecisionEvents(dec, now);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("\nStarting LilyGO T-CAN485 BMS Bridge...");

  // Set the time zone before anything can log a timestamp: the ESP32 RTC
  // keeps its time across a software/panic reset, so time(nullptr) can
  // already be valid well before setupNetwork()'s configTzTime() (which
  // still does the actual NTP sync) runs.
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

  // After SDLogger::begin(): the "[CFG] Loaded config fails validation"
  // lines only reach the SD .log once it is initialised.
  ConfigStore::load(cfg, netLog);

  // Right after the SD log exists to receive it, not deferred to loop(),
  // so a reset within the first 60s of a cold boot still gets logged.
  Diagnostics::setDebugCallback(netLog);
  Diagnostics::logBootDiagnostics();

  // BMS and CAN come up before the network: setupNetwork() can block up to
  // ~15s, and the SMA should get frames as soon as real BMS data exists.
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

  webUI.begin(&cfg);
  netReady = true;

  netLog("[SYS] Boot sequence complete. Multithreading Active.\n");
}

void loop()
{
  ArduinoOTA.handle();
  drainWifiEvents();

  static Interval sdTelemetry(kSdTelemetryPeriodMs);
  static Interval confirmCheck(kConfirmCheckPeriodMs);
  static Interval health(kHealthPeriodMs);

  if (sdTelemetry.due(millis()))
  {
    if (MutexLock lock{dataMutex, kLoopLockTimeout})
    {
      // No placeholder rows before the first real BMS data.
      if (bmsLink.ready())
        SDLogger::logTelemetry(currentData);
    }
  }

  // One logHealth() sample once the clock/BMS data settle, ahead of the
  // regular 10-minute cadence below.
  static bool firstHealthDone = false;
  if (!firstHealthDone && (LocalClock::clockValid(time(nullptr)) || millis() > kFirstHealthDeadlineMs))
  {
    firstHealthDone = true;
    Diagnostics::logHealth();
  }

  // bmsUp needs dataMutex, so it's gathered only while the check can still
  // act (after 2 min, until confirmed) and then at most once a second.
  if (Diagnostics::confirmCheckDue() && confirmCheck.due(millis()))
  {
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    bool bmsUp = false;
    if (MutexLock lock{dataMutex, kLoopLockTimeout})
      bmsUp = bmsLink.ready(); // else bmsUp stays false, retried next second
    Diagnostics::confirmImageIfReady(wifiUp, bmsUp);
  }

  if (health.due(millis()))
  {
    Diagnostics::logHealth();
  }

  delay(1);
}
