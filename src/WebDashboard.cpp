#include "WebDashboard.h"
#include "WebPages.h"
#include "SDLogger.h"
#include <SD.h>

WebDashboard::WebDashboard(uint16_t port)
    : _server(port), _events("/events"), _actionCb(nullptr), _cfg(nullptr) {}

void WebDashboard::begin(SystemConfig &configOut)
{
    _cfg = &configOut;

    loadConfig();
    setupRoutes();

    _server.addHandler(&_events);
    _server.begin();
}

void WebDashboard::setActionCallback(ActionCallback cb)
{
    _actionCb = cb;
}

void WebDashboard::broadcastLog(const char *msg)
{
    // Send string to the "log" event listener in the JS
    _events.send(msg, "log", millis());
}

void WebDashboard::broadcastTelemetry(const DashboardData &data)
{
    // Build the cell array safely using char arrays instead of String concatenation
    char cellsStr[256] = "[";
    for (size_t i = 0; i < data.cellVoltages.size(); i++)
    {
        char temp[16];
        snprintf(temp, sizeof(temp), "%.3f", data.cellVoltages[i]);
        strcat(cellsStr, temp);
        if (i < data.cellVoltages.size() - 1)
            strcat(cellsStr, ",");
    }
    strcat(cellsStr, "]");

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"v\":%.2f,\"cv\":%.3f,\"minC\":%.3f,\"maxC\":%.3f,\"i\":%.1f,\"reqI\":%.1f,\"soc\":%.1f,\"smam\":\"%s\",\"maint\":%d,\"force\":%d,\"isR\":%d,\"cells\":%s}",
             data.packVoltage, data.avgCellVoltage, data.minCellVoltage, data.maxCellVoltage,
             data.packCurrent, data.requestedCurrent, data.packSOC,
             data.smaChargeMode.c_str(), (int)data.maintenanceActive, (int)data.forceCharge,
             (int)data.isResetting, cellsStr);

    _events.send(json, "data", millis());
}

void WebDashboard::loadConfig()
{
    _prefs.begin("bms-bridge", false);

    // Read from NVS or set defaults
    _cfg->maxChargeA = _prefs.getFloat("ca", 250.0);
    _cfg->maxDischargeA = _prefs.getFloat("da", 500.0);
    _cfg->cvStartTaper = _prefs.getFloat("cvt", 3.375);
    _cfg->cvMaxCharge = _prefs.getFloat("cmv", 3.450);
    _cfg->cvStartDTaper = _prefs.getFloat("cdvt", 3.100);
    _cfg->cvMinDischarge = _prefs.getFloat("cmdv", 3.000);
    _cfg->cvHighAlarmGate = _prefs.getFloat("cag", 3.425);
    _cfg->cvLowAlarmGate = _prefs.getFloat("clag", 3.065);
    _cfg->trickleA = _prefs.getFloat("ta", 2.0);
    _cfg->limpDischargeA = _prefs.getFloat("ld_v2", 15.0);
    _cfg->vSamples = _prefs.getInt("vs", 12);
    _cfg->bmsTimeout = _prefs.getInt("to", 60);
    _cfg->cvMaintStart = _prefs.getFloat("cmsv", 3.030);
    _cfg->cvMaintStop = _prefs.getFloat("cmpp", 3.220);
    _cfg->maintAmps = _prefs.getFloat("mam", 20.0);

    _prefs.end();
}

void WebDashboard::saveConfig(AsyncWebServerRequest *request)
{
    _prefs.begin("bms-bridge", false);

    auto saveFloat = [&](const char *param, float &val)
    {
        if (request->hasParam(param))
        {
            val = request->getParam(param)->value().toFloat();
            _prefs.putFloat(param, val);
        }
    };

    saveFloat("ca", _cfg->maxChargeA);
    saveFloat("cvt", _cfg->cvStartTaper);
    saveFloat("cag", _cfg->cvHighAlarmGate);
    saveFloat("ta", _cfg->trickleA);
    saveFloat("cmv", _cfg->cvMaxCharge);
    saveFloat("cmsv", _cfg->cvMaintStart);
    saveFloat("cmpp", _cfg->cvMaintStop);
    saveFloat("mam", _cfg->maintAmps);
    saveFloat("da", _cfg->maxDischargeA);
    saveFloat("cdvt", _cfg->cvStartDTaper);
    saveFloat("clag", _cfg->cvLowAlarmGate);
    saveFloat("ld_v2", _cfg->limpDischargeA);
    saveFloat("cmdv", _cfg->cvMinDischarge);

    if (request->hasParam("vs"))
    {
        _cfg->vSamples = request->getParam("vs")->value().toInt();
        _prefs.putInt("vs", _cfg->vSamples);
    }

    _prefs.end();

    if (_actionCb)
        _actionCb("configSaved");
    request->redirect("/");
}

