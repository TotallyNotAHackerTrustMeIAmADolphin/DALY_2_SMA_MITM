#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>
#include "driver/twai.h"
#include "pin_config.h"
#include "mcp2515.h"
#include <SPI.h>
#include <deque>

// --- NETWORK CONFIGURATION ---
const char* ssid = "wlesswg";
const char* password = "hba.1245";
IPAddress local_IP(192, 168, 178, 55);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);

// --- POWER LIMITS (Adjust for your specific BMS/Cables) ---
const float MAX_CHARGE_LIMIT_A    = 250.0; 
const float MAX_DISCHARGE_LIMIT_A = 500.0; 
const float BALANCING_A           = 1.0;   // Drops to 1A if a cell "runs away"

// --- VOLTAGE THRESHOLDS ---
const float V_MAX_CHARGE     = 55.20; 
const float V_START_C_TAPER  = 54.00; 
const float V_START_D_TAPER  = 49.60; 
const float V_MIN_DISCHARGE  = 48.00; 
const float V_ALARM_GATE     = 53.50; // Don't trigger cell alarms below this voltage

// --- STATE STORAGE ---
float packVoltage = 52.6; 
float packCurrent = 0.0;
uint16_t packSOC = 50;
uint16_t packSOH = 100;
bool cellHighAlarm = false; 

// Voltage Filtering (Moving Average to stop flicker)
std::deque<float> vHistory;
const int V_SAMPLES = 10; 

unsigned long lastSmaTx = 0;
unsigned long lastBmsRx = 0;

MCP2515 Can_SMA(MCP2515_CS, 10000000, &SPI);

void netLog(const char* format, ...) {
    char loc_res[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_res, sizeof(loc_res), format, arg);
    va_end(arg);
    TelnetStream.print(loc_res);
}

// Filtered Voltage Calculation
float getFilteredVoltage(float newV) {
    vHistory.push_back(newV);
    if (vHistory.size() > V_SAMPLES) vHistory.pop_front();
    float sum = 0;
    for (float v : vHistory) sum += v;
    return sum / vHistory.size();
}

// Logic: Charge Current Limit
uint16_t calculateCCL(float v) {
    // Cell Runner Protection: Only engage if near full and BMS reports high cell
    if (cellHighAlarm && v > V_ALARM_GATE) return (uint16_t)(BALANCING_A * 10);
    
    if (v >= V_MAX_CHARGE) return 0;
    if (v < V_START_C_TAPER) return (uint16_t)(MAX_CHARGE_LIMIT_A * 10);
    
    float ratio = (V_MAX_CHARGE - v) / (V_MAX_CHARGE - V_START_C_TAPER);
    float target = ratio * MAX_CHARGE_LIMIT_A;
    return (uint16_t)(max(target, 2.0f) * 10);
}

// Logic: Discharge Current Limit
uint16_t calculateDCL(float v) {
    if (v <= V_MIN_DISCHARGE) return 0;
    if (v > V_START_D_TAPER) return (uint16_t)(MAX_DISCHARGE_LIMIT_A * 10);
    
    float ratio = (v - V_MIN_DISCHARGE) / (V_START_D_TAPER - V_MIN_DISCHARGE);
    float target = ratio * MAX_DISCHARGE_LIMIT_A;
    return (uint16_t)(max(target, 10.0f) * 10);
}

// SMA Heartbeat Function (Timed)
void sendToSma() {
    struct can_frame f;
    uint16_t ccl = calculateCCL(packVoltage);
    uint16_t dcl = calculateDCL(packVoltage);

    // 0x351: Limits
    f.can_id = 0x351; f.can_dlc = 8;
    f.data[0] = 0x58; f.data[1] = 0x02; // 58.4V Request
    f.data[2] = lowByte(ccl); f.data[3] = highByte(ccl);
    f.data[4] = lowByte(dcl); f.data[5] = highByte(dcl);
    f.data[6] = 0x40; f.data[7] = 0x01; // 44.8V Request
    Can_SMA.sendMessage(&f);

    // 0x355: SOC / SOH
    f.can_id = 0x355; f.can_dlc = 4;
    f.data[0] = lowByte(packSOC); f.data[1] = highByte(packSOC);
    f.data[2] = lowByte(packSOH); f.data[3] = highByte(packSOH);
    Can_SMA.sendMessage(&f);

    // 0x356: Voltage / Current
    uint16_t v = (uint16_t)(packVoltage * 100);
    int16_t i = (int16_t)(packCurrent * 10.0);
    f.can_id = 0x356; f.can_dlc = 6;
    f.data[0] = lowByte(v); f.data[1] = highByte(v);
    f.data[2] = lowByte(i); f.data[3] = highByte(i);
    f.data[4] = 0x00; f.data[5] = 0x00; 
    Can_SMA.sendMessage(&f);

    // Alarms, Heartbeat & Handshake
    f.can_id = 0x359; f.can_dlc = 7; memset(f.data, 0, 7); Can_SMA.sendMessage(&f);
    f.can_id = 0x35A; f.can_dlc = 8; memset(f.data, 0, 8); Can_SMA.sendMessage(&f);
    f.can_id = 0x35E; f.can_dlc = 8; memcpy(f.data, "SMA     ", 8); Can_SMA.sendMessage(&f);
}

