#pragma once
#include <array>
#include <stdio.h>
#include "SystemConfig.h"
#include "SettingFormat.h"

// Pure /save parsing, validation and change-log formatting (#89), split out
// of WebDashboard::saveConfig() for pio test -e native.
namespace ConfigForm
{
    struct Result
    {
        bool present[SystemConfig::kNumSettings] = {false}; // key was submitted
        bool ok = true; // false = caller must not publish or store copy
    };

    // Parses every field lookup(key) returns non-null for into copy, marks
    // it present, and - only if everything parsed - checks the two-setting
    // rules. emit(message) fires once per problem; both callables' return
    // values need only stay valid for that one call.
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

        // #53/#55: only once everything parsed, so a rule can't describe a
        // field's stale value.
        if (r.ok)
            SystemConfig::validate(copy).forEachMessage([&](const char *msg)
                                                          { emit(msg); r.ok = false; });
        return r;
    }

    // "[CFG] <label>: <old> -> <new> <unit>\n" per changed setting, or
    // "[CFG] Saved, no changes\n" if none did - one emit(line) per line.
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
