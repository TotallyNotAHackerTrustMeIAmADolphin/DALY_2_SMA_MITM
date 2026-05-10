#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>

#include "pin_config.h"
#include "SystemState.h"
#include "DalyRS485.h"
#include "SMA_CAN.h"
#include "WebDashboard.h"

// --- CONFIG & NETWORK ---
const char *ssid = "wlesswg";
const char *password = "hba.1245";
IPAddress local_IP(192, 168, 178, 56);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);

#define CELL_COUNT 16.0f

// --- GLOBAL INSTANCES ---
DalyRS485 bms(Serial2);
SMA_CAN inverter;
WebDashboard webUI(80);

SystemConfig cfg;
DashboardData currentData;

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
  Serial.print(loc_res);
  TelnetStream.print(loc_res);
  webUI.broadcastLog(loc_res);
}

void libraryLogger(const char *msg) { netLog("%s", msg); }

// --- GLIDESLOPE LOGIC (Upgraded to Min/Max Cell math!) ---
uint16_t calculateCCL(float maxCellV)
{
  if (currentData.maintenanceActive)
    return (uint16_t)(cfg.maintAmps * 10);

  if (maxCellV >= cfg.cvMaxCharge)
    return 0; // Hard wall for the highest cell
  if (maxCellV >= cfg.cvHighAlarmGate)
    return (uint16_t)(cfg.trickleA * 10);

  if (maxCellV > cfg.cvStartTaper)
  {
    float slope = (cfg.cvHighAlarmGate - maxCellV) / (cfg.cvHighAlarmGate - cfg.cvStartTaper);
    float target = cfg.trickleA + (slope * (cfg.maxChargeA - cfg.trickleA));
    return (uint16_t)(max(target, cfg.trickleA) * 10);
  }
  return (uint16_t)(cfg.maxChargeA * 10);
}

uint16_t calculateDCL(float minCellV)
{
  if (currentData.maintenanceActive)
    return 0;

  if (minCellV <= cfg.cvMinDischarge)
    return 0; // Hard wall for the lowest cell
  if (minCellV <= cfg.cvLowAlarmGate)
    return (uint16_t)(cfg.limpDischargeA * 10);

  if (minCellV < cfg.cvStartDTaper)
  {
    float slope = (minCellV - cfg.cvLowAlarmGate) / (cfg.cvStartDTaper - cfg.cvLowAlarmGate);
    float target = cfg.limpDischargeA + (slope * (cfg.maxDischargeA - cfg.limpDischargeA));
    return (uint16_t)(max(target, cfg.limpDischargeA) * 10);
  }
  return (uint16_t)(cfg.maxDischargeA * 10);
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

void setup()
{
  Serial.begin(115200);
  Serial.println("\nStarting LilyGO T-CAN485 BMS Bridge...");

  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED && millis() < 10000)
    delay(100);

  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("BMS-Bridge");
  ArduinoOTA.begin();

  TelnetStream.begin();

  webUI.setActionCallback(handleUIAction);
  webUI.begin(cfg);

  bms.setDebugCallback(libraryLogger);
  bms.begin(RS485_RX, RS485_TX, RS485_SE, RS485_EN, PIN_5V_EN);

  inverter.setDebugCallback(libraryLogger);
  inverter.begin((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX);

  // Initial default states
  currentData.smaChargeMode = "Unknown";
  currentData.smaErrorCode = 0;
  currentData.packTemp = 220; // Default 22.0 C
  currentData.minCellVoltage = 3.3f;
  currentData.maxCellVoltage = 3.3f;
}

void loop()
{
  ArduinoOTA.handle();
  inverter.checkBusHealth();
  inverter.readMessages(currentData);

  // Handle Reset Timer
  if (isResetting && (millis() - resetHoldStartTime > 5500))
  {
    isResetting = false;
    netLog("[SYS] Recovery cycle finished.\n");
  }

  // SMA CAN Transmission (Every 250ms)
  if (millis() - lastSmaTx > 250)
  {
    lastSmaTx = millis();

    // Evaluate Winter Maintenance Trigger (Still uses Pack Voltage to avoid false triggering on a single dipping cell under load)
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

    // Feed the Min/Max cell outliers into the glideslope limits
    tx.ccl = calculateCCL(currentData.maxCellVoltage);
    tx.dcl = calculateDCL(currentData.minCellVoltage);
    tx.cvl = currentData.maintenanceActive ? 560 : (uint16_t)(cfg.cvMaxCharge * CELL_COUNT * 10);
    tx.dvl = (uint16_t)(cfg.cvMinDischarge * CELL_COUNT * 10);

    inverter.sendStatus(tx);
  }

  // RS485 Daly Polling (Every 2 seconds)
  if (millis() - lastBmsPoll > 2000)
  {
    lastBmsPoll = millis();

    DalyBasicInfo info;
    if (bms.readBasicInfo(info))
    {
      currentData.packVoltage = info.packVoltage;
      currentData.packCurrent = info.packCurrent;
      currentData.packSOC = (int)info.packSOC;
    }

    std::vector<float> cellVolts;
    if (bms.readCellVoltages(CELL_COUNT, cellVolts))
    {
      float sum = 0;
      float localMin = 10.0f;
      float localMax = 0.0f;

      for (float v : cellVolts)
      {
        sum += v;
        if (v < localMin)
          localMin = v;
        if (v > localMax)
          localMax = v;
      }

      currentData.avgCellVoltage = sum / CELL_COUNT;
      currentData.minCellVoltage = localMin;
      currentData.maxCellVoltage = localMax;
      currentData.cellVoltages = cellVolts; // Save to struct for Dashboard

      // Push combined telemetry to the Web UI
      webUI.broadcastTelemetry(currentData);
    }
  }
}