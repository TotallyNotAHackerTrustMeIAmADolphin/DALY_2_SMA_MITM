#include "WebDashboard.h"
#include "WebPages.h"
#include "SDLogger.h"
#include "TelemetrySchema.h"
#include "Diagnostics.h"
#include "SettingFormat.h"
#include "ConfigStore.h"
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
    // Every setting is a Setting<T> member of SystemConfig (include/
    // SystemConfig.h) that carries its own key, label, unit, range and
    // default, and whose set() refuses anything outside that range.
    // ConfigStore::load(), saveConfig() and the /config page just loop over
    // cfg.all().

    // A value or limit of s, formatted with its display precision - for the
    // /config page and range text, not the [CFG] log (see formatSettingValue).
    String formatNumber(const SettingBase &s, double v)
    {
        char buf[24];
        formatSettingFixed(s, v, buf, sizeof(buf));
        return String(buf);
    }

    // "Max Charge Vpc must be between 2.500 and 3.550 V."
    String rangeMessage(const SettingBase &s)
    {
        return String(s.label()) + " must be between " + formatNumber(s, s.min()) + " and " +
               formatNumber(s, s.max()) + " " + s.unit() + ".";
    }

    // The setting's <input> plus a "min-max unit - default" line under it,
    // so the browser enforces the same range as set() and the page can't
    // state a range or default the firmware doesn't use. Always with a min:
    // without one the browser counts steps from the stored value (stored
    // 51, step 5: typing 60 was refused).
    String inputTag(const SettingBase &s)
    {
        return String("<div class=\"field\"><input type=\"number\" name=\"") + s.key() + "\" step=\"" + s.step() +
               "\" min=\"" + formatNumber(s, s.min()) + "\" max=\"" + formatNumber(s, s.max()) +
               "\" value=\"" + formatNumber(s, s.value()) + "\"><span class=\"range\">" +
               formatNumber(s, s.min()) + "&ndash;" + formatNumber(s, s.max()) + " " + s.unit() +
               " &middot; default " + formatNumber(s, s.def()) + "</span></div>";
    }
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

