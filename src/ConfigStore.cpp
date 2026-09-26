#include "ConfigStore.h"
#include "SettingFormat.h"
#include <Preferences.h>

namespace
{
    constexpr const char *kNvsNamespace = "bms-bridge";
}

void ConfigStore::load(SystemConfig &cfg, LogSink log)
{
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);

    // A stored value is only taken if it has the expected NVS type and
    // set() accepts it; otherwise the setting keeps its default (#61: an
    // out-of-range value must not silently switch a safety mechanism off).
    for (SettingBase *s : cfg.all())
    {
        s->reset();
        PreferenceType stored = prefs.getType(s->key());
        if (stored == PT_INVALID)
            continue; // never saved: default

        char defBuf[24];
        formatSettingValue(*s, s->def(), defBuf, sizeof(defBuf));
        // Preferences enforces the stored type: a key written with putUInt
        // must be read back with getUInt.
        PreferenceType expected = s->kind() == SettingBase::KIND_INT      ? PT_I32
                                  : s->kind() == SettingBase::KIND_UINT16 ? PT_U32
                                                                          : PT_BLOB; // putFloat = putBytes
        if (stored != expected)
        {
            logTo(log, "[CFG] Stored %s has NVS type %d, expected %d - using the default %s\n",
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
            // v/default are values (round-trip); min/max are limits the
            // owner typed, so they print the same fixed text /config does.
            char valBuf[24], minBuf[24], maxBuf[24];
            formatSettingValue(*s, v, valBuf, sizeof(valBuf));
            formatSettingFixed(*s, s->min(), minBuf, sizeof(minBuf));
            formatSettingFixed(*s, s->max(), maxBuf, sizeof(maxBuf));
            logTo(log, "[CFG] Stored %s (%s) is outside %s-%s %s - using the default %s\n",
                  s->label(), valBuf, minBuf, maxBuf, s->unit(), defBuf);
        }
    }

    prefs.end();

    // #56: set() already range-checked every setting; only the rules
    // relating two settings can still fail. Logged only - never blocks boot.
    SystemConfig::validate(cfg).forEachMessage([log](const char *msg)
                                                 { logTo(log, "[CFG] Loaded config fails validation: %s\n", msg); });
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
