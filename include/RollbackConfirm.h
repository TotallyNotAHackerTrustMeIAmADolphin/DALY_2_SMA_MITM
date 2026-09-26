#pragma once

// The OTA rollback-confirmation decision (#58/#57), kept free of Arduino/
// FreeRTOS/ESP-IDF dependencies for the same reason as StatusFrame.h/
// BmsEvents.h: so test/test_rollbackconfirm compiles and runs *this* code
// natively (`pio test -e native`) instead of a hand-copied mirror of it.
//
// Diagnostics::confirmImageIfReady() becomes a thin wrapper: it holds the
// static State, decides whether it needs to ask the ESP-IDF OTA API whether
// the running image is still pending-verify (only when that answer could
// actually change the outcome - see the wrapper's own comment), calls
// decide(), and then does the two side effects (esp_ota_mark_app_valid_
// cancel_rollback() + the "[SYS] Firmware confirmed ..." log line on
// confirmNow; the "[SYS] Firmware NOT confirmed ..." log line on
// logNotConfirmed) exactly as it did inline before.

namespace RollbackConfirm
{
    // The uptime deadline (ms) before the image can be confirmed or the
    // "not confirmed" warning can fire - moved here from src/Diagnostics.cpp,
    // which now uses this one instead of its own copy.
    constexpr unsigned long kConfirmAfterMs = 2UL * 60UL * 1000UL;

    // Persistent between calls; owned by Diagnostics::confirmImageIfReady()
    // as a function-local `static` (replaces the two separate `static bool`s
    // that used to live there).
    struct State
    {
        bool imageConfirmed = false;

        // Once true, the not-confirmed branch has been evaluated once and
        // there is nothing further to do on that path - true whether or not
        // it actually produced a log line (i.e. also when the image was not
        // pending-verify, so there was nothing to warn about).
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
    // queried here so this stays free of esp_ota_ops.h; the wrapper only
    // needs to compute it when it can affect the outcome (see its comment).
    // Whether decide() can still act on wifiUp/bmsUp: false before
    // kConfirmAfterMs (decide() returns early) and forever once confirmed.
    // Lets the caller skip gathering bmsUp - a dataMutex take - on every
    // loop() pass for the whole uptime (#75).
    inline bool needsInputs(const State &state, unsigned long nowMs)
    {
        return !state.imageConfirmed && nowMs > kConfirmAfterMs;
    }

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
