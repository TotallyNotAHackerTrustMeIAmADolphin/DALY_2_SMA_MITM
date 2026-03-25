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

// --- CONFIG & NETWORK ---
const char *ssid = "wlesswg";
const char *password = "hba.1245";
IPAddress local_IP(192, 168, 178, 55);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);

#define WDT_TIMEOUT_SECONDS 15
#define CELL_COUNT 16.0f

struct Config
{
  float maxChargeA;
  float maxDischargeA;
  float cvStartTaper;
  float cvMaxCharge; // Cell-based voltages
  float cvStartDTaper;
  float cvMinDischarge;
  float cvHighAlarmGate;
  float cvLowAlarmGate;
  float trickleA;
  float limpDischargeA;
  int vSamples;
  int bmsTimeout;
  float cvMaintStart;
  float cvMaintStop;
  float maintAmps;
} cfg;

// --- STATE STORAGE ---
float packVoltage = 52.6;
float packCurrent = 0.0;
uint16_t packSOC = 50;
uint16_t packSOH = 100;
int16_t packTemp = 220;
uint16_t smaErrorCode = 0;
unsigned long lastSmaTx = 0;
unsigned long lastBmsRx = 0;
bool isResetting = false;
unsigned long resetHoldStartTime = 0;
bool manualMaintForce = false;
uint32_t bmsAlarmRaw = 0;
std::deque<float> vHistory;
String smaChargeMode = "Unknown";
bool gridPresent = false;
bool maintenanceActive = false;
uint32_t bootCount = 0;

MCP2515 Can_SMA(MCP2515_CS, 10000000, &SPI);
AsyncWebServer server(80);
AsyncEventSource events("/events");
WiFiServer logServer(2323);
WiFiClient logClient;
Preferences prefs;

// --- UTILITIES ---
void netLog(const char *format, ...)
{
  char loc_res[256];
  va_list arg;
  va_start(arg, format);
  vsnprintf(loc_res, sizeof(loc_res), format, arg);
  va_end(arg);
  Serial.print(loc_res);
  if (logClient && logClient.connected())
    logClient.print(loc_res);
  events.send(loc_res, "log", millis());
}

void loadConfig()
{
  prefs.begin("bms-bridge", false);
  bootCount = prefs.getUInt("boots", 0);
  prefs.putUInt("boots", ++bootCount);
  cfg.maxChargeA = prefs.getFloat("ca", 250.0);
  cfg.maxDischargeA = prefs.getFloat("da", 500.0);
  cfg.cvStartTaper = prefs.getFloat("cvt", 3.375);
  cfg.cvMaxCharge = prefs.getFloat("cmv", 3.450);
  cfg.cvStartDTaper = prefs.getFloat("cdvt", 3.100);
  cfg.cvMinDischarge = prefs.getFloat("cmdv", 3.000);
  cfg.cvHighAlarmGate = prefs.getFloat("cag", 3.425);
  cfg.cvLowAlarmGate = prefs.getFloat("clag", 3.065);
  cfg.trickleA = prefs.getFloat("ta", 2.0);
  cfg.limpDischargeA = prefs.getFloat("ld_v2", 15.0);
  cfg.vSamples = prefs.getInt("vs", 12);
  cfg.bmsTimeout = prefs.getInt("to", 15);
  cfg.cvMaintStart = prefs.getFloat("cmsv", 3.030);
  cfg.cvMaintStop = prefs.getFloat("cmpp", 3.220);
  cfg.maintAmps = prefs.getFloat("mam", 20.0);
  prefs.end();
}

float getFilteredVoltage(float newV)
{
  if (newV < 40.0 || newV > 62.0)
    return packVoltage;
  vHistory.push_back(newV);
  while (vHistory.size() > (size_t)cfg.vSamples)
    vHistory.pop_front();
  float sum = 0;
  for (float v : vHistory)
    sum += v;
  return sum / (float)vHistory.size();
}

