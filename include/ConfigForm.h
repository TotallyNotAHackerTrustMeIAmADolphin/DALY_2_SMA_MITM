#pragma once
#include <array>
#include <stdio.h>
#include "SystemConfig.h"
#include "SettingFormat.h"

// Pure /save form-parsing, validation and change-log formatting (#89),
// split out of WebDashboard::saveConfig() so it runs under
// `pio test -e native`. No Arduino dependency: the request itself is never
// named here, only the two callables a caller injects (Lookup, Emit),
// matching the Emit pattern already used by SystemConfig::ValidationResult.
namespace ConfigForm
{
    struct Result
    {
        // present[i] is true iff settings.all()[i]'s key was found by
        // lookup() (whether or not it went on to parse).
        bool present[SystemConfig::kNumSettings] = {false};
        // false if any field failed to parse or a two-setting rule was
        // violated - all-or-nothing: the caller must not publish or store
        // copy, which apply() may still have partially edited.
        bool ok = true;
    };

    // Parses every field lookup(key) returns non-null for into copy (via
    // Setting::parse(), so each one is range-checked exactly like set())
    // and marks it present - every field is tried even after an earlier one
    // fails, so present[] and every successfully-parsed value are complete
    // regardless of ok. Only if every field parsed does it also check
    // SystemConfig::validate()'s two-setting rules. Calls emit(message)
    // once per problem found, in order; message is valid only for that one
    // call. lookup(key)'s return value likewise only needs to stay valid
    // for that one call (true of AsyncWebParameter::value().c_str(), which
    // owns its storage for the whole request).
    template <typename Lookup, typename Emit>
    Result apply(SystemConfig &copy, Lookup lookup, Emit emit)
    {
        Result r;
        std::array<SettingBase *, SystemConfig::kNumSettings> settings = copy.all();
        for (size_t i = 0; i < settings.size(); i++)
        {
            SettingBase &s = *settings[i];
            const char *text = lookup(s.key());
            if (!text)
                continue;
            r.present[i] = true;

            switch (s.parse(text))
            {
            case SettingBase::ParseResult::Ok:
                break;
            case SettingBase::ParseResult::NotANumber:
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "%s is not a valid number.", s.label());
                emit(msg);
                r.ok = false;
                break;
            }
            case SettingBase::ParseResult::OutOfRange:
            {
                char minBuf[24], maxBuf[24], msg[160];
                formatSettingFixed(s, s.min(), minBuf, sizeof(minBuf));
                formatSettingFixed(s, s.max(), maxBuf, sizeof(maxBuf));
                snprintf(msg, sizeof(msg), "%s must be between %s and %s %s.", s.label(), minBuf, maxBuf, s.unit());
                emit(msg);
                r.ok = false;
                break;
            }
            }
        }

        // #53/#55: only once every field parsed - otherwise copy still
        // holds an old value for a rejected field, and a rule about it
        // would describe a config nobody submitted.
        if (r.ok)
        {
            SystemConfig::validate(copy).forEachMessage([&](const char *msg)
                                                          { emit(msg); r.ok = false; });
        }
        return r;
    }

    // "[CFG] <label>: <old> -> <new> <unit>\n" for each setting
    // SystemConfig::changedMask() finds changed between before and after,
    // or "[CFG] Saved, no changes\n" if none did - one emit(line) call per
    // line, so a real change never logs as "X -> X" (formatSettingValue's
    // guarantee) and a caller with a fixed-size log buffer isn't handed
    // more than one line at a time.
    template <typename Emit>
    void logChanges(const SystemConfig &before, const SystemConfig &after, Emit emit)
    {
        uint32_t changed = SystemConfig::changedMask(before, after);
        if (changed == 0)
        {
            emit("[CFG] Saved, no changes\n");
            return;
        }
        std::array<const SettingBase *, SystemConfig::kNumSettings> beforeSettings = before.all();
        std::array<const SettingBase *, SystemConfig::kNumSettings> afterSettings = after.all();
        for (size_t i = 0; i < afterSettings.size(); i++)
        {
            if (!(changed & ((uint32_t)1 << i)))
                continue;
            char oldBuf[24], newBuf[24], line[96];
            formatSettingValue(*beforeSettings[i], beforeSettings[i]->value(), oldBuf, sizeof(oldBuf));
            formatSettingValue(*afterSettings[i], afterSettings[i]->value(), newBuf, sizeof(newBuf));
            snprintf(line, sizeof(line), "[CFG] %s: %s -> %s %s\n", afterSettings[i]->label(), oldBuf, newBuf, afterSettings[i]->unit());
            emit(line);
        }
    }
}
