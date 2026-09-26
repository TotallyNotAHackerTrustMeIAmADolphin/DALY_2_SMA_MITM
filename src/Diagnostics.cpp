#include "Diagnostics.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <WiFi.h>
#include "esp_core_dump.h"
#include "rom/rtc.h"
#include "RollbackConfirm.h"
#include "HealthLog.h"
#include "SDLogger.h"

DiagDebugCallback Diagnostics::debugCb = nullptr;

extern "C" bool verifyRollbackLater() { return true; }

void Diagnostics::setDebugCallback(DiagDebugCallback cb)
{
    debugCb = cb;
}

void Diagnostics::debugLog(const char *format, ...)
{
    if (!debugCb)
        return;

    char loc_res[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_res, sizeof(loc_res), format, arg);
    va_end(arg);

    debugCb("%s", loc_res);
}

const char *Diagnostics::otaStateName(esp_ota_img_states_t s)
{
    switch (s)
    {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify (rollback armed)";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined (no rollback info, e.g. USB-flashed)";
    }
}

const char *Diagnostics::resetReasonName(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart (e.g. OTA)";
    case ESP_RST_PANIC: return "PANIC (crash)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply dip)";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
    }
}

void Diagnostics::logHealth()
{
    // Sampled every 10 minutes from loop(), but only logged when
    // HealthLog::decide() says something moved (see include/HealthLog.h).
    static HealthLog::State st;

    HealthLog::Sample s;
    s.freeHeap = ESP.getFreeHeap();
    s.minFreeHeap = ESP.getMinFreeHeap();
    s.maxBlock = ESP.getMaxAllocHeap();
    // On ESP-IDF the stack high-water mark is in bytes. "loopTask" is the
    // Arduino core's name for the task running loop(); looked up by name
    // like the others rather than via a NULL handle, so the loop column
    // stays right even if this is ever called elsewhere.
    for (int i = 0; i < HealthLog::kNumTasks; i++)
    {
        TaskHandle_t h = xTaskGetHandle(HealthLog::taskNames()[i].full);
        s.stackLeft[i] = h ? (uint32_t)uxTaskGetStackHighWaterMark(h) : HealthLog::kNoTask;
    }

    SDLogger::Stats sdStats = SDLogger::stats();
    s.sdDroppedQueueFull = sdStats.droppedQueueFull;
    s.sdDroppedLockTimeout = sdStats.droppedLockTimeout;
    s.sdWriteFailures = sdStats.writeFailures;

    HealthLog::Decision d = HealthLog::decide(st, s, millis());
    if (d.reason == HealthLog::kNone)
        return;

    char stacks[160];
    BoundedWriter stacksW(stacks, sizeof(stacks));
    for (int i = 0; i < HealthLog::kNumTasks; i++)
    {
        const char *sep = i ? ", " : "";
        if (s.stackLeft[i] == HealthLog::kNoTask)
            stacksW.append("%s%s -", sep, HealthLog::taskNames()[i].shortForm);
        else
            stacksW.append("%s%s %u", sep, HealthLog::taskNames()[i].shortForm, (unsigned)s.stackLeft[i]);
    }

    char rssi[16];
    if (WiFi.status() == WL_CONNECTED)
        snprintf(rssi, sizeof(rssi), "%d dBm", (int)WiFi.RSSI());
    else
        strlcpy(rssi, "n/a", sizeof(rssi));

    char why[48];
    if (d.task >= 0)
        snprintf(why, sizeof(why), "%s: %s", HealthLog::reasonName(d.reason), HealthLog::taskNames()[d.task].shortForm);
    else
        strlcpy(why, HealthLog::reasonName(d.reason), sizeof(why));

    debugLog("[DIAG] Health (%s): heap free %u, min %u, max block %u | stack left: %s | WiFi RSSI %s | SD drops: queue %u, lock %u, write %u\n",
             why, (unsigned)s.freeHeap, (unsigned)s.minFreeHeap, (unsigned)s.maxBlock,
             stacks, rssi, (unsigned)s.sdDroppedQueueFull, (unsigned)s.sdDroppedLockTimeout, (unsigned)s.sdWriteFailures);
}

