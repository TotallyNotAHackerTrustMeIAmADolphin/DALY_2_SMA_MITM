#include "Diagnostics.h"
#include <cstdarg>
#include <cstdio>
#include <WiFi.h>
#include "esp_core_dump.h"
#include "rom/rtc.h"
#include "RollbackConfirm.h"
#include "HealthLog.h"

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

    // "loopTask" is the Arduino core's name for the task running loop();
    // looked up by name like the others rather than via a NULL handle, so
    // the loop column stays right even if this is ever called elsewhere.
    static const char *const names[HealthLog::kNumTasks] = {"loopTask", "BMS_Task", "CAN_Task", "SD_LogTask", "async_tcp", "arduino_events"};
    static const char *const shortNames[HealthLog::kNumTasks] = {"loop", "bms", "can", "sd", "async_tcp", "events"};
    HealthLog::Sample s;
    s.freeHeap = ESP.getFreeHeap();
    s.minFreeHeap = ESP.getMinFreeHeap();
    s.maxBlock = ESP.getMaxAllocHeap();
    // On ESP-IDF the stack high-water mark is in bytes.
    for (int i = 0; i < HealthLog::kNumTasks; i++)
    {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        s.stackLeft[i] = h ? (uint32_t)uxTaskGetStackHighWaterMark(h) : HealthLog::kNoTask;
    }

    HealthLog::Decision d = HealthLog::decide(st, s, millis());
    if (d.reason == HealthLog::kNone)
        return;

    char stacks[HealthLog::kNumTasks][12];
    for (int i = 0; i < HealthLog::kNumTasks; i++)
    {
        if (s.stackLeft[i] == HealthLog::kNoTask)
            strlcpy(stacks[i], "-", sizeof(stacks[i]));
        else
            snprintf(stacks[i], sizeof(stacks[i]), "%u", (unsigned)s.stackLeft[i]);
    }

    char rssi[16];
    if (WiFi.status() == WL_CONNECTED)
        snprintf(rssi, sizeof(rssi), "%d dBm", (int)WiFi.RSSI());
    else
        strlcpy(rssi, "n/a", sizeof(rssi));

    char why[48];
    if (d.task >= 0)
        snprintf(why, sizeof(why), "%s: %s", HealthLog::reasonName(d.reason), shortNames[d.task]);
    else
        strlcpy(why, HealthLog::reasonName(d.reason), sizeof(why));

    debugLog("[DIAG] Health (%s): heap free %u, min %u, max block %u | stack left: loop %s, bms %s, can %s, sd %s, async_tcp %s, events %s | WiFi RSSI %s\n",
             why, (unsigned)s.freeHeap, (unsigned)s.minFreeHeap, (unsigned)s.maxBlock,
             stacks[HealthLog::kLoop], stacks[HealthLog::kBms], stacks[HealthLog::kCan],
             stacks[HealthLog::kSd], stacks[HealthLog::kAsyncTcp], stacks[HealthLog::kEvents],
             rssi);
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

    char elfSha[17] = {0};
    esp_ota_get_app_elf_sha256(elfSha, sizeof(elfSha));
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
        debugLog("[DIAG] Image pending verification: OTA is refused until it is confirmed (needs >= 120 s uptime with WiFi up and BMS data); a reset before that boots the previous firmware.\n");
    }

    // A dump stays in flash until the next crash overwrites it, so it can be
    // from an earlier crash than the one that caused this boot - compare its
    // ELF sha with the running one, and the reset reason above.
    static esp_core_dump_summary_t summary;
    esp_err_t chk = esp_core_dump_image_check();
    if (chk == ESP_OK && esp_core_dump_get_summary(&summary) == ESP_OK)
    {
        debugLog("[DIAG] Stored core dump: task '%s', PC 0x%08x, cause %u, vaddr 0x%08x, ELF %s\n",
                 summary.exc_task, (unsigned)summary.exc_pc,
                 (unsigned)summary.ex_info.exc_cause, (unsigned)summary.ex_info.exc_vaddr,
                 (const char *)summary.app_elf_sha256);

        char bt[200];
        size_t len = 0;
        bt[0] = '\0';
        for (uint32_t i = 0; i < summary.exc_bt_info.depth && i < 16 && len < sizeof(bt); i++)
            len += snprintf(bt + len, sizeof(bt) - len, " 0x%08x", (unsigned)summary.exc_bt_info.bt[i]);
        debugLog("[DIAG] Backtrace%s:%s\n", summary.exc_bt_info.corrupted ? " (corrupted)" : "", bt);
        debugLog("[DIAG] Full dump: GET /api/coredump\n");
    }
    else if (chk == ESP_ERR_NOT_FOUND || chk == ESP_ERR_INVALID_SIZE)
    {
        debugLog("[DIAG] No core dump stored.\n");
    }
    else
    {
        debugLog("[DIAG] Core dump present but unreadable (%s).\n", esp_err_to_name(chk));
    }
}

void Diagnostics::confirmImageIfReady(bool wifiUp, bool bmsUp)
{
    static RollbackConfirm::State st;

    // One clock read for both the guard below and decide(): with two reads,
    // the guard could see exactly kConfirmAfterMs (skip the OTA query) and
    // decide() one ms later (take the not-warned branch with
    // imagePendingVerify=false), silently swallowing the one-shot warning.
    const unsigned long nowMs = millis();

    // Only query the OTA partition state when the answer could actually
    // change the outcome: decide() is a no-op once imageConfirmed or before
    // kConfirmAfterMs, and it only consults imagePendingVerify on the
    // not-yet-warned, not-ready branch - so skip the ESP-IDF call everywhere
    // else instead of doing it unconditionally on every loop() iteration.
    bool imagePendingVerify = false;
    if (!st.imageConfirmed && !st.unconfirmedWarned && !(wifiUp && bmsUp) &&
        nowMs > RollbackConfirm::kConfirmAfterMs)
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
