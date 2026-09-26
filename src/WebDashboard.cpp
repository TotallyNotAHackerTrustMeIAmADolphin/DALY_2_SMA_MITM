#include "WebDashboard.h"
#include "WebPages.h"
#include "SDLogger.h"
#include "TelemetrySchema.h"
#include <SD.h>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"
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
    // `to` (bmsTimeout) is on the /config page since #35 (it was NVS-only
    // before). A field with placeholder = nullptr would be NVS-only again:
    // never substituted into the page, never present in a /save request.
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

    // One row per SystemConfig::ValidationResult flag that can be set, each
    // with a human-readable message naming the offending field(s) (#53).
    // Shared by saveConfig()'s 400 response (#55) and loadConfig()'s startup
    // log (#56) so the flag->message mapping only lives in one place; each
    // caller decides how to emit the lines (concatenated into one HTTP body,
    // or one netLog call per line).
    struct ValidationMessage
    {
        bool SystemConfig::ValidationResult::*flag;
        const char *message;
    };

    const ValidationMessage kValidationMessages[] = {
        {&SystemConfig::ValidationResult::hasNaN,
         "A config value is NaN (corrupted NVS?) - check every setting."},
        {&SystemConfig::ValidationResult::maintStartBelowMinDischarge,
         "Maint. Start Vpc must be above Min Discharge Vpc (#12)."},
        {&SystemConfig::ValidationResult::chargeTaperOrderBad,
         "Charge thresholds must be ordered: Start Taper Vpc < High Alarm Gate Vpc < Max Charge Vpc."},
        {&SystemConfig::ValidationResult::dischargeTaperOrderBad,
         "Discharge thresholds must be ordered: Start D-Taper Vpc > Low Alarm Gate Vpc > Min Discharge Vpc."},
        {&SystemConfig::ValidationResult::maintHysteresisBad,
         "Maint. Stop Vpc must be above Maint. Start Vpc (#12)."},
        {&SystemConfig::ValidationResult::chargeHeadroomBad,
         "Max Charge Vpc must be at most 3.550 V (Daly OV protection 3.65 V minus 100 mV margin, #8)."},
        {&SystemConfig::ValidationResult::hasNegativeCurrent,
         "Current setpoints (charge/discharge/trickle/limp/maintenance) must not be negative."},
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

    // #56: sanity-check whatever just got loaded (defaults or NVS content)
    // and log it - never blocks boot, just surfaces a corrupted/inconsistent
    // NVS setpoint set for a human to notice and fix via /config or /save.
    SystemConfig::ValidationResult validation = SystemConfig::validate(*_cfg);
    if (!validation.ok())
    {
        for (const ValidationMessage &vm : kValidationMessages)
        {
            if (validation.*(vm.flag))
                debugLog("[CFG] Loaded config fails validation: %s\n", vm.message);
        }
    }
}

