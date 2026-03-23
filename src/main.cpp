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

// --- SETTINGS (Defaults) ---
float maxChargeA = 250.0;
float maxDischargeA = 500.0;
float vStartTaper = 54.00;
float vMaxCharge = 55.20;

// --- STATE STORAGE ---
float packVoltage = 52.6; 
float packCurrent = 0.0;
uint16_t packSOC = 50;
bool cellHighAlarm = false; 
unsigned long lastSmaTx = 0;
unsigned long lastBmsRx = 0;

std::deque<float> vHistory;
const int V_SAMPLES = 10; 

MCP2515 Can_SMA(MCP2515_CS, 10000000, &SPI);
AsyncWebServer server(80);
AsyncEventSource events("/events");
WiFiServer logServer(2323); 
WiFiClient logClient;
Preferences prefs;

// --- HTML DASHBOARD (With Settings Form) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>BMS Bridge Pro</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: sans-serif; text-align: center; background: #121212; color: #e0e0e0; margin: 0; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; padding: 15px; }
    .card { background: #1e1e1e; padding: 15px; border-radius: 12px; border: 1px solid #333; }
    .value { font-size: 1.8em; font-weight: bold; color: #4caf50; }
    .config-card { background: #252525; margin: 15px; padding: 15px; border-radius: 12px; text-align: left; }
    input { background: #333; color: white; border: 1px solid #555; padding: 8px; border-radius: 4px; width: 80px; margin: 5px; }
    button { background: #2e7d32; color: white; border: none; padding: 10px 20px; border-radius: 5px; cursor: pointer; }
    #chart-container { width: 95%; max-width: 1000px; margin: auto; background: #1e1e1e; padding: 10px; border-radius: 12px; }
    #console { width: 95%; max-width: 1000px; height: 100px; margin: 10px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 10px; overflow-y: scroll; font-size: 0.8em; }
  </style>
</head>
<body>
  <h2>SMA-DALY BRIDGE PRO</h2>
  <div class="grid">
    <div class="card"><div>Voltage</div><div id="v" class="value">0.00</div></div>
    <div class="card"><div>Amps</div><div id="i" class="value">0.0</div></div>
    <div class="card"><div>SOC</div><div id="soc" class="value">0</div></div>
  </div>

  <div class="config-card">
    <strong>Live Tuning:</strong><br>
    Start Taper: <input type="number" step="0.1" id="in_vt" value="54.0"> V | 
    Max Charge: <input type="number" id="in_ca" value="250"> A | 
    Max Dischg: <input type="number" id="in_da" value="500"> A
    <button onclick="saveSettings()">Apply Settings</button>
  </div>

  <div id="chart-container"><canvas id="liveChart"></canvas></div>
  <div id="console">System Ready.</div>

  <script>
    function saveSettings() {
      const vt = document.getElementById('in_vt').value;
      const ca = document.getElementById('in_ca').value;
      const da = document.getElementById('in_da').value;
      fetch(`/set?vt=${vt}&ca=${ca}&da=${da}`).then(r => alert('Settings Saved!'));
    }

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

void netLog(const char* format, ...) {
    char loc_res[256];
    va_list arg; va_start(arg, format);
    vsnprintf(loc_res, sizeof(loc_res), format, arg);
    va_end(arg);
    Serial.print(loc_res);
    if (logClient && logClient.connected()) logClient.print(loc_res);
    events.send(loc_res, "log", millis());
}

float getFilteredVoltage(float newV) {
    vHistory.push_back(newV);
    if (vHistory.size() > V_SAMPLES) vHistory.pop_front();
    float sum = 0; for (float v : vHistory) sum += v;
    return sum / vHistory.size();
}

uint16_t calculateCCL(float v) {
    if (cellHighAlarm && v > 53.5) return 10; // 1A balancing
    if (v >= vMaxCharge) return 0;
    if (v < vStartTaper) return (uint16_t)(maxChargeA * 10);
    float ratio = (vMaxCharge - v) / (vMaxCharge - vStartTaper);
    return (uint16_t)(max(ratio * maxChargeA, 2.0f) * 10);
}

uint16_t calculateDCL(float v) {
    if (v <= 48.0) return 0;
    if (v > 49.6) return (uint16_t)(maxDischargeA * 10);
    float ratio = (v - 48.0) / (49.6 - 48.0);
    return (uint16_t)(max(ratio * maxDischargeA, 10.0f) * 10);
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
    
    // Load Persisted Settings
    prefs.begin("bms-bridge", false);
    vStartTaper = prefs.getFloat("vt", 54.00);
    maxChargeA = prefs.getFloat("ca", 250.0);
    maxDischargeA = prefs.getFloat("da", 500.0);

    ArduinoOTA.begin();
    logServer.begin(); 

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("vt")) vStartTaper = request->getParam("vt")->value().toFloat();
        if (request->hasParam("ca")) maxChargeA = request->getParam("ca")->value().toFloat();
        if (request->hasParam("da")) maxDischargeA = request->getParam("da")->value().toFloat();
        prefs.putFloat("vt", vStartTaper);
        prefs.putFloat("ca", maxChargeA);
        prefs.putFloat("da", maxDischargeA);
        request->send(200, "text/plain", "OK");
    });

    server.addHandler(&events);
    server.begin();

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
            String mode = (packVoltage > vStartTaper) ? "TAPER" : "NORMAL";
            if (cellHighAlarm && packVoltage > 53.5) mode = "BALANCE";
            
            char json[128];
            snprintf(json, sizeof(json), "{\"v\":%.2f,\"i\":%.1f,\"soc\":%d}", packVoltage, packCurrent, packSOC);
            events.send(json, "data", millis());
            netLog("[STATUS] Mode:%s V:%.2f I:%.1f CCL:%.1f DCL:%.1f\n", 
                   mode.c_str(), packVoltage, packCurrent, calculateCCL(packVoltage)/10.0, calculateDCL(packVoltage)/10.0);
            lastPush = millis();
        }
    }

    if (millis() - lastBmsRx > 15000 && lastBmsRx != 0) {
        packVoltage = vMaxCharge;
    }
}