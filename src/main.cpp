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

// --- NETWORK CONFIG ---
const char* ssid = "wlesswg";
const char* password = "hba.1245";
IPAddress local_IP(192, 168, 178, 55);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);

// --- GLOBAL SETTINGS (Loaded from Flash) ---
struct Config {
    float maxChargeA;       // Bulk charge current
    float maxDischargeA;    // Max house load current
    float vStartTaper;      // Voltage to start slowing down
    float vMaxCharge;       // Target finish voltage
    float vStartDTaper;     // Voltage to start slowing discharge
    float vMinDischarge;    // Hard cutoff voltage
    float vAlarmGate;       // Voltage above which Cell Alarms are active
    float balancingA;       // Current during cell runner alarm
    int   vSamples;         // Moving average window
    int   bmsTimeout;       // Seconds until safety cutoff
} cfg;

// --- STATE STORAGE ---
float packVoltage = 52.6; 
float packCurrent = 0.0;
uint16_t packSOC = 50;
bool cellHighAlarm = false; 
unsigned long lastSmaTx = 0;
unsigned long lastBmsRx = 0;

std::deque<float> vHistory;

MCP2515 Can_SMA(MCP2515_CS, 10000000, &SPI);
AsyncWebServer server(80);
AsyncEventSource events("/events");
WiFiServer logServer(2323); 
WiFiClient logClient;
Preferences prefs;