bool WebDashboard::findLogFile(AsyncWebServerRequest *request, String &outName, uint32_t &outSize)
{
    if (!request->hasParam("file"))
    {
        request->send(400, "text/plain", "Missing file parameter");
        return false;
    }
    String requested = request->getParam("file")->value();

    // Only match names we actually listed ourselves - this doubles as the
    // path-traversal guard, since our filenames never contain '/' or '..'.
    std::vector<String> names;
    std::vector<uint32_t> sizes;
    SDLogger::listLogFiles(names, sizes);

    for (size_t i = 0; i < names.size(); i++)
    {
        if (names[i] == requested)
        {
            outName = names[i];
            outSize = sizes[i];
            return true;
        }
    }

    request->send(404, "text/plain", "Unknown log file");
    return false;
}

const char *WebDashboard::contentTypeForLogFile(const String &name)
{
    return name.endsWith(".csv") ? "text/csv" : "text/plain";
}

void WebDashboard::setupRoutes()
{
    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(200, "text/html", index_html); });

    _server.on("/toggleMaint", HTTP_GET, [this](AsyncWebServerRequest *request)
               { 
        if (_actionCb) _actionCb("toggleMaint");
        request->redirect("/"); });

    _server.on("/resetSMA", HTTP_GET, [this](AsyncWebServerRequest *request)
               { 
        if (_actionCb) _actionCb("resetSMA");
        request->send(200, "text/plain", "OK"); });

    _server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request)
               {
        String h = String(config_html);
        h.replace("!!VAL_CA!!", String(_cfg->maxChargeA, 0)); 
        h.replace("!!VAL_VT!!", String(_cfg->cvStartTaper, 3)); 
        h.replace("!!VAL_AG!!", String(_cfg->cvHighAlarmGate, 3));
        h.replace("!!VAL_TA!!", String(_cfg->trickleA, 1)); 
        h.replace("!!VAL_MV!!", String(_cfg->cvMaxCharge, 3)); 
        h.replace("!!VAL_MSV!!", String(_cfg->cvMaintStart, 3));
        h.replace("!!VAL_MPP!!", String(_cfg->cvMaintStop, 3)); 
        h.replace("!!VAL_MAM!!", String(_cfg->maintAmps, 0)); 
        h.replace("!!VAL_DA!!", String(_cfg->maxDischargeA, 0));
        h.replace("!!VAL_DVT!!", String(_cfg->cvStartDTaper, 3)); 
        h.replace("!!VAL_LAG!!", String(_cfg->cvLowAlarmGate, 3)); 
        h.replace("!!VAL_LIMP!!", String(_cfg->limpDischargeA, 0));
        h.replace("!!VAL_MDV!!", String(_cfg->cvMinDischarge, 3)); 
        h.replace("!!VAL_VS!!", String(_cfg->vSamples));
        request->send(200, "text/html", h); });

    _server.on("/save", HTTP_GET, [this](AsyncWebServerRequest *request)
               { saveConfig(request); });

    _server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(200, "text/html", logs_html); });

    _server.on("/api/logs/list", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        std::vector<String> names;
        std::vector<uint32_t> sizes;
        SDLogger::listLogFiles(names, sizes);

        String json = "[";
        for (size_t i = 0; i < names.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"name\":\"" + names[i] + "\",\"size\":" + String(sizes[i]) + "}";
        }
        json += "]";
        request->send(200, "application/json", json); });

    _server.on("/api/logs/content", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        String name; uint32_t size;
        if (!findLogFile(request, name, size)) return;

        const size_t maxBytes = 65536;
        String content;
        if (!SDLogger::readTail(name, content, maxBytes)) {
            request->send(500, "text/plain", "Failed to read file");
            return;
        }

        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", content);
        if (size > maxBytes) {
            response->addHeader("X-Truncated", "1");
        }
        request->send(response); });

    _server.on("/api/logs/download", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        String name; uint32_t size;
        if (!findLogFile(request, name, size)) return;

        SemaphoreHandle_t mtx = SDLogger::sdMutex();
        if (!mtx || xSemaphoreTake(mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
            request->send(503, "text/plain", "SD card busy, try again");
            return;
        }

        // Released exactly once when this connection closes (completion or
        // abort) - this library always closes file-response connections
        // (no keep-alive), so onDisconnect is a reliable single release point.
        request->onDisconnect([mtx]() { xSemaphoreGive(mtx); });

        request->send(SD, "/" + name, contentTypeForLogFile(name), true /* download */); });
}