#include "WebDashboard.h"
#include "WebPages.h"
#include "SDLogger.h"
#include <SD.h>
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

WebDashboard::WebDashboard(uint16_t port)
    : _server(port), _events("/events"), _actionCb(nullptr), _cfg(nullptr) {}

void WebDashboard::begin()
{
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

void WebDashboard::loadConfig(SystemConfig &configOut)
{
    _cfg = &configOut;

    // No dataMutex needed here: this runs once from setup(), before bmsTask
    // or canTask exist, so there is no concurrent reader yet.
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
    // calculateCCL()/calculateDCL() in main.cpp's loop() read these same
    // _cfg fields while holding dataMutex. Taking the same mutex here makes
    // the whole set of field updates atomic from loop()'s point of view,
    // instead of a reader potentially seeing a mix of old and new setpoints
    // mid-save.
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(300)) != pdTRUE)
    {
        request->send(503, "text/plain", "Device busy, please try Save again");
        return;
    }

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
    xSemaphoreGive(dataMutex);

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
    if (!SDLogger::listLogFiles(names, sizes))
    {
        // Distinguish "SD card busy/not ready" from "genuinely no such file"
        // below - otherwise a transient lock timeout looks like a 404 and
        // misleads anyone debugging it.
        request->send(503, "text/plain", "SD card busy, try again");
        return false;
    }

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

        // Deliberately conservative, not "as much as fits in RAM": this is
        // a single contiguous String, copied again internally by
        // beginResponse(). Live-tested at 64KB with 80KB+ free heap and it
        // still failed - getMaxAllocHeap() showed only ~43KB was actually
        // contiguous, so the copy silently produced an empty String (no
        // error, just content-length: 0) rather than the requested tail.
        // 8KB stays comfortably under real-world fragmentation on this
        // device. A chunked/streaming response (like /api/logs/download
        // already uses) would remove this ceiling entirely if a larger
        // tail is ever needed.
        const size_t maxBytes = 8192;
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
        // Held for the WHOLE transfer (accepted tradeoff: the background
        // writer drops samples it can't log during that window - see PR
        // description). Explicitly (re-)set the library's ack timeout so a
        // client that stops ACKing (e.g. walks out of WiFi range) gets
        // force-disconnected - and this mutex released - within 5s rather
        // than relying silently on the library's own default.
        request->client()->setAckTimeout(5000);
        request->onDisconnect([mtx]() { xSemaphoreGive(mtx); });

        request->send(SD, "/" + name, contentTypeForLogFile(name), true /* download */); });

    // JSON headline of the last stored core dump (issue #11) - task/PC/cause/
    // backtrace, for a UI or API consumer that doesn't want to pull and
    // decode the whole raw ELF dump via /api/coredump below.
    //
    // Registered BEFORE /api/coredump: AsyncURIMatcher's default match type
    // for a plain string with no trailing '*' is "BackwardCompatible", which
    // matches the URI itself OR anything starting with "<uri>/" (see
    // AsyncURIMatcher::matches() in ESPAsyncWebServer's WebServer.cpp) - so
    // "/api/coredump" would also swallow requests to "/api/coredump/summary"
    // if that route were registered second, since AsyncWebServer dispatches
    // to the first handler in registration order whose canHandle() matches
    // (see AsyncWebServer::_attachHandler()).
    _server.on("/api/coredump/summary", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        esp_err_t checkErr = esp_core_dump_image_check();

        static esp_core_dump_summary_t summary;
        bool present = false;
        if (checkErr == ESP_OK) {
            memset(&summary, 0, sizeof(summary));
            present = (esp_core_dump_get_summary(&summary) == ESP_OK);
        }

        char json[900];
        size_t len = 0;
        int n;

        n = snprintf(json + len, sizeof(json) - len,
                     "{\"check\":\"%.32s\",\"present\":%s",
                     esp_err_to_name(checkErr), present ? "true" : "false");
        if (n < 0 || (size_t)n >= sizeof(json) - len) {
            request->send(500, "text/plain", "coredump summary too large");
            return;
        }
        len += (size_t)n;

        if (present) {
            // Backtrace as a JSON array of "0xXXXXXXXX" strings - worst
            // case 16 entries: 16 * strlen("\"0x12345678\",") + brackets.
            char bt[224] = "[";
            size_t btLen = 1;
            for (uint32_t i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
                int bn = snprintf(bt + btLen, sizeof(bt) - btLen, "%s\"0x%08x\"",
                                   i ? "," : "", (unsigned)summary.exc_bt_info.bt[i]);
                if (bn < 0 || btLen + (size_t)bn >= sizeof(bt))
                    break;
                btLen += (size_t)bn;
            }
            bt[btLen++] = ']';
            bt[btLen] = '\0';

            n = snprintf(json + len, sizeof(json) - len,
                         ",\"task\":\"%.16s\",\"pc\":\"0x%08x\",\"cause\":%u,"
                         "\"corrupted\":%s,\"backtrace\":%s,\"crash_elf_sha256\":\"%.16s\"",
                         summary.exc_task, (unsigned)summary.exc_pc,
                         (unsigned)summary.ex_info.exc_cause,
                         summary.exc_bt_info.corrupted ? "true" : "false",
                         bt, (const char *)summary.app_elf_sha256);
            if (n < 0 || (size_t)n >= sizeof(json) - len) {
                request->send(500, "text/plain", "coredump summary too large");
                return;
            }
            len += (size_t)n;
        }

        char runningSha[17] = {0};
        esp_ota_get_app_elf_sha256(runningSha, sizeof(runningSha));

        n = snprintf(json + len, sizeof(json) - len,
                     ",\"reset_reason\":%d,\"running_elf_sha256\":\"%.16s\"}",
                     (int)esp_reset_reason(), runningSha);
        if (n < 0 || (size_t)n >= sizeof(json) - len) {
            request->send(500, "text/plain", "coredump summary too large");
            return;
        }
        len += (size_t)n;

        request->send(200, "application/json", json); });

    // Raw core dump image from the flash "coredump" partition, written by
    // ESP-IDF on the last panic (ELF format, CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    // is on in this Arduino core's prebuilt sdkconfig). Decode on a PC with
    // the ELF of the firmware that crashed:
    //   espcoredump.py info_corefile -t raw -c coredump.bin firmware.elf
    // Streamed straight from flash in small chunks - no 64KB heap buffer.
    _server.on("/api/coredump", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        size_t addr = 0, size = 0;
        if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
            request->send(404, "text/plain", "No core dump stored");
            return;
        }

        AsyncWebServerResponse *response = request->beginResponse(
            "application/octet-stream", size,
            [addr, size](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                if (index >= size)
                    return 0;
                size_t n = size - index;
                if (n > maxLen)
                    n = maxLen;
                if (esp_flash_read(NULL, buffer, addr + index, n) != ESP_OK)
                    return 0;
                return n;
            });
        response->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
        request->send(response); });

    _server.on("/graphs", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(200, "text/html", graphs_html); });

    _server.on("/api/logs/graph", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        String name; uint32_t size;
        if (!findLogFile(request, name, size)) return;

        if (!name.endsWith(".csv")) {
            request->send(400, "text/plain", "Not a telemetry CSV file");
            return;
        }

        // 300, not readGraphSeries' own 2000-point ceiling: request->send()
        // needs this whole CSV as one contiguous String, same as
        // /api/logs/content - live-tested at 600 points (~42KB) with a
        // fragmented heap (getMaxAllocHeap() well under that) and it came
        // back as a silent empty 200 response. 300 points keeps output
        // small enough to reliably fit, and is still plenty of resolution
        // for a trend chart at typical browser widths.
        String csv;
        if (!SDLogger::readGraphSeries(name, 300, csv)) {
            request->send(500, "text/plain", "Failed to read file (it may be too large to graph - try Download instead)");
            return;
        }

        request->send(200, "text/csv", csv); });
}