// --- HTML: DASHBOARD ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>BMS Bridge Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: sans-serif; text-align: center; background: #121212; color: #e0e0e0; margin: 0; }
    .nav { background: #1e1e1e; padding: 10px; border-bottom: 1px solid #333; }
    .nav a { color: #4caf50; text-decoration: none; margin: 0 15px; font-weight: bold; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 15px; padding: 20px; }
    .card { background: #1e1e1e; padding: 15px; border-radius: 12px; border: 1px solid #333; }
    .value { font-size: 1.8em; font-weight: bold; color: #4caf50; }
    #chart-container { width: 95%; max-width: 1000px; margin: 10px auto; background: #1e1e1e; padding: 15px; border-radius: 12px; border: 1px solid #333; }
    #console { width: 95%; max-width: 1000px; height: 120px; margin: 10px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 10px; overflow-y: scroll; border-radius: 8px; font-size: 0.85em; }
  </style>
</head><body>
  <div class="nav"><a href="/">Dashboard</a> | <a href="/config">Configuration</a></div>
  <h2>SYSTEM STATUS</h2>
  <div class="grid">
    <div class="card"><div>Voltage</div><div id="v" class="value">0.00</div></div>
    <div class="card"><div>Current</div><div id="i" class="value">0.0</div></div>
    <div class="card"><div>Power</div><div id="p" class="value">0.0</div></div>
    <div class="card"><div>SOC</div><div id="soc" class="value">0</div></div>
  </div>
  <div id="chart-container"><canvas id="liveChart"></canvas></div>
  <div id="console">Log: Standing by...</div>
  <script>
    const ctx = document.getElementById('liveChart').getContext('2d');
    const chart = new Chart(ctx, {
      type: 'line',
      data: { labels: [], datasets: [
        { label: 'Voltage', data: [], borderColor: '#f44336', yAxisID: 'yV', pointRadius: 0 },
        { label: 'Current', data: [], borderColor: '#4caf50', yAxisID: 'yA', pointRadius: 0 }
      ]},
      options: { animation: false, scales: { 
        yV: { type: 'linear', position: 'left', min: 46, max: 58 },
        yA: { type: 'linear', position: 'right', min: -550, max: 300 }
      }}
    });
    if (!!window.EventSource) {
      var source = new EventSource('/events');
      source.addEventListener('data', function(e) {
        var obj = JSON.parse(e.data);
        document.getElementById('v').innerHTML = obj.v.toFixed(2);
        document.getElementById('i').innerHTML = obj.i.toFixed(1);
        document.getElementById('soc').innerHTML = obj.soc;
        document.getElementById('p').innerHTML = ((obj.v * obj.i)/1000).toFixed(2);
        const now = new Date().toLocaleTimeString();
        if(chart.data.labels.length > 60) { chart.data.labels.shift(); chart.data.datasets.forEach(d => d.data.shift()); }
        chart.data.labels.push(now);
        chart.data.datasets[0].data.push(obj.v);
        chart.data.datasets[1].data.push(obj.i);
        chart.update();
      }, false);
      source.addEventListener('log', function(e) {
        const con = document.getElementById('console');
        con.innerHTML += e.data + "<br>";
        con.scrollTop = con.scrollHeight;
      }, false);
    }
  </script>
</body></html>)rawliteral";

// --- HTML: CONFIG PAGE ---
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>BMS Bridge Configuration</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: sans-serif; background: #121212; color: #e0e0e0; margin: 0; padding: 20px; }
    .container { max-width: 600px; margin: auto; background: #1e1e1e; padding: 20px; border-radius: 12px; border: 1px solid #333; }
    .row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; padding-bottom: 10px; border-bottom: 1px solid #2a2a2a; }
    .desc { font-size: 0.8em; color: #aaa; margin-top: 4px; display: block; }
    input { background: #333; color: white; border: 1px solid #555; padding: 8px; border-radius: 4px; width: 100px; }
    button { background: #2e7d32; color: white; border: none; padding: 15px; border-radius: 8px; width: 100%; cursor: pointer; font-weight: bold; margin-top: 20px; }
    .back { color: #4caf50; text-decoration: none; display: block; margin-bottom: 20px; }
  </style>
</head><body>
  <div class="container">
    <a href="/" class="back">&larr; Back to Dashboard</a>
    <h2>Configuration Settings</h2>
    <form action="/save" method="GET">
      <div class="row"><div><strong>Max Charge Amps</strong><span class="desc">Bulk charging limit (Safe hardware limit).</span></div><input name="ca" value="{ca}"></div>
      <div class="row"><div><strong>Max Discharge Amps</strong><span class="desc">Peak house load limit (Check fuses!).</span></div><input name="da" value="{da}"></div>
      <div class="row"><div><strong>Charge Start Taper (V)</strong><span class="desc">Voltage where current reduction begins.</span></div><input name="vt" value="{vt}"></div>
      <div class="row"><div><strong>Charge Stop Volts (V)</strong><span class="desc">Target full voltage (usually 55.2V).</span></div><input name="mv" value="{mv}"></div>
      <div class="row"><div><strong>Discharge Start Taper (V)</strong><span class="desc">Voltage to begin limiting house loads.</span></div><input name="dvt" value="{dvt}"></div>
      <div class="row"><div><strong>Discharge Cutoff (V)</strong><span class="desc">Hard stop for discharging (usually 48.0V).</span></div><input name="mdv" value="{mdv}"></div>
      <div class="row"><div><strong>Alarm Gate (V)</strong><span class="desc">Enable Cell Runner protection above this voltage.</span></div><input name="ag" value="{ag}"></div>
      <div class="row"><div><strong>Balancing Amps</strong><span class="desc">Current limit during high-cell runners.</span></div><input name="ba" value="{ba}"></div>
      <div class="row"><div><strong>Voltage Samples</strong><span class="desc">Moving average window size (1-20).</span></div><input name="vs" value="{vs}"></div>
      <div class="row"><div><strong>BMS Timeout (s)</strong><span class="desc">Safety cutoff if BMS signal is lost.</span></div><input name="to" value="{to}"></div>
      <button type="submit">Save & Apply Changes</button>
    </form>
  </div>
</body></html>)rawliteral";

// --- LOGGING ---
void netLog(const char* format, ...) {
    char loc_res[256];
    va_list arg; va_start(arg, format);
    vsnprintf(loc_res, sizeof(loc_res), format, arg);
    va_end(arg);
    Serial.print(loc_res);
    if (logClient && logClient.connected()) logClient.print(loc_res);
    events.send(loc_res, "log", millis());
}

// --- CONFIG PERSISTENCE ---
void loadConfig() {
    prefs.begin("bms-bridge", false);
    cfg.maxChargeA = prefs.getFloat("ca", 250.0);
    cfg.maxDischargeA = prefs.getFloat("da", 500.0);
    cfg.vStartTaper = prefs.getFloat("vt", 54.00);
    cfg.vMaxCharge = prefs.getFloat("mv", 55.20);
    cfg.vStartDTaper = prefs.getFloat("dvt", 49.60);
    cfg.vMinDischarge = prefs.getFloat("mdv", 48.00);
    cfg.vAlarmGate = prefs.getFloat("ag", 53.50);
    cfg.balancingA = prefs.getFloat("ba", 1.0);
    cfg.vSamples = prefs.getInt("vs", 10);
    cfg.bmsTimeout = prefs.getInt("to", 15);
    prefs.end();
}

// --- CALCULATION LOGIC ---
float getFilteredVoltage(float newV) {
    vHistory.push_back(newV);
    while (vHistory.size() > cfg.vSamples) vHistory.pop_front();
    float sum = 0; for (float v : vHistory) sum += v;
    return sum / vHistory.size();
}

uint16_t calculateCCL(float v) {
    if (cellHighAlarm && v > cfg.vAlarmGate) return (uint16_t)(cfg.balancingA * 10);
    if (v >= cfg.vMaxCharge) return 0;
    if (v < cfg.vStartTaper) return (uint16_t)(cfg.maxChargeA * 10);
    float ratio = (cfg.vMaxCharge - v) / (cfg.vMaxCharge - cfg.vStartTaper);
    return (uint16_t)(max(ratio * cfg.maxChargeA, 2.0f) * 10);
}

uint16_t calculateDCL(float v) {
    if (v <= cfg.vMinDischarge) return 0;
    if (v > cfg.vStartDTaper) return (uint16_t)(cfg.maxDischargeA * 10);
    float ratio = (v - cfg.vMinDischarge) / (cfg.vStartDTaper - cfg.vMinDischarge);
    return (uint16_t)(max(ratio * cfg.maxDischargeA, 10.0f) * 10);
}

void sendToSma() {
    struct can_frame f;
    uint16_t ccl = calculateCCL(packVoltage);
    uint16_t dcl = calculateDCL(packVoltage);
    f.can_id = 0x351; f.can_dlc = 8;
    f.data[0] = 0x58; f.data[1] = 0x02;
    f.data[2] = lowByte(ccl); f.data[3] = highByte(ccl);
    f.data[4] = lowByte(dcl); f.data[5] = highByte(dcl);
    f.data[6] = 0x40; f.data[7] = 0x01;
    Can_SMA.sendMessage(&f);
    f.can_id = 0x355; f.can_dlc = 4;
    f.data[0] = lowByte(packSOC); f.data[1] = highByte(packSOC);
    f.data[2] = 100; f.data[3] = 0;
    Can_SMA.sendMessage(&f);
    f.can_id = 0x356; f.can_dlc = 6;
    uint16_t v = (uint16_t)(packVoltage * 100);
    int16_t i = (int16_t)(packCurrent * 10.0);
    f.data[0] = lowByte(v); f.data[1] = highByte(v);
    f.data[2] = lowByte(i); f.data[3] = highByte(i);
    f.data[4] = 0; f.data[5] = 0;
    Can_SMA.sendMessage(&f);
    f.can_id = 0x35E; f.can_dlc = 8; memcpy(f.data, "SMA     ", 8); Can_SMA.sendMessage(&f);
}

void setup() {
    Serial.begin(115200);
    WiFi.config(local_IP, gateway, subnet);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(100);
    WiFi.setSleep(false);
    
    loadConfig();
    ArduinoOTA.begin();
    logServer.begin(); 

    // --- WEB HANDLERS ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = String(config_html);
        html.replace("{ca}", String(cfg.maxChargeA));
        html.replace("{da}", String(cfg.maxDischargeA));
        html.replace("{vt}", String(cfg.vStartTaper));
        html.replace("{mv}", String(cfg.vMaxCharge));
        html.replace("{dvt}", String(cfg.vStartDTaper));
        html.replace("{mdv}", String(cfg.vMinDischarge));
        html.replace("{ag}", String(cfg.vAlarmGate));
        html.replace("{ba}", String(cfg.balancingA));
        html.replace("{vs}", String(cfg.vSamples));
        html.replace("{to}", String(cfg.bmsTimeout));
        request->send(200, "text/html", html);
    });

    server.on("/save", HTTP_GET, [](AsyncWebServerRequest *request){
        prefs.begin("bms-bridge", false);
        if(request->hasParam("ca")) cfg.maxChargeA = request->getParam("ca")->value().toFloat();
        if(request->hasParam("da")) cfg.maxDischargeA = request->getParam("da")->value().toFloat();
        if(request->hasParam("vt")) cfg.vStartTaper = request->getParam("vt")->value().toFloat();
        if(request->hasParam("mv")) cfg.vMaxCharge = request->getParam("mv")->value().toFloat();
        if(request->hasParam("dvt")) cfg.vStartDTaper = request->getParam("dvt")->value().toFloat();
        if(request->hasParam("mdv")) cfg.vMinDischarge = request->getParam("mdv")->value().toFloat();
        if(request->hasParam("ag")) cfg.vAlarmGate = request->getParam("ag")->value().toFloat();
        if(request->hasParam("ba")) cfg.balancingA = request->getParam("ba")->value().toFloat();
        if(request->hasParam("vs")) cfg.vSamples = request->getParam("vs")->value().toInt();
        if(request->hasParam("to")) cfg.bmsTimeout = request->getParam("to")->value().toInt();
        
        prefs.putFloat("ca", cfg.maxChargeA); prefs.putFloat("da", cfg.maxDischargeA);
        prefs.putFloat("vt", cfg.vStartTaper); prefs.putFloat("mv", cfg.vMaxCharge);
        prefs.putFloat("dvt", cfg.vStartDTaper); prefs.putFloat("mdv", cfg.vMinDischarge);
        prefs.putFloat("ag", cfg.vAlarmGate); prefs.putFloat("ba", cfg.balancingA);
        prefs.putInt("vs", cfg.vSamples); prefs.putInt("to", cfg.bmsTimeout);
        prefs.end();
        request->redirect("/");
    });

    server.addHandler(&events);
    server.begin();

    // CAN Init
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_driver_install(&g_config, &t_config, &f_config);
    twai_start();

    pinMode(MCP2515_RST, OUTPUT);
    digitalWrite(MCP2515_RST, HIGH); delay(50); digitalWrite(MCP2515_RST, LOW); delay(50); digitalWrite(MCP2515_RST, HIGH);
    SPI.begin(MCP2515_SCLK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);
    Can_SMA.reset(); Can_SMA.setBitrate(CAN_500KBPS); Can_SMA.setNormalMode();
}

void loop() {
    ArduinoOTA.handle();
    if (logServer.hasClient()) {
        if (logClient) logClient.stop();
        logClient = logServer.available();
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
        }
    }

    struct can_frame in_frame;
    if (Can_SMA.readMessage(&in_frame) == MCP2515::ERROR_OK) {
        twai_message_t back_msg;
        back_msg.identifier = in_frame.can_id;
        back_msg.data_length_code = in_frame.can_dlc;
        memcpy(back_msg.data, in_frame.data, 8);
        twai_transmit(&back_msg, 0);
    }

    if (millis() - lastSmaTx > 250) {
        sendToSma();
        lastSmaTx = millis();
        
        static unsigned long lastPush = 0;
        if (millis() - lastPush > 2000) {
            String mode = (packVoltage > cfg.vStartTaper) ? "TAPER_C" : "NORMAL";
            if (packVoltage < cfg.vStartDTaper) mode = "TAPER_D";
            if (cellHighAlarm && packVoltage > cfg.vAlarmGate) mode = "BALANCE";
            
            char json[128];
            snprintf(json, sizeof(json), "{\"v\":%.2f,\"i\":%.1f,\"soc\":%d}", packVoltage, packCurrent, packSOC);
            events.send(json, "data", millis());
            netLog("[STATUS] Mode:%s V:%.2f I:%.1f CCL:%.1f DCL:%.1f\n", 
                   mode.c_str(), packVoltage, packCurrent, calculateCCL(packVoltage)/10.0, calculateDCL(packVoltage)/10.0);
            lastPush = millis();
        }
    }

    // Fail-safe logic
    if (millis() - lastBmsRx > (cfg.bmsTimeout * 1000) && lastBmsRx != 0) {
        packVoltage = cfg.vMaxCharge; // Triggers 0A logic
        netLog("[CRITICAL] BMS TIMEOUT!\n");
    }
}