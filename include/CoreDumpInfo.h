#pragma once

// The stored core dump's headline, in plain types so the JSON for
// /api/coredump/summary can be built and tested natively. Diagnostics::
// readCoreDump() fills it from ESP-IDF; the boot log and the web route both
// use that one reader.

#include <stdint.h>
#include <stdio.h>
#include "BoundedWriter.h"

struct CoreDumpInfo
{
    static constexpr int kMaxBacktrace = 16;

    enum Status
    {
        None,       // nothing stored
        Unreadable, // something stored, but it fails its check or summary
        Present,
    };

    Status status = None;
    const char *checkName = ""; // esp_err_to_name(esp_core_dump_image_check())
    char task[17] = {};
    uint32_t pc = 0;
    uint32_t cause = 0;
    uint32_t vaddr = 0;
    bool corrupted = false;
    uint32_t backtrace[kMaxBacktrace] = {};
    int backtraceDepth = 0;
    char elfSha[17] = {}; // first 16 hex chars of the crashed image's ELF sha256
};

// The /api/coredump/summary JSON. Returns false if it doesn't fit.
inline bool formatCoreDumpJson(const CoreDumpInfo &d, int resetReason, const char *resetReasonName,
                               const char *runningElfSha, char *buf, size_t len)
{
    BoundedWriter w(buf, len);
    w.append("{\"check\":\"%.32s\",\"present\":%s", d.checkName, d.status == CoreDumpInfo::Present ? "true" : "false");
    if (d.status == CoreDumpInfo::Present)
    {
        w.append(",\"task\":\"%.16s\",\"pc\":\"0x%08x\",\"cause\":%u,\"corrupted\":%s,\"backtrace\":[",
                 d.task, (unsigned)d.pc, (unsigned)d.cause, d.corrupted ? "true" : "false");
        for (int i = 0; i < d.backtraceDepth && i < CoreDumpInfo::kMaxBacktrace; i++)
            w.append("%s\"0x%08x\"", i ? "," : "", (unsigned)d.backtrace[i]);
        w.append("],\"crash_elf_sha256\":\"%.16s\"", d.elfSha);
    }
    w.append(",\"reset_reason\":%d,\"reset_reason_name\":\"%s\",\"running_elf_sha256\":\"%.16s\"}",
             resetReason, resetReasonName, runningElfSha);
    return w.ok();
}
