#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>
#include <time.h>

#include "pin_config.h"
#include "SystemState.h"
#include "DalyRS485.h"
#include "SMA_CAN.h"
#include "WebDashboard.h"

// Bring in your Wi-Fi credentials securely
#include "secrets.h"

// --- CONFIG & NETWORK ---
IPAddress local_IP(192, 168, 178, 56);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);   // Google DNS
IPAddress secondaryDNS(1, 1, 1, 1); // Cloudflare DNS

#define CELL_COUNT 16.0f

// --- GLOBAL INSTANCES ---
DalyRS485 bms(Serial2);
SMA_CAN inverter;
WebDashboard webUI(80);

SystemConfig cfg;
DashboardData currentData;
SemaphoreHandle_t dataMutex;

unsigned long lastBmsPoll = 0;
unsigned long lastSmaTx = 0;
unsigned long resetHoldStartTime = 0;
bool manualMaintForce = false;
bool isResetting = false;
bool autoMaint = false;

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
  if (TelnetStream.availableForWrite() > 0) {
    TelnetStream.print(final_res);
  }
  webUI.broadcastLog(final_res);
}

void libraryLogger(const char *msg) { netLog("%s", msg); }

// --- GLIDESLOPE LOGIC ---
uint16_t calculateCCL(float maxCellV)
{
  if (currentData.maintenanceActive)
    return (uint16_t)round(cfg.maintAmps * 10.0f);

  if (maxCellV >= cfg.cvMaxCharge)
    return 0;
  if (maxCellV >= cfg.cvHighAlarmGate)
    return (uint16_t)round(cfg.trickleA * 10.0f);

  if (maxCellV > cfg.cvStartTaper)
  {
    float div = cfg.cvHighAlarmGate - cfg.cvStartTaper;
    if (div <= 0.001f) return (uint16_t)round(cfg.trickleA * 10.0f);
    
    float slope = (cfg.cvHighAlarmGate - maxCellV) / div;
    float target = cfg.trickleA + (slope * (cfg.maxChargeA - cfg.trickleA));
    return (uint16_t)round(max(target, cfg.trickleA) * 10.0f);
  }
  return (uint16_t)round(cfg.maxChargeA * 10.0f);
}

uint16_t calculateDCL(float minCellV)
{
  if (currentData.maintenanceActive)
    return 0;

  if (minCellV <= cfg.cvMinDischarge)
    return 0;
  if (minCellV <= cfg.cvLowAlarmGate)
    return (uint16_t)round(cfg.limpDischargeA * 10.0f);

  if (minCellV < cfg.cvStartDTaper)
  {
    float div = cfg.cvStartDTaper - cfg.cvLowAlarmGate;
    if (div <= 0.001f) return (uint16_t)round(cfg.limpDischargeA * 10.0f);

    float slope = (minCellV - cfg.cvLowAlarmGate) / div;
    float target = cfg.limpDischargeA + (slope * (cfg.maxDischargeA - cfg.limpDischargeA));
    return (uint16_t)round(max(target, cfg.limpDischargeA) * 10.0f);
  }
  return (uint16_t)round(cfg.maxDischargeA * 10.0f);
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
        xSemaphoreGive(dataMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    std::vector<float> cellVolts;
    if (bms.readCellVoltages(CELL_COUNT, cellVolts))
    {
      float sum = 0;
      float localMin = 10.0f;
      float localMax = 0.0f;

      for (float v : cellVolts)
      {
        sum += v;
        if (v < localMin) localMin = v;
        if (v > localMax) localMax = v;
      }

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentData.avgCellVoltage = sum / CELL_COUNT;
        currentData.minCellVoltage = localMin;
        currentData.maxCellVoltage = localMax;
        currentData.cellVoltages = cellVolts;
        
        // Broadcast while holding mutex to ensure vector stability
        webUI.broadcastTelemetry(currentData);
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

void setup()
{
  Serial.begin(115200);
  Serial.println("\nStarting LilyGO T-CAN485 BMS Bridge...");

  dataMutex = xSemaphoreCreateMutex();

  setupNetwork();

  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("BMS-Bridge");
  ArduinoOTA.begin();

  TelnetStream.begin();

  webUI.setActionCallback(handleUIAction);
  webUI.begin(cfg);

  bms.setDebugCallback(libraryLogger);
  bms.begin(RS485_RX, RS485_TX, RS485_SE, RS485_EN, PIN_5V_EN);

  inverter.setDebugCallback(libraryLogger);
  inverter.begin((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, (gpio_num_t)CAN_SE);

  currentData.packVoltage = 52.5f;
  currentData.packCurrent = 0.0f;
  currentData.packSOC = 50.0f;
  currentData.packTemp = 220;
  currentData.minCellVoltage = 3.3f;
  currentData.maxCellVoltage = 3.3f;
  currentData.smaChargeMode = "Unknown";

  xTaskCreatePinnedToCore(bmsTask, "BMS_Task", 4096, NULL, 1, NULL, 0);

  netLog("[SYS] Boot sequence complete. Multithreading Active.\n");
}

void loop()
{
  ArduinoOTA.handle();

  static unsigned long lastCanCheck = 0;
  if (millis() - lastCanCheck > 50)
  {
    lastCanCheck = millis();
    inverter.checkBusHealth();
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      inverter.readMessages(currentData);
      xSemaphoreGive(dataMutex);
    }
  }

  if (isResetting && (millis() - resetHoldStartTime > 5500))
  {
    isResetting = false;
    netLog("[SYS] Recovery cycle finished.\n");
  }

  if (millis() - lastSmaTx > 250)
  {
    lastSmaTx = millis();

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (!autoMaint && currentData.packVoltage > 0 && currentData.packVoltage < (cfg.cvMaintStart * CELL_COUNT))
        autoMaint = true;
      else if (autoMaint && currentData.packVoltage > (cfg.cvMaintStop * CELL_COUNT))
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
      tx.dcl = calculateDCL(currentData.minCellVoltage);
      tx.cvl = currentData.maintenanceActive ? 560 : (uint16_t)(cfg.cvMaxCharge * CELL_COUNT * 10);
      tx.dvl = (uint16_t)(cfg.cvMinDischarge * CELL_COUNT * 10);

      inverter.sendStatus(tx);
      xSemaphoreGive(dataMutex);
    }
  }
}