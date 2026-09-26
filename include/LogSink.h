#pragma once
#include <cstdarg>
#include <cstdio>

// One callback shape for every module that logs through a callback instead
// of calling netLog() directly (see the Logging section in CLAUDE.md):
// a single already-formatted line, no varargs. Replaces DalyDebugCallback/
// SMADebugCallback/SDDebugCallback/WebDebugCallback/DiagDebugCallback/
// ConfigStore::LogFn - two incompatible shapes and five copies of the same
// buf+vsnprintf body (#84).
using LogSink = void (*)(const char *line);

// printf-checked formatting into a 256-byte buffer, then one call to sink
// (a no-op if null). Takes fmt+args directly rather than a pre-formatted
// string, so an argument containing a literal '%' can't be reinterpreted
// as a format specifier.
inline void logf(LogSink sink, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

inline void logf(LogSink sink, const char *fmt, ...)
{
    if (!sink)
        return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    sink(buf);
}