void WebDashboard::begin(SystemConfig *cfg)
{
    // /config and /save (registered by setupRoutes() below) dereference
    // _cfg. Refuse to start rather than serve routes that would crash on a
    // null deref.
    if (cfg == nullptr)
    {
        debugLog("[WEB] begin() called with a null config - server not started\n");
        return;
    }
    _cfg = cfg;

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

void WebDashboard::saveConfig(AsyncWebServerRequest *request)
{
    // canTask reads *_cfg under dataMutex and only waits 20 ms for it per
    // 250 ms SMA frame, so this holds the lock for nothing but the publish
    // (`*_cfg = copy`): parsing, validation and building the error text
    // (heap allocations) run unlocked, on a local copy. That's safe because
    // this handler is the only writer of *_cfg and always runs on the
    // async_tcp task, so *_cfg can't change between the copy and the
    // publish, and reading it here without the lock races no writer. The
    // NVS write (flash erase/write, tens of ms) also runs after the lock is
    // released (#21). `present[]` (parallel to all()) remembers which
    // settings were in the request, so only those are written to NVS.
    bool present[SystemConfig::kNumSettings] = {false};

    // Captured before copy is mutated below, so it holds the pre-save value
    // of every setting for the post-save "[CFG] <label>: <old> -> <new>"
    // log lines - reading *_cfg here without the lock is fine, per the
    // comment above (this handler is the only writer).
    const SystemConfig before = *_cfg;
    SystemConfig copy = *_cfg;
    std::array<SettingBase *, SystemConfig::kNumSettings> settings = copy.all();
    std::vector<String> errors;

    // Only settings present in the request are parsed (Setting::parse()
    // range-checks through set()) and later written to NVS; everything
    // else keeps its value from *_cfg.
    for (size_t i = 0; i < settings.size(); i++)
    {
        SettingBase &s = *settings[i];
        if (!request->hasParam(s.key()))
            continue;
        present[i] = true;

        switch (s.parse(request->getParam(s.key())->value().c_str()))
        {
        case SettingBase::ParseResult::Ok:
            break;
        case SettingBase::ParseResult::NotANumber:
            errors.push_back(String(s.label()) + " is not a valid number.");
            break;
        case SettingBase::ParseResult::OutOfRange:
            errors.push_back(rangeMessage(s));
            break;
        }
    }

    // #53/#55: the rules relating two settings. Only once every setting
    // parsed: otherwise the copy still holds the old value of a rejected
    // one, and a rule about it would describe a config nobody submitted.
    if (errors.empty())
    {
        SystemConfig::ValidationResult validation = SystemConfig::validate(copy);
        validation.forEachMessage([&errors](const char *msg)
                                   { errors.push_back(String(msg)); });
    }

    if (!errors.empty())
    {
        String body;
        for (const String &msg : errors)
        {
            body += msg + "\n";
            // One log line per violation: debugLog's buffer is 256 bytes,
            // and one call with embedded newlines would both truncate and
            // leave the continuation lines untimestamped.
            debugLog("[WEB] /save refused: %s\n", msg.c_str());
        }
        request->send(400, "text/plain", body + "Nothing saved.\n");
        return;
    }
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(300)) != pdTRUE)
    {
        debugLog("[WEB] /save refused: config busy\n");
        request->send(503, "text/plain", "Device busy, please try Save again");
        return;
    }
    *_cfg = copy;
    xSemaphoreGive(dataMutex);

    ConfigStore::store(copy, present);

    // Log every setting that actually changed, by name, old -> new - only
    // now that the save has fully succeeded (published under the lock and
    // written to NVS), never for a refused save. One debugLog call per
    // changed setting: its buffer is 256 bytes, so one call with embedded
    // newlines would both truncate and leave continuation lines
    // untimestamped, same reasoning as the /save-refused loop above.
    uint32_t changed = SystemConfig::changedMask(before, copy);
    if (changed == 0)
    {
        debugLog("[CFG] Saved, no changes\n");
    }
    else
    {
        std::array<const SettingBase *, SystemConfig::kNumSettings> beforeSettings = before.all();
        for (size_t i = 0; i < settings.size(); i++)
        {
            if (!(changed & ((uint32_t)1 << i)))
                continue;
            const SettingBase &s = *settings[i];
            char oldBuf[24];
            char newBuf[24];
            formatSettingValue(*beforeSettings[i], beforeSettings[i]->value(), oldBuf, sizeof(oldBuf));
            formatSettingValue(s, s.value(), newBuf, sizeof(newBuf));
            debugLog("[CFG] %s: %s -> %s %s\n", s.label(), oldBuf, newBuf, s.unit());
        }
    }

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
    std::vector<SDLogger::LogFileInfo> files;
    if (!SDLogger::listLogFiles(files))
    {
        // Distinguish "SD card busy/not ready" from "genuinely no such file"
        // below - otherwise a transient lock timeout looks like a 404 and
        // misleads anyone debugging it.
        request->send(503, "text/plain", "SD card busy, try again");
        return false;
    }

    for (const SDLogger::LogFileInfo &f : files)
    {
        if (f.name == requested)
        {
            outName = f.name;
            outSize = f.size;
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

namespace
{
    // ReadResult -> status code, once, for both /api/logs/content and
    // /api/logs/graph. tooLargeBody differs per route (different caps).
    void sendReadFailure(AsyncWebServerRequest *request, SDLogger::ReadResult r, const char *tooLargeBody)
    {
        switch (r)
        {
        case SDLogger::ReadResult::Busy:
            request->send(503, "text/plain", "SD card busy, try again");
            return;
        case SDLogger::ReadResult::NotFound:
            request->send(404, "text/plain", "Unknown log file");
            return;
        case SDLogger::ReadResult::TooLarge:
            request->send(413, "text/plain", tooLargeBody);
            return;
        case SDLogger::ReadResult::Ok:
            return;
        }
    }
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
        // Each setting's label and <input> (value, min, max, step, range
        // line) come from the Setting itself, via !!LABEL_<key>!! and
        // !!IN_<key>!!.
        for (const SettingBase *s : static_cast<const SystemConfig *>(_cfg)->all()) {
            h.replace(String("!!LABEL_") + s->key() + "!!", s->label());
            h.replace(String("!!IN_") + s->key() + "!!", inputTag(*s));
        }
        request->send(200, "text/html", h); });

    _server.on("/save", HTTP_GET, [this](AsyncWebServerRequest *request)
               { saveConfig(request); });

    _server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(200, "text/html", logs_html); });

    _server.on("/api/logs/list", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        std::vector<SDLogger::LogFileInfo> files;
        // Same 503 as findLogFile(): a busy card must not read as "no log
        // files yet" (#67).
        if (!SDLogger::listLogFiles(files)) {
            request->send(503, "text/plain", "SD card busy, try again");
            return;
        }

        String json = "[";
        for (size_t i = 0; i < files.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"name\":\"" + files[i].name + "\",\"size\":" + String(files[i].size) + "}";
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
        SDLogger::ReadResult r = SDLogger::readTail(name, content, maxBytes);
        if (r != SDLogger::ReadResult::Ok) {
            sendReadFailure(request, r, "File too large to view (over 512KB) - try Download instead");
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

        String sdPath;
        auto lock = SDLogger::beginDownload(name, sdPath);
        if (!lock) {
            request->send(503, "text/plain", "SD card busy, try again");
            return;
        }

        // Held for the whole transfer (accepted tradeoff: the background
        // writer drops samples it can't log during that window). Force a
        // stalled client's disconnect within 5s so the lock can't be held
        // indefinitely; resetting the shared_ptr in onDisconnect releases
        // it exactly once, whichever side closes the connection.
        request->client()->setAckTimeout(5000);
        request->onDisconnect([lock]() mutable { lock.reset(); });

        request->send(SD, sdPath, contentTypeForLogFile(name), true /* download */); });

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
        CoreDumpInfo dump;
        Diagnostics::readCoreDump(dump);
        char runningSha[17];
        Diagnostics::runningElfSha(runningSha);
        esp_reset_reason_t reason = esp_reset_reason();

        char json[900];
        if (!formatCoreDumpJson(dump, (int)reason, Diagnostics::resetReasonName(reason), runningSha, json, sizeof(json))) {
            request->send(500, "text/plain", "coredump summary too large");
            return;
        }
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
        if (Diagnostics::coreDumpAbsent(checkErr)) {
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
        SDLogger::ReadResult r = SDLogger::readGraphSeries(name, 300, csv);
        if (r != SDLogger::ReadResult::Ok) {
            sendReadFailure(request, r, "Failed to read file (it may be too large to graph - try Download instead)");
            return;
        }

        request->send(200, "text/csv", csv); });
}