void setup() {
    Serial.begin(115200);

    // 1. WiFi & Fixed IP
    WiFi.config(local_IP, gateway, subnet);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(100);
    ArduinoOTA.setHostname("SMA-BMS-Bridge");
    ArduinoOTA.begin();
    TelnetStream.begin();

    // 2. Init Port B (Daly)
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_driver_install(&g_config, &t_config, &f_config);
    twai_start();

    // 3. Init Port A (SMA)
    pinMode(MCP2515_RST, OUTPUT);
    digitalWrite(MCP2515_RST, HIGH); delay(50);
    digitalWrite(MCP2515_RST, LOW); delay(50);
    digitalWrite(MCP2515_RST, HIGH);
    SPI.begin(MCP2515_SCLK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);
    Can_SMA.reset();
    Can_SMA.setBitrate(CAN_500KBPS);
    Can_SMA.setNormalMode();

    netLog("\n[BRIDGE] PRO-EDITION LOADED. IP: 192.168.178.55\n");
}

void loop() {
    ArduinoOTA.handle();

    // --- CAPTURE DALY DATA ---
    twai_message_t b_msg;
    if (twai_receive(&b_msg, 0) == ESP_OK) {
        lastBmsRx = millis();
        if (b_msg.identifier == 0x356) {
            uint16_t v_raw = (b_msg.data[1] << 8) | b_msg.data[0];
            packVoltage = getFilteredVoltage(v_raw / 100.0);
            int16_t i_raw = (int16_t)((b_msg.data[3] << 8) | b_msg.data[2]);
            packCurrent = i_raw / 10.0;
        }
        if (b_msg.identifier == 0x355) {
            uint16_t incomingSOC = (b_msg.data[1] << 8) | b_msg.data[0];
            if (incomingSOC <= 100) packSOC = incomingSOC;
        }
        if (b_msg.identifier == 0x35A || b_msg.identifier == 0x359) {
            // Refined: Only bit 2 (0x04) is "Cell Over Voltage"
            cellHighAlarm = (b_msg.data[0] & 0x04) || (b_msg.data[2] & 0x04);
        }
    }

    // --- FORWARD SMA CLUSTER SYNC ---
    struct can_frame in_frame;
    if (Can_SMA.readMessage(&in_frame) == MCP2515::ERROR_OK) {
        twai_message_t back_msg;
        back_msg.identifier = in_frame.can_id;
        back_msg.data_length_code = in_frame.can_dlc;
        memcpy(back_msg.data, in_frame.data, 8);
        twai_transmit(&back_msg, 0);
    }

    // --- TIMED SMA TX (Every 250ms) ---
    if (millis() - lastSmaTx > 250) {
        sendToSma();
        lastSmaTx = millis();
        
        static unsigned long lastDash = 0;
        if (millis() - lastDash > 2000) {
            netLog("\n--- SMA-DALY PRO DASHBOARD ---\n");
            netLog("BATT: %.2fV | %.1fA | %d%% SOC\n", packVoltage, packCurrent, packSOC);
            netLog("LIMITS: CCL: %.1fA | DCL: %.1fA\n", calculateCCL(packVoltage)/10.0, calculateDCL(packVoltage)/10.0);
            if (cellHighAlarm && packVoltage > V_ALARM_GATE) 
                netLog("ALARM: [CELL RUNNER! Forced Balance at 1.0A]\n");
            else if (packVoltage > V_START_C_TAPER) 
                netLog("STATUS: [Charge Tapering active]\n");
            else 
                netLog("STATUS: [Normal Operation]\n");
            netLog("------------------------------\n");
            lastDash = millis();
        }
    }

    // Safety: Emergency Cutoff
    if (millis() - lastBmsRx > 15000 && lastBmsRx != 0) {
        packVoltage = V_MAX_CHARGE; // Stop charging
        netLog("[WARN] DALY BMS TIMEOUT - CUTTING CHARGE\n");
    }
}