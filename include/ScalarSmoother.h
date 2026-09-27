#pragma once

#include "SystemConfig.h"

// Single-value moving-average smoother - same window/reseed-on-change
// semantics as CellSmoother.h, but for one scalar (pack voltage/current)
// instead of a per-cell array. Pure: no Arduino/FreeRTOS dependency, so
// test/test_scalarsmoother compiles and runs *this* code natively
// (`pio test -e native`).
//
// Nothing safety-critical reads pack voltage/current - Glideslope only ever
// reads per-cell voltages (include/Glideslope.h) - so unlike CellSmoother
// there is no raw variant to keep alongside this: bmsTask stores only the
// smoothed value into DashboardData. BmsEvents::decide()'s SOC-jump
// detector still sees the true raw DalyBasicInfo, since it runs on that
// struct before bmsTask calls update() below (see main.cpp's
// pollBasicInfo()).
class ScalarSmoother
{
public:
    static constexpr int MAX_SAMPLES = kMaxVSamples;

    // raw: this read's value. windowSize: cfg.vSamples, clamped to
    // [1, MAX_SAMPLES], same window CellSmoother uses.
    float update(float raw, int windowSize)
    {
        if (windowSize < 1)
            windowSize = 1;
        if (windowSize > MAX_SAMPLES)
            windowSize = MAX_SAMPLES;

        // Reseed the whole window on the first call or a window-size change -
        // same rationale as CellSmoother: without this, slots
        // [windowSize..MAX_SAMPLES) keep whatever was last written there, and
        // raising cfg.vSamples at runtime would average stale values back in.
        if (windowSize != lastWindowSize_)
        {
            for (int j = 0; j < MAX_SAMPLES; j++)
                buf_[j] = raw;
            index_ = 0;
            lastWindowSize_ = windowSize;
        }

        buf_[index_] = raw;

        float sum = 0.0f;
        for (int j = 0; j < windowSize; j++)
            sum += buf_[j];
        float result = sum / (float)windowSize;

        index_ = (index_ + 1) % windowSize;
        return result;
    }

private:
    float buf_[MAX_SAMPLES] = {};
    int index_ = 0;
    int lastWindowSize_ = -1; // never matches a real windowSize -> first call always reseeds
};
