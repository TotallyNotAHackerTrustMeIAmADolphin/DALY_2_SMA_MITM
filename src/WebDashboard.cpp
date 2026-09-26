#include "WebDashboard.h"
#include "WebPages.h"
#include "SDLogger.h"
#include "TelemetryJson.h"
#include "Diagnostics.h"
#include "SettingFormat.h"
#include "ConfigStore.h"
#include "ConfigForm.h"
#include <SD.h>
#include <cstddef>

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
    : _server(port), _events("/events") {}

void WebDashboard::begin(SystemConfig *cfg)
{
    // /config and /save (registered by setupRoutes() below) dereference
    // _cfg. Refuse to start rather than serve routes that would crash on a
    // null deref.
    if (cfg == nullptr)
    {
        logf(_debugCb, "[WEB] begin() called with a null config - server not started\n");
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

void WebDashboard::setDebugCallback(LogSink cb)
{
    _debugCb = cb;
}

void WebDashboard::broadcastLog(const char *msg)
{
    // Send string to the "log" event listener in the JS
    _events.send(msg, "log", millis());
}

void WebDashboard::broadcastTelemetry(const DashboardData &data)
{
    char json[1024]; // TelemetryJson::format() needs ~500 B; see its own comment.
    if (TelemetryJson::format(data, json, sizeof(json)))
        _events.send(json, "data", millis());
}

void WebDashboard::saveConfig(AsyncWebServerRequest *request)
{
    // Captured before copy is mutated below, so it holds the pre-save value
    // of every setting for the post-save "[CFG] <label>: <old> -> <new>"
    // log lines - reading *_cfg here without the lock is fine (this handler
    // is the only writer of *_cfg and always runs on the async_tcp task, so
    // *_cfg can't change underneath it).
    const SystemConfig before = *_cfg;
    SystemConfig copy = *_cfg;

    std::vector<String> errors;
    ConfigForm::Result result = ConfigForm::apply(
        copy,
        [request](const char *key) -> const char *
        { return request->hasParam(key) ? request->getParam(key)->value().c_str() : nullptr; },
        [&errors](const char *msg)
        { errors.push_back(String(msg)); });

    if (!result.ok)
    {
        String body;
        for (const String &msg : errors)
        {
            body += msg + "\n";
            // One log line per violation: logf's buffer is 256 bytes, and
            // one call with embedded newlines would both truncate and leave
            // the continuation lines untimestamped.
            logf(_debugCb, "[WEB] /save refused: %s\n", msg.c_str());
        }
        request->send(400, "text/plain", body + "Nothing saved.\n");
        return;
    }

    // canTask reads *_cfg under dataMutex and only waits 20 ms for it per
    // 250 ms SMA frame, so this holds the lock for nothing but the publish
    // itself; the NVS write (flash erase/write, tens of ms) runs after the
    // lock is released (#21).
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(300)) != pdTRUE)
    {
        logf(_debugCb, "[WEB] /save refused: config busy\n");
        request->send(503, "text/plain", "Device busy, please try Save again");
        return;
    }
    *_cfg = copy;
    xSemaphoreGive(dataMutex);

    ConfigStore::store(copy, result.present);

    // Only now that the save has fully succeeded (published under the lock
    // and written to NVS), never for a refused save.
    ConfigForm::logChanges(before, copy, [this](const char *line)
                            { logf(_debugCb, "%s", line); });

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

void WebDashboard::handleIndex(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", index_html);
}

namespace
{
    // Both action routes differ only in which UiAction they apply.
    void respondToAction(AsyncWebServerRequest *request, ActionCallback cb, UiAction action)
    {
        bool applied = cb && cb(action);
        request->send(applied ? 200 : 503, "text/plain", applied ? "OK" : "Device busy, try again");
    }
}

void WebDashboard::handleToggleMaint(AsyncWebServerRequest *request)
{
    respondToAction(request, _actionCb, UiAction::ToggleMaint);
}

void WebDashboard::handleResetSMA(AsyncWebServerRequest *request)
{
    respondToAction(request, _actionCb, UiAction::ResetSma);
}

void WebDashboard::handleConfigPage(AsyncWebServerRequest *request)
{
    String h = String(config_html);
    // Each setting's label and <input> (value, min, max, step, range line)
    // come from the Setting itself, via !!LABEL_<key>!! and !!IN_<key>!!.
    for (const SettingBase *s : static_cast<const SystemConfig *>(_cfg)->all())
    {
        h.replace(String("!!LABEL_") + s->key() + "!!", s->label());
        h.replace(String("!!IN_") + s->key() + "!!", inputTag(*s));
    }
    request->send(200, "text/html", h);
}

void WebDashboard::handleLogsPage(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", logs_html);
}

void WebDashboard::handleGraphsPage(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", graphs_html);
}

void WebDashboard::handleLogList(AsyncWebServerRequest *request)
{
    std::vector<SDLogger::LogFileInfo> files;
    // Same 503 as findLogFile(): a busy card must not read as "no log
    // files yet" (#67).
    if (!SDLogger::listLogFiles(files))
    {
        request->send(503, "text/plain", "SD card busy, try again");
        return;
    }

    String json = "[";
    for (size_t i = 0; i < files.size(); i++)
    {
        if (i > 0)
            json += ",";
        json += "{\"name\":\"" + files[i].name + "\",\"size\":" + String(files[i].size) + "}";
    }
    json += "]";
    request->send(200, "application/json", json);
}

void WebDashboard::handleLogContent(AsyncWebServerRequest *request)
{
    String name;
    uint32_t size;
    if (!findLogFile(request, name, size))
        return;

    // Deliberately conservative, not "as much as fits in RAM": this is a
    // single contiguous String, copied again internally by beginResponse().
    // Live-tested at 64KB with 80KB+ free heap and it still failed -
    // getMaxAllocHeap() showed only ~43KB was actually contiguous, so the
    // copy silently produced an empty String (no error, just
    // content-length: 0) rather than the requested tail. 8KB stays
    // comfortably under real-world fragmentation on this device. A
    // chunked/streaming response (like /api/logs/download already uses)
    // would remove this ceiling entirely if a larger tail is ever needed.
    const size_t maxBytes = 8192;
    String content;
    SDLogger::ReadResult r = SDLogger::readTail(name, content, maxBytes);
    if (r != SDLogger::ReadResult::Ok)
    {
        sendReadFailure(request, r, "File too large to view (over 512KB) - try Download instead");
        return;
    }

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", content);
    if (size > maxBytes)
    {
        response->addHeader("X-Truncated", "1");
    }
    request->send(response);
}

void WebDashboard::handleLogDownload(AsyncWebServerRequest *request)
{
    String name;
    uint32_t size;
    if (!findLogFile(request, name, size))
        return;

    String sdPath;
    auto lock = SDLogger::beginDownload(name, sdPath);
    if (!lock)
    {
        request->send(503, "text/plain", "SD card busy, try again");
        return;
    }

    // Held for the whole transfer (accepted tradeoff: the background writer
    // drops samples it can't log during that window). Force a stalled
    // client's disconnect within 5s so the lock can't be held indefinitely;
    // resetting the shared_ptr in onDisconnect releases it exactly once,
    // whichever side closes the connection.
    request->client()->setAckTimeout(5000);
    request->onDisconnect([lock]() mutable
                           { lock.reset(); });

    request->send(SD, sdPath, contentTypeForLogFile(name), true /* download */);
}

void WebDashboard::handleGraph(AsyncWebServerRequest *request)
{
    String name;
    uint32_t size;
    if (!findLogFile(request, name, size))
        return;

    if (!name.endsWith(".csv"))
    {
        request->send(400, "text/plain", "Not a telemetry CSV file");
        return;
    }

    // 300, not readGraphSeries' own 2000-point ceiling: request->send()
    // needs this whole CSV as one contiguous String, same as
    // /api/logs/content - live-tested at 600 points (~42KB) with a
    // fragmented heap (getMaxAllocHeap() well under that) and it came back
    // as a silent empty 200 response. 300 points keeps output small enough
    // to reliably fit, and is still plenty of resolution for a trend chart
    // at typical browser widths.
    String csv;
    SDLogger::ReadResult r = SDLogger::readGraphSeries(name, 300, csv);
    if (r != SDLogger::ReadResult::Ok)
    {
        sendReadFailure(request, r, "Failed to read file (it may be too large to graph - try Download instead)");
        return;
    }

    request->send(200, "text/csv", csv);
}

void WebDashboard::setupRoutes()
{
    _server.on("/", HTTP_GET, handleIndex);
    _server.on("/toggleMaint", HTTP_POST, [this](AsyncWebServerRequest *r)
               { handleToggleMaint(r); });
    _server.on("/resetSMA", HTTP_POST, [this](AsyncWebServerRequest *r)
               { handleResetSMA(r); });
    _server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *r)
               { handleConfigPage(r); });
    _server.on("/save", HTTP_GET, [this](AsyncWebServerRequest *r)
               { saveConfig(r); });
    _server.on("/logs", HTTP_GET, handleLogsPage);
    _server.on("/api/logs/list", HTTP_GET, handleLogList);
    _server.on("/api/logs/content", HTTP_GET, handleLogContent);
    _server.on("/api/logs/download", HTTP_GET, handleLogDownload);
    // Both coredump routes, and the comment on why their registration order
    // matters, live in Diagnostics::registerRoutes() (#90) - neither is web
    // code.
    Diagnostics::registerRoutes(_server);
    _server.on("/graphs", HTTP_GET, handleGraphsPage);
    _server.on("/api/logs/graph", HTTP_GET, handleGraph);
}