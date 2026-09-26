#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

// snprintf appends into a fixed buffer with a sticky overflow flag, so a
// sequence of appends needs one check at the end instead of one per call.
struct BoundedWriter
{
    char *buf;
    size_t cap;
    size_t len = 0;
    bool overflow = false;

    BoundedWriter(char *b, size_t c) : buf(b), cap(c)
    {
        if (cap)
            buf[0] = '\0';
    }

    bool append(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (overflow)
            return false;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n < 0 || (size_t)n >= cap - len)
        {
            overflow = true;
            buf[len] = '\0'; // drop the partial append
            return false;
        }
        len += (size_t)n;
        return true;
    }

    bool ok() const { return !overflow; }
};
