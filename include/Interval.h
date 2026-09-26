#pragma once

#include <stdint.h>

// A periodic millis() timer: due(now) is true, and restarts the period,
// once more than periodMs has passed since the last time it fired (the
// first call fires once now > periodMs after boot). Unsigned subtraction
// keeps it correct across the ~49-day millis() wrap.
struct Interval
{
    uint32_t periodMs;
    uint32_t last = 0;

    explicit Interval(uint32_t period) : periodMs(period) {}

    bool due(uint32_t nowMs)
    {
        if ((uint32_t)(nowMs - last) <= periodMs)
            return false;
        last = nowMs;
        return true;
    }
};
