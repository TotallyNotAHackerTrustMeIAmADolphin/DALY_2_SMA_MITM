#pragma once
#include <float.h>
#include <stdio.h>
#include <string.h>
#include "SystemConfig.h"

// Not always defined by <float.h> (C99/C++11); 9 covers a round-tripped binary32.
#ifndef FLT_DECIMAL_DIG
#define FLT_DECIMAL_DIG 9
#endif

// A setting's own display decimals - the /config page's <input>s, and a
// LIMIT (min/max) in a [CFG] log line or 400 response. Pure, snprintf-based.
inline void formatSettingFixed(const SettingBase &s, double v, char *out, size_t outSize)
{
    if (s.kind() == SettingBase::KIND_FLOAT)
        snprintf(out, outSize, "%.*f", (int)s.decimals(), v);
    else
        snprintf(out, outSize, "%ld", (long)v);
}

// Shortest round-trip %g for a setting's own VALUE (stored/old/new/default,
// never a limit) in a [CFG] log line, so a real change never prints as
// "X -> X". Pure, snprintf-based; 24 bytes holds every output.
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
