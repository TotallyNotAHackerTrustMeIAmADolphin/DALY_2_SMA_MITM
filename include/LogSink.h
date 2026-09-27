#pragma once
#include <cstdarg>
#include <cstdio>

// One callback shape for every module that logs through a callback instead
// of calling netLog() directly: a single already-formatted line, no varargs.
using LogSink = void (*)(const char *line);

// vsnprintf into buf, then - if truncated - force back the trailing '\n' a
// longer line would have had, so it can't run onto whatever the next line
// writes. Shared by logTo() below and main.cpp's netLog(), which wraps the
// same formatted line with a timestamp before it goes to Serial/SSE/SD.
inline void vformatLine(char *buf, size_t bufSize, const char *fmt, va_list args)
{
    int n = vsnprintf(buf, bufSize, fmt, args);
    if (n >= (int)bufSize)
        buf[bufSize - 2] = '\n';
}

// printf-checked formatting into a 256-byte buffer, then one call to sink
// (a no-op if null). Takes fmt+args directly, not a pre-formatted string, so
// an argument with a literal '%' can't be reinterpreted as a format spec.
// Named logTo, not logf: the latter collides with <cmath>'s float logf(float).
inline void logTo(LogSink sink, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

inline void logTo(LogSink sink, const char *fmt, ...)
{
    if (!sink)
        return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vformatLine(buf, sizeof(buf), fmt, args);
    va_end(args);
    sink(buf);
}
