#include "ConfigStore.h"
#include "SettingFormat.h"
#include <Preferences.h>
#include <cstdarg>
#include <cstdio>

namespace
{
    constexpr const char *kNvsNamespace = "bms-bridge";

    // The one place a Setting::Kind maps to the NVS type it's stored as.
    // Preferences enforces the stored type, so a key written with putUInt
    // must be read back with getUInt.
    PreferenceType expectedNvsType(SettingBase::Kind kind)
    {
        switch (kind)
        {
        case SettingBase::KIND_INT:
            return PT_I32;
        case SettingBase::KIND_UINT16:
            return PT_U32;
        default:
            return PT_BLOB; // putFloat = putBytes
        }
    }

    void debugLog(ConfigStore::LogFn log, const char *format, ...) __attribute__((format(printf, 2, 3)));
    void debugLog(ConfigStore::LogFn log, const char *format, ...)
    {
        if (!log)
            return;
        char buf[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        log(buf);
    }
}

void ConfigStore::load(SystemConfig &cfg, LogFn log)
{
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);

    // Each setting starts at its default. A stored value is only taken if
    // the setting's set() accepts it; one outside its range (NaN from
    // corrupted flash, or one saved before #61 such as a spread threshold
    // that wrapped to 65476) keeps the default and is logged, instead of
    // running with it - several of these (bmsTimeout, spread) would switch
    // a safety mechanism off. NVS integers are read at full width, so the
    // range check happens before anything is narrowed.
    for (SettingBase *s : cfg.all())
    {
        s->reset();
        // One getType() both finds the key and tells its stored type: a key
        // written as another type (e.g. by an older firmware) would make
        // the typed read below fail and quietly return the default. Say so
        // instead.
        PreferenceType stored = prefs.getType(s->key());
        if (stored == PT_INVALID)
            continue; // never saved: default

        char defBuf[24];
        formatSettingValue(*s, s->def(), defBuf, sizeof(defBuf));
        PreferenceType expected = expectedNvsType(s->kind());
        if (stored != expected)
        {
            debugLog(log, "[CFG] Stored %s has NVS type %d, expected %d - using the default %s\n",
                     s->label(), (int)stored, (int)expected, defBuf);
            continue;
        }

        double v;
        switch (s->kind())
        {
        case SettingBase::KIND_INT:
            v = prefs.getInt(s->key(), (int)s->def());
            break;
        case SettingBase::KIND_UINT16:
            v = prefs.getUInt(s->key(), (uint32_t)s->def());
            break;
        default:
            v = prefs.getFloat(s->key(), (float)s->def());
            break;
        }
        if (!s->set(v))
        {
            char valBuf[24], minBuf[24], maxBuf[24];
            formatSettingValue(*s, v, valBuf, sizeof(valBuf));
            formatSettingValue(*s, s->min(), minBuf, sizeof(minBuf));
            formatSettingValue(*s, s->max(), maxBuf, sizeof(maxBuf));
            debugLog(log, "[CFG] Stored %s (%s) is outside %s-%s %s - using the default %s\n",
                     s->label(), valBuf, minBuf, maxBuf, s->unit(), defBuf);
        }
    }

    prefs.end();

    // #56: every setting is in range now (set() saw to that); what can
    // still fail are the rules relating two settings. Logged only - never
    // blocks boot, and there's no single safe default to fall back to for
    // an ordering.
    SystemConfig::ValidationResult validation = SystemConfig::validate(cfg);
    validation.forEachMessage([log](const char *msg)
                               { debugLog(log, "[CFG] Loaded config fails validation: %s\n", msg); });
}

void ConfigStore::store(const SystemConfig &cfg, const bool present[SystemConfig::kNumSettings])
{
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);
    std::array<const SettingBase *, SystemConfig::kNumSettings> settings = cfg.all();
    for (size_t i = 0; i < settings.size(); i++)
    {
        if (!present[i])
            continue;
        const SettingBase &s = *settings[i];
        switch (s.kind())
        {
        case SettingBase::KIND_FLOAT:
            prefs.putFloat(s.key(), (float)s.value());
            break;
        case SettingBase::KIND_INT:
            prefs.putInt(s.key(), (int)s.value());
            break;
        case SettingBase::KIND_UINT16:
            prefs.putUInt(s.key(), (uint32_t)s.value());
            break;
        }
    }
    prefs.end();
}
