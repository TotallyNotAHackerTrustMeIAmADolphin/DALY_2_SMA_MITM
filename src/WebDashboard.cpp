#include "WebDashboard.h"
#include "WebPages.h"
#include "SDLogger.h"
#include "TelemetrySchema.h"
#include <SD.h>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

namespace
{
    // One row per NVS-persisted setting (issue #34). `key` is both the NVS
    // storage key and the /save HTTP GET parameter name for every field
    // below (they've always been the same string). `placeholder` is the
    // !!VAL_XXX!! token substituted into config_html by the /config route;
    // nullptr means the field has no UI representation.
    //
    // KIND_UINT16 exists (rather than folding sps/spm into KIND_INT) because
    // they're stored via Preferences::getUInt/putUInt, not getInt/putInt -
    // NVS enforces the stored type, so reading a UInt-written key with
    // getInt would fail against a device's existing NVS content.
    //
    // Keys as of #34 (unchanged from before this refactor):
    //   ca cvt cag ta cmv cmsv cmpp mam da cdvt clag ld_v2 cmdv vs to sps spm
    // `to` (bmsTimeout) is loaded from NVS but was never wired to /save or
    // the /config page even before this change - see the note in
    // saveConfig() below. Preserved as-is: placeholder = nullptr, and it's
    // simply never present in a /save request.
    struct ConfigField
    {
        const char *key;
        enum Kind
        {
            KIND_FLOAT,
            KIND_INT,
            KIND_UINT16
        } kind;
        size_t offset; // offsetof(SystemConfig, <member>)
        float defF;
        int defI; // also holds the default for KIND_UINT16 fields
        const char *placeholder;
        uint8_t decimals; // String(float, decimals) formatting for /config; KIND_INT/KIND_UINT16 ignore this
    };

    const ConfigField kConfigFields[] = {
        // key      kind                       offset                                    defF    defI  placeholder      decimals
        {"ca", ConfigField::KIND_FLOAT, offsetof(SystemConfig, maxChargeA), 250.0f, 0, "!!VAL_CA!!", 0},
        {"cvt", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvStartTaper), 3.375f, 0, "!!VAL_VT!!", 3},
        {"cag", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvHighAlarmGate), 3.425f, 0, "!!VAL_AG!!", 3},
        {"ta", ConfigField::KIND_FLOAT, offsetof(SystemConfig, trickleA), 2.0f, 0, "!!VAL_TA!!", 1},
        {"cmv", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMaxCharge), 3.450f, 0, "!!VAL_MV!!", 3},
        {"cmsv", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMaintStart), 3.030f, 0, "!!VAL_MSV!!", 3},
        {"cmpp", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMaintStop), 3.220f, 0, "!!VAL_MPP!!", 3},
        {"mam", ConfigField::KIND_FLOAT, offsetof(SystemConfig, maintAmps), 20.0f, 0, "!!VAL_MAM!!", 0},
        {"da", ConfigField::KIND_FLOAT, offsetof(SystemConfig, maxDischargeA), 500.0f, 0, "!!VAL_DA!!", 0},
        {"cdvt", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvStartDTaper), 3.100f, 0, "!!VAL_DVT!!", 3},
        {"clag", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvLowAlarmGate), 3.065f, 0, "!!VAL_LAG!!", 3},
        {"ld_v2", ConfigField::KIND_FLOAT, offsetof(SystemConfig, limpDischargeA), 15.0f, 0, "!!VAL_LIMP!!", 0},
        {"cmdv", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMinDischarge), 3.000f, 0, "!!VAL_MDV!!", 3},
        {"vs", ConfigField::KIND_INT, offsetof(SystemConfig, vSamples), 0, 12, "!!VAL_VS!!", 0},
        {"to", ConfigField::KIND_INT, offsetof(SystemConfig, bmsTimeout), 0, 60, "!!VAL_TO!!", 0},
        {"sps", ConfigField::KIND_UINT16, offsetof(SystemConfig, spreadStartMv), 0, 60, "!!VAL_SPS!!", 0},
        {"spm", ConfigField::KIND_UINT16, offsetof(SystemConfig, spreadMaxMv), 0, 150, "!!VAL_SPM!!", 0},
    };
}

WebDashboard::WebDashboard(uint16_t port)
    : _server(port), _events("/events"), _actionCb(nullptr), _debugCb(nullptr), _cfg(nullptr) {}