void WebDashboard::saveConfig(AsyncWebServerRequest *request)
{
    // calculateCCL()/calculateDCL() in main.cpp's loop() read these same
    // _cfg fields while holding dataMutex. Taking the same mutex here makes
    // the whole set of field updates atomic from loop()'s point of view,
    // instead of a reader potentially seeing a mix of old and new setpoints
    // mid-save.
    //
    // The NVS write itself (Preferences - flash erase/write, tens of ms) is
    // deliberately done AFTER dataMutex is released (#21): canTask only
    // takes dataMutex for up to 20ms per 250ms SMA-frame cycle, and used to
    // time out and skip a frame if a save's flash write was still running
    // under the same lock. So: parse every submitted field into a local
    // SystemConfig copy (starting from *_cfg, so untouched fields keep
    // their current value) under the lock, publish it to *_cfg, release the
    // lock, then write only the submitted fields to NVS from that copy -
    // `present[]` (parallel to kConfigFields, by index) remembers which
    // fields were actually in the request, since a value in `copy` alone
    // can't distinguish "submitted, same as before" from "not submitted".
    constexpr size_t kNumFields = sizeof(kConfigFields) / sizeof(kConfigFields[0]);
    bool present[kNumFields] = {false};

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(300)) != pdTRUE)
    {
        debugLog("[WEB] /save refused: config busy\n");
        request->send(503, "text/plain", "Device busy, please try Save again");
        return;
    }

    SystemConfig copy = *_cfg;

    // Only fields present in the request are parsed and later written to
    // NVS; everything else keeps its value from *_cfg.
    for (size_t i = 0; i < kNumFields; i++)
    {
        const ConfigField &f = kConfigFields[i];
        if (!request->hasParam(f.key))
            continue;
        present[i] = true;

        const String &val = request->getParam(f.key)->value();
        uint8_t *member = reinterpret_cast<uint8_t *>(&copy) + f.offset;
        switch (f.kind)
        {
        case ConfigField::KIND_FLOAT:
            *reinterpret_cast<float *>(member) = val.toFloat();
            break;
        case ConfigField::KIND_INT:
            *reinterpret_cast<int *>(member) = val.toInt();
            break;
        case ConfigField::KIND_UINT16:
            *reinterpret_cast<uint16_t *>(member) = (uint16_t)val.toInt();
            break;
        }
    }

    // #53/#55: full sanity check (ordering, hysteresis, NaN, charge headroom
    // margin, negative current), not just the old single #12 comparison.
    SystemConfig::ValidationResult validation = SystemConfig::validate(copy);
    if (!validation.ok())
    {
        xSemaphoreGive(dataMutex);
        String body;
        for (const ValidationMessage &vm : kValidationMessages)
        {
            if (validation.*(vm.flag))
                body += String(vm.message) + "\n";
        }
        debugLog("[WEB] /save refused: config failed validation:\n%s", body.c_str());
        request->send(400, "text/plain", body + "Nothing saved.\n");
        return;
    }
    *_cfg = copy;
    xSemaphoreGive(dataMutex);

    _prefs.begin("bms-bridge", false);
    for (size_t i = 0; i < kNumFields; i++)
    {
        if (!present[i])
            continue;

        const ConfigField &f = kConfigFields[i];
        const uint8_t *member = reinterpret_cast<const uint8_t *>(&copy) + f.offset;
        switch (f.kind)
        {
        case ConfigField::KIND_FLOAT:
            _prefs.putFloat(f.key, *reinterpret_cast<const float *>(member));
            break;
        case ConfigField::KIND_INT:
            _prefs.putInt(f.key, *reinterpret_cast<const int *>(member));
            break;
        case ConfigField::KIND_UINT16:
            _prefs.putUInt(f.key, *reinterpret_cast<const uint16_t *>(member));
            break;
        }
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
            if (!f.placeholder) continue; // NVS-only field, no UI input
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
    //
    // Mapped once via esp_partition_mmap() (#21) instead of a per-chunk
    // esp_flash_read(): the previous filler's flash read disabled the cache
    // and stalled the other core for every single chunk. The mapping is
    // released exactly once - on normal completion AND on client abort -
    // via onDisconnect, same single-release pattern as /api/logs/download's
    // sdMutex release above (this library always closes non-keep-alive
    // file-response connections, so onDisconnect is a reliable single
    // release point).
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

        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        if (!part) {
            request->send(404, "text/plain", "No core dump stored");
            return;
        }

        // esp_core_dump_image_get()'s size describes the stored image and
        // should never exceed the partition it lives in - but if it ever
        // did (e.g. a corrupt size field slipping past image_check()'s CRC),
        // mapping/serving more than the partition actually holds must never
        // happen. Clamp once, here, and use this same value for the mmap
        // call, the filler's bound and Content-Length below, so those three
        // can't disagree with each other (#21) - unlike the old per-chunk
        // esp_flash_read() filler, whose only error path (a failed chunk
        // read) returned 0 mid-stream and left the response short against
        // an already-sent, un-clamped Content-Length.
        size_t mapSize = size < part->size ? size : part->size;

        const void *mapPtr = nullptr;
        spi_flash_mmap_handle_t mapHandle = 0;
        if (esp_partition_mmap(part, 0, mapSize, SPI_FLASH_MMAP_DATA, &mapPtr, &mapHandle) != ESP_OK) {
            request->send(500, "text/plain", "Failed to map core dump partition");
            return;
        }

        const uint8_t *base = static_cast<const uint8_t *>(mapPtr);
        request->onDisconnect([mapHandle]() { spi_flash_munmap(mapHandle); });

        AsyncWebServerResponse *response = request->beginResponse(
            "application/octet-stream", mapSize,
            [base, mapSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                // The only remaining "end" condition: index has reached the
                // clamped size. memcpy from an already-successful mmap
                // can't itself fail mid-stream the way esp_flash_read()
                // could, so there is no other error path left to handle.
                if (index >= mapSize)
                    return 0;
                size_t n = mapSize - index;
                if (n > maxLen)
                    n = maxLen;
                memcpy(buffer, base + index, n);
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