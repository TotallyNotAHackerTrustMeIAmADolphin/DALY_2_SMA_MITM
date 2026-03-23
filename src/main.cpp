#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include "driver/twai.h"
#include "pin_config.h"
#include "mcp2515.h"
#include <SPI.h>
#include <deque>
#include <esp_task_wdt.h>

// --- SETTINGS & NETWORK ---
const char* ssid = "wlesswg";
const char* password = "hba.1245";
IPAddress local_IP(192, 168, 178, 55);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);

#define WDT_TIMEOUT_SECONDS 15

struct Config {
    float maxChargeA;       float maxDischargeA;    float vStartTaper;      
    float vMaxCharge;       float vStartDTaper;     float vMinDischarge;    
    float vHighAlarmGate;   float vLowAlarmGate;    float trickleA;         
    float limpDischargeA;   int   vSamples;         int   bmsTimeout;       
    int   socStartTaper;    int   socStartDTaper;
} cfg;

// --- STATE ---
float packVoltage = 52.6; float packCurrent = 0.0; uint16_t packSOC = 50;
bool cellHighAlarm = false; bool cellLowAlarm = false;
unsigned long lastSmaTx = 0; unsigned long lastBmsRx = 0;
std::deque<float> vHistory;
String smaChargeMode = "Unknown"; bool gridPresent = false;
uint32_t bootCount = 0;

MCP2515 Can_SMA(MCP2515_CS, 10000000, &SPI);
AsyncWebServer server(80);
AsyncEventSource events("/events");
WiFiServer logServer(2323); WiFiClient logClient;
Preferences prefs;

// --- UTILITIES ---
void netLog(const char* format, ...) {
    char loc_res[256]; va_list arg; va_start(arg, format);
    vsnprintf(loc_res, sizeof(loc_res), format, arg); va_end(arg);
    Serial.print(loc_res); if (logClient && logClient.connected()) logClient.print(loc_res);
    events.send(loc_res, "log", millis());
}

void loadConfig() {
    prefs.begin("bms-bridge", false);
    bootCount = prefs.getUInt("boots", 0);
    prefs.putUInt("boots", ++bootCount); 
    cfg.maxChargeA = prefs.getFloat("ca", 250.0);
    cfg.maxDischargeA = prefs.getFloat("da", 500.0);
    cfg.vStartTaper = prefs.getFloat("vt", 54.00);
    cfg.vMaxCharge = prefs.getFloat("mv", 55.20);
    cfg.vStartDTaper = prefs.getFloat("dvt", 49.60);
    cfg.vMinDischarge = prefs.getFloat("mdv", 48.00);
    cfg.vHighAlarmGate = prefs.getFloat("ag", 53.50);
    cfg.vLowAlarmGate = prefs.getFloat("lag", 50.00);
    cfg.trickleA = prefs.getFloat("ta", 2.0); 
    cfg.limpDischargeA = prefs.getFloat("ld_v2", 5.0); // Renamed Key to fix visibility
    cfg.vSamples = prefs.getInt("vs", 10);
    cfg.bmsTimeout = prefs.getInt("to", 15);
    cfg.socStartTaper = prefs.getInt("st", 95); 
    cfg.socStartDTaper = prefs.getInt("sdt", 15); 
    prefs.end();
}

// --- LOGIC ---
float getFilteredVoltage(float newV) {
    if (newV < 30.0 || newV > 70.0) return packVoltage;
    vHistory.push_back(newV); while (vHistory.size() > cfg.vSamples) vHistory.pop_front();
    float sum = 0; for (float v : vHistory) sum += v; return sum / (float)vHistory.size();
}

uint16_t calculateCCL(float v, int soc) {
    if (millis() - lastBmsRx > (cfg.bmsTimeout * 1000)) return 0;
    if (v >= cfg.vMaxCharge) return 0;
    if ((cellHighAlarm && v > cfg.vHighAlarmGate) || (soc >= 100)) return (uint16_t)(cfg.trickleA * 10);
    float vRatio = (v > cfg.vStartTaper) ? (cfg.vMaxCharge - v) / (cfg.vMaxCharge - cfg.vStartTaper) : 1.0;
    float socRatio = (soc > cfg.socStartTaper) ? (100.0 - (float)soc) / (100.0 - (float)cfg.socStartTaper) : 1.0;
    return (uint16_t)(max(min(vRatio, socRatio) * cfg.maxChargeA, cfg.trickleA) * 10);
}