void WebDashboard::debugLog(const char *format, ...)
{
    if (!_debugCb)
        return;

    char loc_res[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_res, sizeof(loc_res), format, arg);
    va_end(arg);

    // "%s" as the format string, loc_res as its argument - not
    // _debugCb(loc_res) - so a log line containing a literal '%' (e.g. a
    // percentage) isn't reinterpreted as a format specifier by netLog's own
    // vsnprintf. Same guard as main.cpp's libraryLogger(): netLog("%s", msg).
    _debugCb("%s", loc_res);
}

void WebDashboard::begin()
{
    // /config and /save (registered by setupRoutes() below) dereference
    // _cfg; it's only set by loadConfig(), which setup() must call before
    // begin(). Refuse to start rather than serve routes that would crash on
    // a null deref if that ordering is ever broken.
    if (_cfg == nullptr)
    {
        debugLog("[WEB] begin() called before loadConfig() - server not started\n");
        return;
    }

    setupRoutes();

    _server.addHandler(&_events);
    _server.begin();
}

void WebDashboard::setActionCallback(ActionCallback cb)
{
    _actionCb = cb;
}

void WebDashboard::setDebugCallback(WebDebugCallback cb)
{
    _debugCb = cb;
}

void WebDashboard::broadcastLog(const char *msg)
{
    // Send string to the "log" event listener in the JS
    _events.send(msg, "log", millis());
}

// A change to TelemetrySchema::kColumns (#32) that adds/removes a column is
// exactly the kind of change broadcastTelemetry()'s hand-written JSON below
// needs a human to look at (its key set is meant to track the schema's
// jsonKey-bearing columns) - this fails the build as a nudge to check it,
// rather than silently drifting the way CSV header/row/JSON used to before
// TelemetrySchema.h existed.
static_assert(sizeof(TelemetrySchema::kColumns) / sizeof(TelemetrySchema::kColumns[0]) == 38,
              "TelemetrySchema column count changed - check broadcastTelemetry()'s hand-written JSON still matches");

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

    // Worst-case length, recounted for the #24 spreadMv/derate fields:
    // numeric fields at their widest (99.99/9.999/-999.9-valued) plus keys
    // ~150 bytes, a 20-char smam string ~29 bytes, cells:%s up to the
    // cellsStr buffer itself (256) ~264 bytes, spreadMv (5-digit uint16)
    // ~16 bytes and derate (0.00-1.00) ~13 bytes, plus punctuation - comes
    // to ~490 bytes. json[1024] keeps comfortable headroom above that.
    //
    // Kept hand-written rather than built field-by-field from
    // TelemetrySchema::kColumns (#32): most of these keys DO map onto a
    // CSV column with identical formatting (v/PackV, minC/MinCellV,
    // maxC/MaxCellV, minCellRaw/MinCellRaw, maxCellRaw/MaxCellRaw,
    // reqI/ReqI, soc/SOC, smam/Mode, maint/MaintenanceActive,
    // force/ForceCharge, spreadMv/RawSpreadMv, derate/Derate - see each
    // column's jsonKey in TelemetrySchema.h), but "i" is the one field that
    // doesn't: it's PackCurrent formatted to one decimal place here vs. two
    // in the CSV's PackI column, a genuine pre-existing difference between
    // the two outputs. cv/avgCellVoltage, isR/isResetting and the cells
    // array aren't CSV columns at all, and this object's key ORDER (part of
    // the byte-identical contract - see #32) doesn't match kColumns' CSV
    // order either. Reusing the column formatters here would either bake in
    // the wrong precision for "i" or require reordering these keys, so the
    // dedup stops at "same table defines which keys exist" (the static_assert
    // above) rather than "same code formats every value".
    char json[1024];
    snprintf(json, sizeof(json),
             "{\"v\":%.2f,\"cv\":%.3f,\"minC\":%.3f,\"maxC\":%.3f,\"minCellRaw\":%.3f,\"maxCellRaw\":%.3f,\"i\":%.1f,\"reqI\":%.1f,\"soc\":%.1f,\"smam\":\"%s\",\"maint\":%d,\"force\":%d,\"isR\":%d,\"spreadMv\":%u,\"derate\":%.2f,\"cells\":%s}",
             data.packVoltage, data.avgCellVoltage, data.minCellVoltage, data.maxCellVoltage,
             data.minCellVoltageRaw, data.maxCellVoltageRaw,
             data.packCurrent, data.requestedCurrent, data.packSOC,
             data.smaChargeMode, (int)data.maintenanceActive, (int)data.forceCharge,
             (int)data.isResetting, (unsigned)data.cellSpreadRawMv, data.derateFactor, cellsStr);

    _events.send(json, "data", millis());
}