uint16_t calculateCCL(float v, int soc)
{
  if (maintenanceActive)
    return (uint16_t)(cfg.maintAmps * 10);
  if (millis() - lastBmsRx > (unsigned long)(cfg.bmsTimeout * 1000))
    return 0;
  if (v >= (cfg.cvMaxCharge * CELL_COUNT))
    return 0;
  if (v >= (cfg.cvHighAlarmGate * CELL_COUNT))
    return (uint16_t)(cfg.trickleA * 10);
  if (v > (cfg.cvStartTaper * CELL_COUNT))
  {
    float vStart = cfg.cvStartTaper * CELL_COUNT;
    float vGate = cfg.cvHighAlarmGate * CELL_COUNT;
    float slope = (vGate - v) / (vGate - vStart);
    float target = cfg.trickleA + (slope * (cfg.maxChargeA - cfg.trickleA));
    return (uint16_t)(max(target, cfg.trickleA) * 10);
  }
  return (uint16_t)(cfg.maxChargeA * 10);
}

uint16_t calculateDCL(float v, int soc)
{
  if (maintenanceActive)
    return 0;
  if (millis() - lastBmsRx > (unsigned long)(cfg.bmsTimeout * 1000))
    return 0;
  if (v <= (cfg.cvMinDischarge * CELL_COUNT))
    return 0;
  if (v <= (cfg.cvLowAlarmGate * CELL_COUNT))
    return (uint16_t)(cfg.limpDischargeA * 10);
  if (v < (cfg.cvStartDTaper * CELL_COUNT))
  {
    float vGate = cfg.cvLowAlarmGate * CELL_COUNT;
    float vStart = cfg.cvStartDTaper * CELL_COUNT;
    float slope = (v - vGate) / (vStart - vGate);
    float target = cfg.limpDischargeA + (slope * (cfg.maxDischargeA - cfg.limpDischargeA));
    return (uint16_t)(max(target, cfg.limpDischargeA) * 10);
  }
  return (uint16_t)(cfg.maxDischargeA * 10);
}

void sendToSma()
{
  struct can_frame f;
  uint16_t ccl = calculateCCL(packVoltage, packSOC);
  uint16_t dcl = calculateDCL(packVoltage, packSOC);
  uint16_t cvl = maintenanceActive ? 560 : (uint16_t)(cfg.cvMaxCharge * CELL_COUNT * 10);
  uint16_t dvl = (uint16_t)(cfg.cvMinDischarge * CELL_COUNT * 10);

  f.can_id = 0x351;
  f.can_dlc = 8;
  f.data[0] = lowByte(cvl);
  f.data[1] = highByte(cvl);
  f.data[2] = lowByte(ccl);
  f.data[3] = highByte(ccl);
  f.data[4] = lowByte(dcl);
  f.data[5] = highByte(dcl);
  f.data[6] = (isResetting) ? 0x00 : (maintenanceActive ? 0x70 : 0xC0);
  f.data[7] = 0x00;
  Can_SMA.sendMessage(&f);

  f.can_id = 0x355;
  f.can_dlc = 4;
  uint16_t outSOC = maintenanceActive ? 2 : packSOC;
  f.data[0] = lowByte(outSOC);
  f.data[1] = highByte(outSOC);
  f.data[2] = 100;
  f.data[3] = 0;
  Can_SMA.sendMessage(&f);

  f.can_id = 0x356;
  f.can_dlc = 6;
  uint16_t v_out = (uint16_t)(packVoltage * 100);
  int16_t i_out = (int16_t)(packCurrent * 10.0);
  f.data[0] = lowByte(v_out);
  f.data[1] = highByte(v_out);
  f.data[2] = lowByte(i_out);
  f.data[3] = highByte(i_out);
  f.data[4] = lowByte(packTemp);
  f.data[5] = highByte(packTemp);
  Can_SMA.sendMessage(&f);

  f.can_id = 0x359;
  f.can_dlc = 8;
  memset(f.data, 0, 8);
  if (maintenanceActive)
    f.data[0] |= 0x10;
  Can_SMA.sendMessage(&f);

  static uint8_t ticker = 0;
  if (++ticker > 10)
  {
    ticker = 0;
    f.can_id = 0x35E;
    f.can_dlc = 8;
    f.data[0] = 'S';
    f.data[1] = 0;
    f.data[2] = 'M';
    f.data[3] = 0;
    f.data[4] = 'A';
    f.data[5] = 0;
    f.data[6] = 0;
    f.data[7] = 0;
    Can_SMA.sendMessage(&f);
    f.can_id = 0x35F;
    f.can_dlc = 8;
    f.data[0] = 3;
    f.data[1] = 0;
    f.data[2] = 0;
    f.data[3] = 0;
    f.data[4] = 0x48;
    f.data[5] = 0x03;
    f.data[6] = 0;
    f.data[7] = 0;
    Can_SMA.sendMessage(&f);
  }
}