uint16_t calculateDCL(float v, int soc) {
    if (millis() - lastBmsRx > (cfg.bmsTimeout * 1000)) return 0;
    if (v <= cfg.vMinDischarge || soc <= 0) return 0;
    if (cellLowAlarm && v < cfg.vLowAlarmGate) return (uint16_t)(cfg.limpDischargeA * 10);
    float vRatio = (v < cfg.vStartDTaper) ? (v - cfg.vMinDischarge) / (cfg.vStartDTaper - cfg.vMinDischarge) : 1.0;
    float socRatio = (soc < cfg.socStartDTaper) ? (float)soc / (float)cfg.socStartDTaper : 1.0;
    return (uint16_t)(max(min(vRatio, socRatio) * cfg.maxDischargeA, cfg.limpDischargeA) * 10);
}

// --- HTML: DASHBOARD ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>BMS Bridge Pro</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; text-align: center; background: #121212; color: #e0e0e0; margin: 0; }
  .nav { background: #1e1e1e; padding: 10px; border-bottom: 2px solid #333; margin-bottom: 10px; }
  .nav a { color: #4caf50; text-decoration: none; margin: 0 15px; font-weight: bold; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; padding: 15px; }
  .card { background: #1e1e1e; padding: 15px; border-radius: 10px; border: 1px solid #333; }
  .value { font-size: 2em; font-weight: bold; color: #4caf50; }
  #console { width: 95%; max-width: 1000px; height: 350px; margin: 15px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 15px; overflow-y: scroll; border-radius: 8px; font-size: 0.85em; border: 1px solid #444; }
</style></head><body>
<div class="nav"><a href="/">DASHBOARD</a> | <a href="/config">CONFIGURATION</a></div>
<div class="grid">
  <div class="card"><div>Voltage</div><div id="v" class="value">--</div></div>
  <div class="card"><div>Current</div><div id="i" class="value">--</div></div>
  <div class="card"><div>SOC</div><div id="soc" class="value">--</div></div>
  <div class="card"><div>SMA Mode</div><div id="smam" class="value">--</div></div>
</div>
<div id="console">Log Active...<br></div>
<script>
  var source = new EventSource('/events');
  source.addEventListener('data', function(e) {
    var obj = JSON.parse(e.data);
    document.getElementById('v').innerHTML = obj.v.toFixed(2) + " V";
    document.getElementById('i').innerHTML = obj.i.toFixed(1) + " A";
    document.getElementById('soc').innerHTML = obj.soc + "%";
    document.getElementById('smam').innerHTML = obj.smam;
  }, false);
  source.addEventListener('log', function(e) {
    const con = document.getElementById('console');
    con.innerHTML += e.data + "<br>";
    if(con.childNodes.length > 100) con.removeChild(con.firstChild);
    con.scrollTop = con.scrollHeight;
  }, false);
</script></body></html>)rawliteral";

// --- HTML: CONFIG ---
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>Settings</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #121212; color: #eee; padding: 10px; }
  .container { max-width: 650px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; }
  .row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; border-bottom: 1px solid #333; padding-bottom: 10px; }
  .text-group { text-align: left; padding-right: 15px; }
  .desc { font-size: 0.8em; color: #999; display: block; margin-top: 3px; }
  input { font-size: 1.2em; padding: 5px; width: 110px; text-align: center; background: #000; color: #0f0; border: 1px solid #555; border-radius: 4px; }
  .save { background: #2e7d32; color: white; border: none; padding: 15px; width: 100%; border-radius: 5px; font-weight: bold; cursor: pointer; font-size: 1.1em; }
</style></head><body>
<div class="container">
  <a href="/" style="color:#4caf50;text-decoration:none;">&larr; Back to Dashboard</a>
  <h2>Protection & Tapering</h2>
  <form action="/save" method="GET">
    <div class="row"><div class="text-group"><strong>SOC Start Taper (C) %</strong><span class="desc">Lower charge current gradually above this level.</span></div>
      <input type="number" name="st" step="1" value="!!VAL_ST!!"></div>
    <div class="row"><div class="text-group"><strong>SOC Start Taper (D) %</strong><span class="desc">Lower discharge current gradually below this level.</span></div>
      <input type="number" name="sdt" step="1" value="!!VAL_SDT!!"></div>
    <div class="row"><div class="text-group"><strong>Trickle Charge (A)</strong><span class="desc">Current for balancing and finishing charge.</span></div>
      <input type="number" name="ta" step="1" value="!!VAL_TA!!"></div>
    <div class="row"><div class="text-group"><strong>Max Charge Amps (A)</strong><span class="desc">Global bulk charging limit.</span></div>
      <input type="number" name="ca" step="10" value="!!VAL_CA!!"></div>
    <div class="row"><div class="text-group"><strong>Max Discharge Amps (A)</strong><span class="desc">Peak household load limit.</span></div>
      <input type="number" name="da" step="10" value="!!VAL_DA!!"></div>
    <div class="row"><div class="text-group"><strong>Charge Stop Volts (V)</strong><span class="desc">Target finish voltage (Hard cutoff).</span></div>
      <input type="number" name="mv" step="0.1" value="!!VAL_MV!!"></div>
    <div class="row"><div class="text-group"><strong>Discharge Cutoff (V)</strong><span class="desc">Absolute minimum battery voltage.</span></div>
      <input type="number" name="mdv" step="0.1" value="!!VAL_MDV!!"></div>
    <div class="row"><div class="text-group"><strong>Limp Discharge (A)</strong><span class="desc">Fixed limit when a low cell is detected.</span></div>
      <input type="number" name="ld" step="1" value="!!VAL_LIMP!!"></div>
    <div class="row"><div class="text-group"><strong>High Cell Gate (V)</strong><span class="desc">Enable Trickle logic above this voltage.</span></div>
      <input type="number" name="ag" step="0.1" value="!!VAL_AG!!"></div>
    <div class="row"><div class="text-group"><strong>Low Cell Gate (V)</strong><span class="desc">Enable Limp logic below this voltage.</span></div>
      <input type="number" name="lag" step="0.1" value="!!VAL_LAG!!"></div>
    <div class="row"><div class="text-group"><strong>Voltage Samples</strong><span class="desc">Smoothing window (1-20).</span></div>
      <input type="number" name="vs" step="1" value="!!VAL_VS!!"></div>
    <button type="submit" class="save">SAVE & APPLY ALL CHANGES</button>
  </form>
</div></body></html>)rawliteral";

void sendToSma() {
    struct can_frame f; uint16_t ccl = calculateCCL(packVoltage, packSOC); uint16_t dcl = calculateDCL(packVoltage, packSOC);
    f.can_id = 0x351; f.can_dlc = 8; f.data[0] = 0x58; f.data[1] = 0x02; f.data[2] = lowByte(ccl); f.data[3] = highByte(ccl); f.data[4] = lowByte(dcl); f.data[5] = highByte(dcl); f.data[6] = 0x40; f.data[7] = 0x01; Can_SMA.sendMessage(&f);
    f.can_id = 0x355; f.can_dlc = 4; f.data[0] = lowByte(packSOC); f.data[1] = highByte(packSOC); f.data[2] = 100; f.data[3] = 0; Can_SMA.sendMessage(&f);
    f.can_id = 0x356; f.can_dlc = 6; uint16_t v = (uint16_t)(packVoltage * 100); int16_t i = (int16_t)(packCurrent * 10.0); f.data[0] = lowByte(v); f.data[1] = highByte(v); f.data[2] = lowByte(i); f.data[3] = highByte(i); f.data[4] = 0; f.data[5] = 0; Can_SMA.sendMessage(&f);
    f.can_id = 0x35E; f.can_dlc = 8; memcpy(f.data, "SMA     ", 8); Can_SMA.sendMessage(&f);
}

void setup() {
    Serial.begin(115200);
    loadConfig(); 

    // Watchdog
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
    esp_task_wdt_add(NULL);

    WiFi.config(local_IP, gateway, subnet); WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED && millis() < 10000) delay(100);
    
    ArduinoOTA.begin(); logServer.begin(); 
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send_P(200, "text/html", index_html); });
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
        String h = String(config_html);
        // Important: Replace LIMP first and use explicit integer conversion
        h.replace("!!VAL_LIMP!!", String((int)cfg.limpDischargeA));
        h.replace("!!VAL_LAG!!", String(cfg.vLowAlarmGate, 1));
        h.replace("!!VAL_AG!!", String(cfg.vHighAlarmGate, 1));
        h.replace("!!VAL_SDT!!", String(cfg.socStartDTaper));
        h.replace("!!VAL_ST!!", String(cfg.socStartTaper));
        h.replace("!!VAL_TA!!", String(cfg.trickleA, 0));
        h.replace("!!VAL_CA!!", String(cfg.maxChargeA, 0));
        h.replace("!!VAL_DA!!", String(cfg.maxDischargeA, 0));
        h.replace("!!VAL_MV!!", String(cfg.vMaxCharge, 1));
        h.replace("!!VAL_MDV!!", String(cfg.vMinDischarge, 1));
        h.replace("!!VAL_VS!!", String(cfg.vSamples));
        request->send(200, "text/html", h);
    });
    server.on("/save", HTTP_GET, [](AsyncWebServerRequest *request){
        prefs.begin("bms-bridge", false); String audit = "[AUDIT] Changes: "; bool changed = false;
        auto checkF = [&](String p, float &v, String k, String l) {
            if(request->hasParam(p)) {
                float n = request->getParam(p)->value().toFloat();
                if (abs(n - v) > 0.01) { audit += l + "(" + String(v,1) + "->" + String(n,1) + ") "; v = n; prefs.putFloat(k.c_str(), v); changed = true; }
            }
        };
        auto checkI = [&](String p, int &v, String k, String l) {
            if(request->hasParam(p)) {
                int n = request->getParam(p)->value().toInt();
                if (n != v) { audit += l + "(" + String(v) + "->" + String(n) + ") "; v = n; prefs.putInt(k.c_str(), v); changed = true; }
            }
        };
        checkI("st", cfg.socStartTaper, "st", "SOC_C_Tap"); checkI("sdt", cfg.socStartDTaper, "sdt", "SOC_D_Tap");
        checkF("ta", cfg.trickleA, "ta", "Trickle"); checkF("ca", cfg.maxChargeA, "ca", "Max_C");
        checkF("da", cfg.maxDischargeA, "da", "Max_D"); checkF("mv", cfg.vMaxCharge, "mv", "Stop_V");
        checkF("mdv", cfg.vMinDischarge, "mdv", "Cutoff_V"); checkF("ld", cfg.limpDischargeA, "ld_v2", "Limp_A");
        checkF("ag", cfg.vHighAlarmGate, "ag", "Hi_Gate"); checkF("lag", cfg.vLowAlarmGate, "lag", "Lo_Gate");
        checkI("vs", cfg.vSamples, "vs", "Samples");
        prefs.end(); if (changed) netLog("%s\n", audit.c_str());
        request->redirect("/");
    });
    server.addHandler(&events); server.begin();

    // CAN
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_driver_install(&g_config, &t_config, &f_config); twai_start();
    
    pinMode(MCP2515_RST, OUTPUT); digitalWrite(MCP2515_RST, HIGH); delay(50); digitalWrite(MCP2515_RST, LOW); delay(50); digitalWrite(MCP2515_RST, HIGH);
    SPI.begin(MCP2515_SCLK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);
    Can_SMA.reset(); Can_SMA.setBitrate(CAN_500KBPS); Can_SMA.setNormalMode();

    netLog("[SYSTEM] Fixed-Limp Started. Boot: %d, RAM: %d\n", bootCount, ESP.getFreeHeap());
}