// Why did we (re)boot, and what does the last stored core dump say? Called
// directly from setup() right after SD init, so even a reset within the
// first 60s of a cold boot (or a crash loop) still gets its reason onto the
// SD log - it doesn't wait for loop()'s clock-ready gate.
void Diagnostics::logBootDiagnostics()
{
    esp_reset_reason_t reason = esp_reset_reason();
    debugLog("[DIAG] Reset reason: %s (%d), core0 %d, core1 %d\n",
             resetReasonName(reason), (int)reason,
             (int)rtc_get_reset_reason(0), (int)rtc_get_reset_reason(1));

    char elfSha[17];
    runningElfSha(elfSha);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &otaState);
    debugLog("[DIAG] Running firmware ELF sha256: %s, partition %s, image state: %s\n",
             elfSha, running ? running->label : "?", otaStateName(otaState));

    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad)
    {
        debugLog("[DIAG] ROLLED BACK: partition %s is invalid/aborted - running the previous firmware\n", bad->label);
    }

    if (otaState == ESP_OTA_IMG_PENDING_VERIFY)
    {
        debugLog("[DIAG] Image pending verification: OTA is refused until it is confirmed (needs >= %lu s uptime with WiFi up and BMS data); a reset before that boots the previous firmware.\n",
                 RollbackConfirm::kConfirmAfterMs / 1000);
    }

    // A dump stays in flash until the next crash overwrites it, so it can be
    // from an earlier crash than the one that caused this boot - compare its
    // ELF sha with the running one, and the reset reason above.
    CoreDumpInfo dump;
    readCoreDump(dump);
    switch (dump.status)
    {
    case CoreDumpInfo::Present:
    {
        debugLog("[DIAG] Stored core dump: task '%s', PC 0x%08x, cause %u, vaddr 0x%08x, ELF %s\n",
                 dump.task, (unsigned)dump.pc, (unsigned)dump.cause, (unsigned)dump.vaddr, dump.elfSha);

        char bt[200];
        BoundedWriter w(bt, sizeof(bt));
        for (int i = 0; i < dump.backtraceDepth; i++)
            w.append(" 0x%08x", (unsigned)dump.backtrace[i]);
        debugLog("[DIAG] Backtrace%s:%s\n", dump.corrupted ? " (corrupted)" : "", bt);
        debugLog("[DIAG] Full dump: GET /api/coredump\n");
        break;
    }
    case CoreDumpInfo::None:
        debugLog("[DIAG] No core dump stored.\n");
        break;
    case CoreDumpInfo::Unreadable:
        debugLog("[DIAG] Core dump present but unreadable (%s).\n", dump.checkName);
        break;
    }
}

void Diagnostics::readCoreDump(CoreDumpInfo &out)
{
    out = CoreDumpInfo{};
    esp_err_t chk = esp_core_dump_image_check();
    out.checkName = esp_err_to_name(chk);
    if (coreDumpAbsent(chk))
    {
        out.status = CoreDumpInfo::None;
        return;
    }

    // Static: the summary struct is large for a task stack.
    static esp_core_dump_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    if (chk != ESP_OK || esp_core_dump_get_summary(&summary) != ESP_OK)
    {
        out.status = CoreDumpInfo::Unreadable;
        return;
    }

    out.status = CoreDumpInfo::Present;
    snprintf(out.task, sizeof(out.task), "%.16s", summary.exc_task);
    out.pc = summary.exc_pc;
    out.cause = summary.ex_info.exc_cause;
    out.vaddr = summary.ex_info.exc_vaddr;
    out.corrupted = summary.exc_bt_info.corrupted;
    out.backtraceDepth = summary.exc_bt_info.depth < (uint32_t)CoreDumpInfo::kMaxBacktrace
                             ? (int)summary.exc_bt_info.depth
                             : CoreDumpInfo::kMaxBacktrace;
    for (int i = 0; i < out.backtraceDepth; i++)
        out.backtrace[i] = summary.exc_bt_info.bt[i];
    snprintf(out.elfSha, sizeof(out.elfSha), "%.16s", (const char *)summary.app_elf_sha256);
}

void Diagnostics::runningElfSha(char (&out)[17])
{
    out[0] = '\0';
    esp_ota_get_app_elf_sha256(out, sizeof(out));
}

// File scope (was a function-local static in confirmImageIfReady()) so
// confirmCheckDue() can read it too.
static RollbackConfirm::State s_rollbackState;

bool Diagnostics::confirmCheckDue()
{
    return RollbackConfirm::needsInputs(s_rollbackState, millis());
}

void Diagnostics::confirmImageIfReady(bool wifiUp, bool bmsUp)
{
    RollbackConfirm::State &st = s_rollbackState;

    // One clock read for both the guard below and decide(): with two reads,
    // the guard could see exactly kConfirmAfterMs (skip the OTA query) and
    // decide() one ms later (take the not-warned branch with
    // imagePendingVerify=false), silently swallowing the one-shot warning.
    const unsigned long nowMs = millis();

    // Only query the OTA partition state when decide() will read it.
    bool imagePendingVerify = false;
    if (RollbackConfirm::needsPendingVerify(st, wifiUp, bmsUp, nowMs))
    {
        esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
        esp_ota_get_state_partition(esp_ota_get_running_partition(), &otaState);
        imagePendingVerify = (otaState == ESP_OTA_IMG_PENDING_VERIFY);
    }

    RollbackConfirm::Action action = RollbackConfirm::decide(st, wifiUp, bmsUp, nowMs, imagePendingVerify);

    if (action.confirmNow)
    {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        debugLog("[SYS] Firmware confirmed after %lus uptime with WiFi up and BMS data flowing (rollback cancelled): %s\n",
                 millis() / 1000, esp_err_to_name(err));
    }
    else if (action.logNotConfirmed)
    {
        debugLog("[SYS] Firmware NOT confirmed at %lus: %s%s- a reset now boots the previous firmware\n",
                 millis() / 1000, wifiUp ? "" : "WiFi down ", bmsUp ? "" : "no BMS data ");
    }
}