// --- UI DASHBOARD ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>BMS Bridge Pro</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; text-align: center; background: #121212; color: #e0e0e0; margin: 0; }
  .nav { background: #1e1e1e; padding: 10px; border-bottom: 2px solid #333; margin-bottom: 10px; }
  .nav a { color: #4caf50; text-decoration: none; margin: 0 15px; font-weight: bold; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 10px; padding: 15px; }
  .card { background: #1e1e1e; padding: 15px; border-radius: 10px; border: 1px solid #333; }
  .value { font-size: 2em; font-weight: bold; color: #4caf50; }
  .m-active { color: #ff9800 !important; }
  .err-active { color: #f44336 !important; }
  .btn { border: none; padding: 12px 20px; border-radius: 5px; cursor: pointer; font-weight: bold; margin: 10px; font-size: 1em; display: inline-block; text-decoration: none; color: white; }
  .btn-blue { background: #0277bd; } .btn-red { background: #d32f2f; }
  #console { width: 95%; max-width: 1000px; height: 300px; margin: 15px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 15px; overflow-y: scroll; border-radius: 8px; font-size: 0.85em; border: 1px solid #444; }
</style></head><body>
<div class="nav"><a href="/">DASHBOARD</a> | <a href="/config">CONFIGURATION</a></div>
<div class="grid">
  <div class="card"><div>Pack Voltage</div><div id="v" class="value">--</div></div>
  <div class="card"><div>Avg Cell</div><div id="cv" class="value">--</div></div>
  <div class="card"><div>Current</div><div id="i" class="value">--</div></div>
  <div class="card"><div>SOC</div><div id="soc" class="value">--</div></div>
  <div class="card"><div>SMA Status</div><div id="smastat" class="value">--</div></div>
</div>
<div class="card" style="margin: 0 15px;">
  <a href="/toggleMaint" class="btn btn-blue" id="mbtn">TRIGGER FORCE CHARGE</a>
  <button class="btn btn-red" onclick="if(confirm('Simulate battery disconnect?')) fetch('/resetSMA')">CLEAR SMA ERROR (Reset)</button>
</div>
<div id="console">Log Active...<br></div>
<script>
  var source = new EventSource('/events');
  source.addEventListener('data', function(e) {
    var obj = JSON.parse(e.data);
    document.getElementById('v').innerHTML = obj.v.toFixed(2) + " V";
    document.getElementById('cv').innerHTML = (obj.v / 16.0).toFixed(3) + " V";
    document.getElementById('i').innerHTML = obj.i.toFixed(1) + " A";
    document.getElementById('soc').innerHTML = obj.soc + "%";
    document.getElementById('smastat').innerHTML = (obj.err > 0 && obj.err < 60000) ? "ERROR "+obj.err : "OK";
    if(obj.err > 0 && obj.err < 60000) document.getElementById('smastat').className = "value err-active";
    else document.getElementById('smastat').className = "value";
    const mb = document.getElementById('mbtn');
    if(obj.force) { mb.innerHTML = "STOP FORCE CHARGE"; mb.style.background = "#ff9800"; }
    else { mb.innerHTML = "TRIGGER FORCE CHARGE"; mb.style.background = "#0277bd"; }
  }, false);
  source.addEventListener('log', function(e) {
    const con = document.getElementById('console'); con.innerHTML += e.data + "<br>";
    if(con.childNodes.length > 100) con.removeChild(con.firstChild);
    con.scrollTop = con.scrollHeight;
  }, false);
</script></body></html>)rawliteral";

// --- UI CONFIG (Updated for V/Cell) ---
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>Settings</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #121212; color: #eee; padding: 10px; }
  .container { max-width: 650px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; }
  .row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; border-bottom: 1px solid #2a2a2a; padding-bottom: 8px; }
  .text-group { text-align: left; padding-right: 15px; }
  .desc { font-size: 0.8em; color: #888; display: block; margin-top: 2px; }
  h2 { color: #4caf50; border-bottom: 2px solid #4caf50; padding-bottom: 5px; margin-top: 25px; }
  .winter-h { color: #ff9800 !important; border-bottom: 2px solid #ff9800 !important; }
  input { font-size: 1.1em; padding: 5px; width: 110px; text-align: center; background: #000; color: #0f0; border: 1px solid #444; border-radius: 4px; }
  .save { background: #2e7d32; color: white; border: none; padding: 15px; width: 100%; border-radius: 5px; font-weight: bold; cursor: pointer; font-size: 1.1em; margin-top: 20px; }
  .reset { background: #d32f2f; color: white; border: none; padding: 10px; width: 100%; border-radius: 5px; font-weight: bold; cursor: pointer; font-size: 0.9em; text-decoration: none; display: block; text-align: center; margin-top: 15px; }
</style></head><body>
<div class="container">
  <a href="/" style="color:#4caf50;text-decoration:none;">&larr; Back to Dashboard</a>
  <form action="/save" method="GET">
    <h2>Charging Profile (16S)</h2>
    <div class="row"><div class="text-group"><strong>Max Charge Amps</strong><span class="desc">Global bulk charging limit.</span></div>
      <input type="number" name="ca" step="5" value="!!VAL_CA!!"></div>
    <div class="row"><div class="text-group"><strong>Start Taper Vpc</strong><span class="desc">Current begins to slow at this cell voltage.</span></div>
      <input type="number" name="cvt" step="0.001" value="!!VAL_VT!!"></div>
    <div class="row"><div class="text-group"><strong>Target Trickle Vpc</strong><span class="desc">Voltage where balancing floor is reached.</span></div>
      <input type="number" name="cag" step="0.001" value="!!VAL_AG!!"></div>
    <div class="row"><div class="text-group"><strong>Trickle Amps</strong><span class="desc">Constant current floor for balancing.</span></div>
      <input type="number" name="ta" step="0.5" value="!!VAL_TA!!"></div>
    <div class="row"><div class="text-group"><strong>Max Charge Vpc</strong><span class="desc">Absolute cell safety cutoff (Hard Floor).</span></div>
      <input type="number" name="cmv" step="0.001" value="!!VAL_MV!!"></div>

    <h2 class="winter-h">Winter Force Charge</h2>
    <div class="row"><div class="text-group"><strong>Maint. Start Vpc</strong><span class="desc">Trigger grid charge if any cell falls below this.</span></div>
      <input type="number" name="cmsv" step="0.001" value="!!VAL_MSV!!"></div>
    <div class="row"><div class="text-group"><strong>Maint. Stop Vpc</strong><span class="desc">Stop grid charge when cells reach this.</span></div>
      <input type="number" name="cmpp" step="0.001" value="!!VAL_MPP!!"></div>
    <div class="row"><div class="text-group"><strong>Maintenance Amps</strong><span class="desc">Constant current drawn from grid.</span></div>
      <input type="number" name="mam" step="1" value="!!VAL_MAM!!"></div>

    <h2>Discharging Profile</h2>
    <div class="row"><div class="text-group"><strong>Max Discharge Amps</strong><span class="desc">Peak household load limit.</span></div>
      <input type="number" name="da" step="10" value="!!VAL_DA!!"></div>
    <div class="row"><div class="text-group"><strong>Start Taper Vpc (D)</strong><span class="desc">Voltage where discharge current is restricted.</span></div>
      <input type="number" name="cdvt" step="0.001" value="!!VAL_DVT!!"></div>
    <div class="row"><div class="text-group"><strong>Target Limp Vpc</strong><span class="desc">Entry point for keeping-alive mode.</span></div>
      <input type="number" name="clag" step="0.001" value="!!VAL_LAG!!"></div>
    <div class="row"><div class="text-group"><strong>Limp Amps</strong><span class="desc">Minimum keeping-alive current floor.</span></div>
      <input type="number" name="ld" step="1" value="!!VAL_LIMP!!"></div>
    <div class="row"><div class="text-group"><strong>Min Discharge Vpc</strong><span class="desc">Absolute floor to prevent cell reversal.</span></div>
      <input type="number" name="cmdv" step="0.001" value="!!VAL_MDV!!"></div>

    <h2>System Tuning</h2>
    <div class="row"><div class="text-group"><strong>Voltage Window</strong><span class="desc">Number of moving average samples (1-20).</span></div>
      <input type="number" name="vs" step="1" value="!!VAL_VS!!"></div>

    <button type="submit" class="save">SAVE & APPLY ALL CHANGES</button>
  </form>
  <a href="/reset" class="reset" onclick="return confirm('Restore defaults?')">RESTORE FACTORY DEFAULTS</a>
</div></body></html>)rawliteral";

void setup()
{
  Serial.begin(115200);
  loadConfig();
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  esp_task_wdt_add(NULL);
  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED && millis() < 10000)
    delay(100);
  ArduinoOTA.begin();
  logServer.begin();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/html", index_html); });
  server.on("/toggleMaint", HTTP_GET, [](AsyncWebServerRequest *request)
            { manualMaintForce = !manualMaintForce; netLog("[USER] Manual Force Charge: %s\n", manualMaintForce?"ON":"OFF"); request->redirect("/"); });
  server.on("/resetSMA", HTTP_GET, [](AsyncWebServerRequest *request)
            { isResetting = true; resetHoldStartTime = millis(); netLog("[USER] Manual Cluster Reset.\n"); request->send(200, "text/plain", "OK"); });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        String h = String(config_html);
        h.replace("!!VAL_CA!!", String(cfg.maxChargeA, 0)); h.replace("!!VAL_VT!!", String(cfg.cvStartTaper, 3)); h.replace("!!VAL_AG!!", String(cfg.cvHighAlarmGate, 3));
        h.replace("!!VAL_TA!!", String(cfg.trickleA, 1)); h.replace("!!VAL_MV!!", String(cfg.cvMaxCharge, 3)); h.replace("!!VAL_MSV!!", String(cfg.cvMaintStart, 3));
        h.replace("!!VAL_MPP!!", String(cfg.cvMaintStop, 3)); h.replace("!!VAL_MAM!!", String(cfg.maintAmps, 0)); h.replace("!!VAL_DA!!", String(cfg.maxDischargeA, 0));
        h.replace("!!VAL_DVT!!", String(cfg.cvStartDTaper, 3)); h.replace("!!VAL_LAG!!", String(cfg.cvLowAlarmGate, 3)); h.replace("!!VAL_LIMP!!", String(cfg.limpDischargeA, 0));
        h.replace("!!VAL_MDV!!", String(cfg.cvMinDischarge, 3)); h.replace("!!VAL_VS!!", String(cfg.vSamples));
        request->send(200, "text/html", h); });

  server.on("/save", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        prefs.begin("bms-bridge", false);
        auto cF = [&](String p, float &v, String k) { if(request->hasParam(p)) { v = request->getParam(p)->value().toFloat(); prefs.putFloat(k.c_str(), v); } };
        cF("ca", cfg.maxChargeA, "ca"); cF("cvt", cfg.cvStartTaper, "cvt"); cF("cag", cfg.cvHighAlarmGate, "cag");
        cF("ta", cfg.trickleA, "ta"); cF("cmv", cfg.cvMaxCharge, "cmv"); cF("cmsv", cfg.cvMaintStart, "cmsv");
        cF("cmpp", cfg.cvMaintStop, "cmpp"); cF("mam", cfg.maintAmps, "mam"); cF("da", cfg.maxDischargeA, "da");
        cF("cdvt", cfg.cvStartDTaper, "cdvt"); cF("clag", cfg.cvLowAlarmGate, "clag"); cF("ld", cfg.limpDischargeA, "ld_v2");
        cF("cmdv", cfg.cvMinDischarge, "cmdv");
        if(request->hasParam("vs")) { cfg.vSamples = request->getParam("vs")->value().toInt(); prefs.putInt("vs", cfg.vSamples); }
        prefs.end(); netLog("[CONFIG] Cell-based settings updated.\n"); request->redirect("/"); });

  server.addHandler(&events);
  server.begin();
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  twai_driver_install(&g_config, &t_config, &f_config);
  twai_start();
  pinMode(MCP2515_RST, OUTPUT);
  digitalWrite(MCP2515_RST, HIGH);
  delay(50);
  digitalWrite(MCP2515_RST, LOW);
  delay(50);
  digitalWrite(MCP2515_RST, HIGH);
  SPI.begin(MCP2515_SCLK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);
  Can_SMA.reset();
  Can_SMA.setBitrate(CAN_500KBPS);
  Can_SMA.setNormalMode();
}

void loop()
{
  esp_task_wdt_reset();
  ArduinoOTA.handle();
  if (logServer.hasClient())
  {
    if (logClient)
      logClient.stop();
    logClient = logServer.available();
    netLog("--- LOGGER ONLINE ---\n");
  }
  twai_status_info_t twai_stat;
  twai_get_status_info(&twai_stat);
  if (twai_stat.state == TWAI_STATE_BUS_OFF)
    twai_initiate_recovery();

  twai_message_t b_msg;
  if (twai_receive(&b_msg, 0) == ESP_OK)
  {
    lastBmsRx = millis();
    if (b_msg.identifier == 0x356)
    {
      uint16_t v_raw = (b_msg.data[1] << 8) | b_msg.data[0];
      packVoltage = getFilteredVoltage(v_raw / 100.0);
      int16_t i_raw = (int16_t)((b_msg.data[3] << 8) | b_msg.data[2]);
      packCurrent = i_raw / 10.0;
      packTemp = (int16_t)((b_msg.data[5] << 8) | b_msg.data[4]);
    }
    if (b_msg.identifier == 0x355)
    {
      packSOC = (b_msg.data[1] << 8) | b_msg.data[0];
      packSOH = (b_msg.data[3] << 8) | b_msg.data[2];
    }
    if (b_msg.identifier == 0x359)
      bmsAlarmRaw = (b_msg.data[3] << 24) | (b_msg.data[2] << 16) | (b_msg.data[1] << 8) | b_msg.data[0];
  }

  struct can_frame in_frame;
  if (Can_SMA.readMessage(&in_frame) == MCP2515::ERROR_OK)
  {
    if (in_frame.can_id == 0x305)
    {
      uint8_t m = in_frame.data[0];
      smaChargeMode = (m == 1) ? "Bulk" : (m == 2) ? "Absorption"
                                      : (m == 3)   ? "Float"
                                                   : "Equalize";
    }
    if (in_frame.can_id == 0x300)
      gridPresent = (in_frame.data[0] & 0x01);
    if (in_frame.can_id == 0x301)
    {
      uint16_t nE = (in_frame.data[1] << 8) | in_frame.data[0];
      if (nE > 0 && nE < 60000 && nE != smaErrorCode)
      {
        smaErrorCode = nE;
        netLog("[SMA-ALARM] Error %d detected!\n", smaErrorCode);
        if (smaErrorCode == 9331 || smaErrorCode == 8609 || smaErrorCode == 9362)
        {
          isResetting = true;
          resetHoldStartTime = millis();
        }
      }
      else if (nE == 0 || nE > 60000)
      {
        smaErrorCode = nE;
      }
    }
  }

  if (isResetting && (millis() - resetHoldStartTime > 5500))
  {
    isResetting = false;
    netLog("[SYS] Recovery cycle finished.\n");
  }

  if (millis() - lastSmaTx > 250)
  {
    static bool autoMaint = false;
    if (!autoMaint && packVoltage < (cfg.cvMaintStart * CELL_COUNT))
      autoMaint = true;
    else if (autoMaint && packVoltage > (cfg.cvMaintStop * CELL_COUNT))
      autoMaint = false;
    maintenanceActive = manualMaintForce || autoMaint;

    sendToSma();
    lastSmaTx = millis();
    static unsigned long lastPush = 0;
    if (millis() - lastPush > 2000)
    {
      char json[400];
      snprintf(json, sizeof(json), "{\"v\":%.2f,\"i\":%.1f,\"soc\":%d,\"smam\":\"%s\",\"maint\":%d,\"force\":%d,\"err\":%d,\"isR\":%d}",
               packVoltage, packCurrent, (int)packSOC, smaChargeMode.c_str(), (int)autoMaint, (int)manualMaintForce, (int)smaErrorCode, (int)isResetting);
      events.send(json, "data", millis());
      String modeStr = maintenanceActive ? "MAINT" : "RUN";
      netLog("[STATUS] Mode:%s V:%.2f (Avg:%.3fV) I:%.1f SOC:%d%% CCL:%.1f DCL:%.1f SMA:%s\n", modeStr.c_str(), packVoltage, packVoltage / 16.0, packCurrent, (int)packSOC, calculateCCL(packVoltage, packSOC) / 10.0, calculateDCL(packVoltage, packSOC) / 10.0, smaChargeMode.c_str());
      lastPush = millis();
    }
  }
  if (ESP.getFreeHeap() < 18000)
  {
    netLog("[SYS] Heap Critical. Safe Restart.\n");
    ESP.restart();
  }
}