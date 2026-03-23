#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
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

// --- POWER LIMITS ---
const float MAX_CHARGE_LIMIT_A    = 250.0; 
const float MAX_DISCHARGE_LIMIT_A = 500.0; 
const float BALANCING_A           = 1.0;   

// --- VOLTAGE THRESHOLDS ---
const float V_MAX_CHARGE     = 55.20; 
const float V_START_C_TAPER  = 54.00; 
const float V_START_D_TAPER  = 49.60; 
const float V_MIN_DISCHARGE  = 48.00; 
const float V_ALARM_GATE     = 53.50; 

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

// --- HTML DASHBOARD ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>BMS Bridge Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: sans-serif; text-align: center; background: #121212; color: #e0e0e0; margin: 0; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 15px; padding: 20px; }
    .card { background: #1e1e1e; padding: 15px; border-radius: 12px; border: 1px solid #333; }
    .value { font-size: 1.8em; font-weight: bold; color: #4caf50; margin: 5px 0; }
    #chart-container { width: 95%; max-width: 1000px; margin: 20px auto; background: #1e1e1e; padding: 15px; border-radius: 12px; border: 1px solid #333; }
    #console { width: 95%; max-width: 1000px; height: 120px; margin: 10px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 10px; overflow-y: scroll; border-radius: 8px; font-size: 0.85em; }
  </style>
</head>
<body>
  <h2 style="margin-top:20px;">SMA-DALY BRIDGE PRO</h2>
  <div class="grid">
    <div class="card"><div>Voltage</div><div id="v" class="value">0.00</div><div>Volts</div></div>
    <div class="card"><div>Current</div><div id="i" class="value">0.0</div><div>Amps</div></div>
    <div class="card"><div>Power</div><div id="p" class="value">0.0</div><div>kW</div></div>
    <div class="card"><div>SOC</div><div id="soc" class="value">0</div><div>%</div></div>
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
        yV: { type: 'linear', position: 'left', min: 46, max: 57 },
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
        if(con.childNodes.length > 50) con.removeChild(con.firstChild);
        con.scrollTop = con.scrollHeight;
      }, false);
    }
  </script>
</body></html>)rawliteral";

// --- CUSTOM LOGGER ---
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
    if (cellHighAlarm && v > V_ALARM_GATE) return (BALANCING_A * 10);
    if (v >= V_MAX_CHARGE) return 0;
    if (v < V_START_C_TAPER) return (MAX_CHARGE_LIMIT_A * 10);
    float ratio = (V_MAX_CHARGE - v) / (V_MAX_CHARGE - V_START_C_TAPER);
    return (uint16_t)(max(ratio * MAX_CHARGE_LIMIT_A, 2.0f) * 10);
}

uint16_t calculateDCL(float v) {
    if (v <= V_MIN_DISCHARGE) return 0;
    if (v > V_START_D_TAPER) return (uint16_t)(MAX_DISCHARGE_LIMIT_A * 10);
    float ratio = (v - V_MIN_DISCHARGE) / (V_START_D_TAPER - V_MIN_DISCHARGE);
    return (uint16_t)(max(ratio * MAX_DISCHARGE_LIMIT_A, 10.0f) * 10);
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
    ArduinoOTA.begin();
    logServer.begin(); 
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send_P(200, "text/html", index_html); });
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
        logClient.println("Bridge Logger Connected.");
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
            // Determine System Mode for Log
            String mode = "NORMAL";
            if (cellHighAlarm && packVoltage > V_ALARM_GATE) mode = "BALANCING";
            else if (packVoltage >= V_MAX_CHARGE) mode = "FULL";
            else if (packVoltage > V_START_C_TAPER) mode = "TAPER_CHG";
            else if (packVoltage < V_START_D_TAPER) mode = "TAPER_DISCH";

            // Push JSON to Browser
            char json[128];
            snprintf(json, sizeof(json), "{\"v\":%.2f,\"i\":%.1f,\"soc\":%d}", packVoltage, packCurrent, packSOC);
            events.send(json, "data", millis());
            
            // Detailed Log for Python script
            netLog("[STATUS] Mode:%s V:%.2f I:%.1f SOC:%d CCL:%.1f DCL:%.1f\n", 
                   mode.c_str(), packVoltage, packCurrent, packSOC, calculateCCL(packVoltage)/10.0, calculateDCL(packVoltage)/10.0);
            
            lastPush = millis();
        }
    }

    if (millis() - lastBmsRx > 15000 && lastBmsRx != 0) {
        packVoltage = V_MAX_CHARGE; // Stop current
        netLog("[CRITICAL] BMS TIMEOUT - SAFETY CUTOFF\n");
    }
}