void loop() {
    esp_task_wdt_reset(); 
    ArduinoOTA.handle();

    if (logServer.hasClient()) { if (logClient) logClient.stop(); logClient = logServer.available(); }

    twai_status_info_t twai_stat;
    twai_get_status_info(&twai_stat);
    if (twai_stat.state == TWAI_STATE_BUS_OFF) {
        netLog("[CRITICAL] CAN BUS OFF - Restarting...\n");
        twai_initiate_recovery();
    }

    twai_message_t b_msg;
    if (twai_receive(&b_msg, 0) == ESP_OK) {
        lastBmsRx = millis();
        if (b_msg.identifier == 0x356) {
            uint16_t v_raw = (b_msg.data[1] << 8) | b_msg.data[0];
            packVoltage = getFilteredVoltage(v_raw / 100.0);
            int16_t i_raw = (int16_t)((b_msg.data[3] << 8) | b_msg.data[2]);
            packCurrent = i_raw / 10.0;
        }
        if (b_msg.identifier == 0x355) packSOC = (b_msg.data[1] << 8) | b_msg.data[0];
        if (b_msg.identifier == 0x35A || b_msg.identifier == 0x359) {
            cellHighAlarm = (b_msg.data[0] & 0x04) || (b_msg.data[2] & 0x04);
            cellLowAlarm  = (b_msg.data[0] & 0x08) || (b_msg.data[2] & 0x08);
        }
    }

    struct can_frame in_frame;
    if (Can_SMA.readMessage(&in_frame) == MCP2515::ERROR_OK) {
        if (in_frame.can_id == 0x305) { uint8_t m = in_frame.data[0]; smaChargeMode = (m==1)?"Bulk":(m==2)?"Absorption":(m==3)?"Float":"Equalize"; }
        if (in_frame.can_id == 0x300) gridPresent = (in_frame.data[0] & 0x01);
        twai_message_t back_msg; back_msg.identifier = in_frame.can_id; back_msg.data_length_code = in_frame.can_dlc; memcpy(back_msg.data, in_frame.data, 8); twai_transmit(&back_msg, 0);
    }

    if (millis() - lastSmaTx > 250) {
        sendToSma(); lastSmaTx = millis();
        static unsigned long lastPush = 0;
        if (millis() - lastPush > 2000) {
            char json[256]; snprintf(json, sizeof(json), "{\"v\":%.2f,\"i\":%.1f,\"soc\":%d,\"smam\":\"%s\",\"smag\":%d}", packVoltage, packCurrent, packSOC, smaChargeMode.c_str(), gridPresent);
            events.send(json, "data", millis());
            
            String mode = "NORMAL";
            if (millis() - lastBmsRx > (cfg.bmsTimeout * 1000) && lastBmsRx != 0) mode = "BMS_OFFLINE";
            else if (cellHighAlarm && packVoltage > cfg.vHighAlarmGate) mode = "BALANCING";
            else if (cellLowAlarm && packVoltage < cfg.vLowAlarmGate) mode = "LOW_CELL_LIMP";
            else if (packVoltage >= cfg.vMaxCharge || packSOC >= 100) mode = "FULL/TRICKLE";
            else if (packVoltage > cfg.vStartTaper || packSOC > cfg.socStartTaper) mode = "TAPER_C";
            else if (packVoltage < cfg.vStartDTaper || packSOC < cfg.socStartDTaper) mode = "TAPER_D";
            
            netLog("[STATUS] Mode:%s V:%.2f I:%.1f SOC:%d%% CCL:%.1f DCL:%.1f RAM:%d\n", mode.c_str(), packVoltage, packCurrent, packSOC, calculateCCL(packVoltage, packSOC)/10.0, calculateDCL(packVoltage, packSOC)/10.0, smaChargeMode.c_str(), ESP.getFreeHeap());
            lastPush = millis();
        }
    }

    if (ESP.getFreeHeap() < 20000) { delay(1000); ESP.restart(); }
    // if (WiFi.status() != WL_CONNECTED && millis() > 60000) { delay(1000); ESP.restart(); }
}