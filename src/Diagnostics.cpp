#include "Diagnostics.h"
#include <cstdarg>
#include <cstdio>
#include <WiFi.h>
#include "esp_core_dump.h"
#include "rom/rtc.h"

DiagDebugCallback Diagnostics::debugCb = nullptr;

extern "C" bool verifyRollbackLater() { return true; }

namespace
{
    constexpr unsigned long kConfirmAfterMs = 2UL * 60UL * 1000UL;
}

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
    TaskHandle_t bmsTask = xTaskGetHandle("BMS_Task");
    TaskHandle_t canTask = xTaskGetHandle("CAN_Task");
    TaskHandle_t asyncTcp = xTaskGetHandle("async_tcp");
    TaskHandle_t sdTask = xTaskGetHandle("SD_LogTask");
    TaskHandle_t evt = xTaskGetHandle("arduino_events");

    char rssi[16];
    if (WiFi.status() == WL_CONNECTED)
        snprintf(rssi, sizeof(rssi), "%d dBm", (int)WiFi.RSSI());
    else
        strlcpy(rssi, "n/a", sizeof(rssi));

    // On ESP-IDF the stack high-water mark is in bytes.
    debugLog("[DIAG] Heap free %u, min %u, max block %u | stack left: loop %u, bms %u, can %u, sd %u, async_tcp %u, events %u | WiFi RSSI %s\n",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             bmsTask ? (unsigned)uxTaskGetStackHighWaterMark(bmsTask) : 0u,
             canTask ? (unsigned)uxTaskGetStackHighWaterMark(canTask) : 0u,
             sdTask ? (unsigned)uxTaskGetStackHighWaterMark(sdTask) : 0u,
             asyncTcp ? (unsigned)uxTaskGetStackHighWaterMark(asyncTcp) : 0u,
             evt ? (unsigned)uxTaskGetStackHighWaterMark(evt) : 0u,
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
    static bool imageConfirmed = false;
    static bool unconfirmedWarned = false;
    if (imageConfirmed || millis() <= kConfirmAfterMs)
        return;

    if (wifiUp && bmsUp)
    {
        imageConfirmed = true;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        debugLog("[SYS] Firmware confirmed after %lus uptime with WiFi up and BMS data flowing (rollback cancelled): %s\n",
                 millis() / 1000, esp_err_to_name(err));
    }
    else if (!unconfirmedWarned)
    {
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        esp_ota_get_state_partition(esp_ota_get_running_partition(), &st);
        if (st == ESP_OTA_IMG_PENDING_VERIFY)
        {
            unconfirmedWarned = true;
            debugLog("[SYS] Firmware NOT confirmed at %lus: %s%s- a reset now boots the previous firmware\n",
                     millis() / 1000, wifiUp ? "" : "WiFi down ", bmsUp ? "" : "no BMS data ");
        }
    }
}