void WebDashboard::loadConfig(SystemConfig &configOut)
{
    _cfg = &configOut;

    // No dataMutex needed here: this runs once from setup(), before bmsTask
    // or canTask exist, so there is no concurrent reader yet.
    _prefs.begin("bms-bridge", false);

    // Read from NVS or set defaults - see kConfigFields above for the key/
    // default list.
    for (const ConfigField &f : kConfigFields)
    {
        uint8_t *member = reinterpret_cast<uint8_t *>(_cfg) + f.offset;
        switch (f.kind)
        {
        case ConfigField::KIND_FLOAT:
            *reinterpret_cast<float *>(member) = _prefs.getFloat(f.key, f.defF);
            break;
        case ConfigField::KIND_INT:
            *reinterpret_cast<int *>(member) = _prefs.getInt(f.key, f.defI);
            break;
        case ConfigField::KIND_UINT16:
            *reinterpret_cast<uint16_t *>(member) = (uint16_t)_prefs.getUInt(f.key, (uint32_t)f.defI);
            break;
        }
    }

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
        debugLog("[WEB] /save refused: config busy\n");
        request->send(503, "text/plain", "Device busy, please try Save again");
        return;
    }

    _prefs.begin("bms-bridge", false);

    // "to" (bmsTimeout) has no form field in config_html, so hasParam(f.key)
    // is always false for it here - it just never gets touched, same as
    // before this refactor.
    for (const ConfigField &f : kConfigFields)
    {
        if (!request->hasParam(f.key))
            continue;

        const String &val = request->getParam(f.key)->value();
        uint8_t *member = reinterpret_cast<uint8_t *>(_cfg) + f.offset;
        switch (f.kind)
        {
        case ConfigField::KIND_FLOAT:
        {
            float v = val.toFloat();
            *reinterpret_cast<float *>(member) = v;
            _prefs.putFloat(f.key, v);
            break;
        }
        case ConfigField::KIND_INT:
        {
            int v = val.toInt();
            *reinterpret_cast<int *>(member) = v;
            _prefs.putInt(f.key, v);
            break;
        }
        case ConfigField::KIND_UINT16:
        {
            uint16_t v = (uint16_t)val.toInt();
            *reinterpret_cast<uint16_t *>(member) = v;
            _prefs.putUInt(f.key, v);
            break;
        }
        }
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
        for (const ConfigField &f : kConfigFields) {
            if (!f.placeholder) continue; // e.g. "to"/bmsTimeout - no UI field
            const uint8_t *member = reinterpret_cast<const uint8_t *>(_cfg) + f.offset;
            switch (f.kind) {
            case ConfigField::KIND_FLOAT:
                h.replace(f.placeholder, String(*reinterpret_cast<const float *>(member), (unsigned int)f.decimals));
                break;
            case ConfigField::KIND_INT:
                h.replace(f.placeholder, String(*reinterpret_cast<const int *>(member)));
                break;
            case ConfigField::KIND_UINT16:
                h.replace(f.placeholder, String(*reinterpret_cast<const uint16_t *>(member)));
                break;
            }
        }
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
    //
    // esp_core_dump_image_check() runs first (not just image_get()) so a
    // dump that's present-sized but fails its CRC (e.g. brownout mid-panic,
    // writing cut off partway) is refused with 409 instead of being streamed
    // as bytes espcoredump.py will reject anyway while the boot log claims
    // "no core dump".
    _server.on("/api/coredump", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        esp_err_t checkErr = esp_core_dump_image_check();
        if (checkErr == ESP_ERR_NOT_FOUND || checkErr == ESP_ERR_INVALID_SIZE) {
            request->send(404, "text/plain", "No core dump stored");
            return;
        }
        if (checkErr != ESP_OK) {
            char msg[96];
            snprintf(msg, sizeof(msg), "core dump present but corrupt: %.64s", esp_err_to_name(checkErr));
            request->send(409, "text/plain", msg);
            return;
        }

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