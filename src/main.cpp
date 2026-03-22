#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>
#include "driver/twai.h"
#include "pin_config.h"
#include "mcp2515.h"
#include <SPI.h>

// --- NETWORK CONFIGURATION ---
const char* ssid = "wlesswg";
const char* password = "hba.1245";
IPAddress local_IP(192, 168, 178, 55);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);

// --- SEPARATE HIGH-POWER CAPS ---
const float MAX_CHARGE_LIMIT_A    = 250.0; // Your Charge Cap
const float MAX_DISCHARGE_LIMIT_A = 500.0; // Your Discharge Cap (High Power)

// --- TAPER THRESHOLDS ---
const float V_MAX_CHARGE     = 55.20; 
const float V_START_C_TAPER  = 54.00; 
const float V_START_D_TAPER  = 49.60; 
const float V_MIN_DISCHARGE  = 48.00; 

// --- STATE STORAGE ---
float packVoltage = 52.6; 
float packCurrent = 0.0; // Real-time Amps from BMS
uint16_t packSOC = 50;
uint16_t packSOH = 100;
uint16_t lastDalyCCL = 0; // What the Daly is requesting (Raw)
uint16_t lastDalyDCL = 0; // What the Daly is requesting (Raw)

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

// --- CALCULATION LOGIC ---
uint16_t calculateCCL(float v) {
    if (v >= V_MAX_CHARGE) return 0;
    if (v < V_START_C_TAPER) return (uint16_t)(MAX_CHARGE_LIMIT_A * 10);
    float ratio = (V_MAX_CHARGE - v) / (V_MAX_CHARGE - V_START_C_TAPER);
    float target = ratio * MAX_CHARGE_LIMIT_A;
    return (uint16_t)(max(target, 2.0f) * 10);
}

uint16_t calculateDCL(float v) {
    if (v <= V_MIN_DISCHARGE) return 0;
    if (v > V_START_D_TAPER) return (uint16_t)(MAX_DISCHARGE_LIMIT_A * 10);
    float ratio = (v - V_MIN_DISCHARGE) / (V_START_D_TAPER - V_MIN_DISCHARGE);
    float target = ratio * MAX_DISCHARGE_LIMIT_A;
    return (uint16_t)(max(target, 10.0f) * 10);
}

// --- SMA HEARTBEAT ENGINE ---
void sendToSma() {
    struct can_frame f;
    uint16_t bridgeCCL = calculateCCL(packVoltage);
    uint16_t bridgeDCL = calculateDCL(packVoltage);

    // 0x351: Limits
    f.can_id = 0x351; f.can_dlc = 8;
    f.data[0] = 0x58; f.data[1] = 0x02; // 58.4V
    f.data[2] = lowByte(bridgeCCL); f.data[3] = highByte(bridgeCCL);
    f.data[4] = lowByte(bridgeDCL); f.data[5] = highByte(bridgeDCL);
    f.data[6] = 0x40; f.data[7] = 0x01; // 44.8V
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

    // Alarms & Handshake
    f.can_id = 0x359; f.can_dlc = 7; memset(f.data, 0, 7); Can_SMA.sendMessage(&f);
    f.can_id = 0x35A; f.can_dlc = 8; memset(f.data, 0, 8); Can_SMA.sendMessage(&f);
    f.can_id = 0x35E; f.can_dlc = 8; memcpy(f.data, "SMA     ", 8); Can_SMA.sendMessage(&f);
}

void setup() {
    WiFi.config(local_IP, gateway, subnet);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(100);
    ArduinoOTA.begin();
    TelnetStream.begin();

    // CAN B (Daly)
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_driver_install(&g_config, &t_config, &f_config);
    twai_start();

    // CAN A (SMA)
    pinMode(MCP2515_RST, OUTPUT);
    digitalWrite(MCP2515_RST, HIGH); delay(50);
    digitalWrite(MCP2515_RST, LOW); delay(50);
    digitalWrite(MCP2515_RST, HIGH);
    SPI.begin(MCP2515_SCLK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);
    Can_SMA.reset();
    Can_SMA.setBitrate(CAN_500KBPS);
    Can_SMA.setNormalMode();

    netLog("\n[BRIDGE] HIGH-POWER VERBOSE MODE LOADED.\n");
}

void loop() {
    ArduinoOTA.handle();

    // --- CAPTURE DALY DATA ---
    twai_message_t b_msg;
    if (twai_receive(&b_msg, 0) == ESP_OK) {
        lastBmsRx = millis();
        if (b_msg.identifier == 0x356) {
            uint16_t v_raw = (b_msg.data[1] << 8) | b_msg.data[0];
            packVoltage = v_raw / 100.0;
            int16_t i_raw = (int16_t)((b_msg.data[3] << 8) | b_msg.data[2]);
            packCurrent = i_raw / 10.0;
        }
        if (b_msg.identifier == 0x355) {
            packSOC = (b_msg.data[1] << 8) | b_msg.data[0];
        }
        if (b_msg.identifier == 0x351) {
            lastDalyCCL = (b_msg.data[3] << 8) | b_msg.data[2];
            lastDalyDCL = (b_msg.data[5] << 8) | b_msg.data[4];
        }
    }

    // --- FORWARD SMA SYNC ---
    struct can_frame in_frame;
    if (Can_SMA.readMessage(&in_frame) == MCP2515::ERROR_OK) {
        twai_message_t back_msg;
        back_msg.identifier = in_frame.can_id;
        back_msg.data_length_code = in_frame.can_dlc;
        memcpy(back_msg.data, in_frame.data, 8);
        twai_transmit(&back_msg, 0);
    }

    // --- SMA HEARTBEAT & DASHBOARD (Every 1s for log, 200ms for Tx) ---
    if (millis() - lastSmaTx > 200) {
        sendToSma();
        lastSmaTx = millis();
        
        static unsigned long lastDashboard = 0;
        if (millis() - lastDashboard > 2000) { // Update Dashboard every 2s
            netLog("\n--- BMS BRIDGE DASHBOARD ---\n");
            netLog("BATTERY: %.2fV | %.1fA | %d%% SOC\n", packVoltage, packCurrent, packSOC);
            netLog("CHARGE : Daly Req: %dA | Bridge Allowed: %.1fA ", lastDalyCCL/10, calculateCCL(packVoltage)/10.0);
            if (packVoltage > V_START_C_TAPER) netLog("[Tapering!]");
            netLog("\nDISCHRG: Daly Req: %dA | Bridge Allowed: %.1fA ", lastDalyDCL/10, calculateDCL(packVoltage)/10.0);
            if (packVoltage < V_START_D_TAPER) netLog("[Tapering!]");
            netLog("\n----------------------------\n");
            lastDashboard = millis();
        }
    }

    // Safety: Emergency Cutoff
    if (millis() - lastBmsRx > 10000 && lastBmsRx != 0) {
        packVoltage = 55.2; // Force logic stop
        netLog("[WARN] DALY BMS TIMEOUT - CUTTING POWER\n");
    }
}