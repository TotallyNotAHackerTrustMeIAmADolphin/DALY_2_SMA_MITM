#pragma once
#include <float.h>
#include <stdio.h>
#include <string.h>
#include "SystemConfig.h"

// Some toolchains' <float.h> doesn't define this C99/C++11 addition (kept
// as a fallback, not because either build here is known to lack it -
// verified present on both the native host and the ESP32 (xtensa) toolchain
// this project builds with).
#ifndef FLT_DECIMAL_DIG
#define FLT_DECIMAL_DIG 9
#endif

// Formats v (one of s's own value/min/max/def) at s's own display decimals -
// the /config page's <input> values, its range text, and a load-time [CFG]
// log line's min/max/default. An integer kind prints as a plain integer.
// Pure, snprintf-based, always NUL-terminated; any outSize is safe (snprintf
// truncates).
inline void formatSettingFixed(const SettingBase &s, double v, char *out, size_t outSize)
{
    if (s.kind() == SettingBase::KIND_FLOAT)
        snprintf(out, outSize, "%.*f", (int)s.decimals(), v);
    else
        snprintf(out, outSize, "%ld", (long)v);
}

// Formats v as the shortest FIXED-FORM (non-exponent) %g precision that
// round-trips its binary32 value exactly (up to FLT_DECIMAL_DIG significant
// digits), falling back to exponent form only where %g can't avoid it (a
// magnitude below 1e-4) - so two distinct binary32 values, or a genuine
// change, never print identically. Used for every [CFG] log line that
// reports a setting's value (the post-save change log, and a load-time
// out-of-range/default report). An integer kind prints as a plain integer.
// Pure, snprintf-based, always NUL-terminated; 24 bytes holds every output.
inline void formatSettingValue(const SettingBase &s, double v, char *out, size_t outSize)
{
    if (s.kind() != SettingBase::KIND_FLOAT)
    {
        snprintf(out, outSize, "%ld", (long)v);
        return;
    }
    float target = (float)v;
    char firstRoundTrip[24] = "";
    for (int precision = 1; precision <= FLT_DECIMAL_DIG; precision++)
    {
        char candidate[24];
        snprintf(candidate, sizeof(candidate), "%.*g", precision, v);
        if ((float)strtod(candidate, nullptr) != target)
            continue;
        if (firstRoundTrip[0] == '\0')
            snprintf(firstRoundTrip, sizeof(firstRoundTrip), "%s", candidate);
        if (strchr(candidate, 'e') == nullptr && strchr(candidate, 'E') == nullptr)
        {
            snprintf(out, outSize, "%s", candidate);
            return;
        }
    }
    snprintf(out, outSize, "%s", firstRoundTrip);
}
