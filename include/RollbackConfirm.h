#pragma once

// The OTA rollback-confirmation decision, kept free of Arduino/FreeRTOS/
// ESP-IDF dependencies so test/test_rollbackconfirm runs *this* code
// natively (`pio test -e native`). Diagnostics::confirmImageIfReady() is a
// thin wrapper: it holds the static State, calls decide(), and performs
// the resulting side effect (esp_ota_mark_app_valid_cancel_rollback() + a
// log line).

namespace RollbackConfirm
{
    // Uptime deadline (ms) before the image can be confirmed or the
    // "not confirmed" warning can fire.
    constexpr unsigned long kConfirmAfterMs = 2UL * 60UL * 1000UL;

    // Persistent between calls; owned by Diagnostics::confirmImageIfReady()
    // as a function-local `static`.
    struct State
    {
        bool imageConfirmed = false;

        // Once true, the not-confirmed branch has been evaluated and there
        // is nothing further to do, whether or not it logged a warning.
        bool unconfirmedWarned = false;
    };

    // What the caller should do this call - at most one of these is true.
    struct Action
    {
        bool confirmNow = false;
        bool logNotConfirmed = false;
    };

    // imagePendingVerify is the caller's answer to "is the running OTA
    // partition still ESP_OTA_IMG_PENDING_VERIFY?", passed in rather than
    // queried here so this stays free of esp_ota_ops.h.
    inline Action decide(State &state, bool wifiUp, bool bmsUp,
                          unsigned long nowMs, bool imagePendingVerify)
    {
        Action action;

        if (state.imageConfirmed || nowMs <= kConfirmAfterMs)
            return action;

        if (wifiUp && bmsUp)
        {
            state.imageConfirmed = true;
            action.confirmNow = true;
        }
        else if (!state.unconfirmedWarned)
        {
            state.unconfirmedWarned = true;
            action.logNotConfirmed = imagePendingVerify;
        }

        return action;
    }
}
