#pragma once

#include <stdint.h>
#include "SystemConfig.h"

// Per-cell moving-average smoother for Daly BMS cell voltages. Pure: no
// Arduino/FreeRTOS dependency, so test/test_cellsmoother can include and
// run *this* header natively (`pio test -e native`), same pattern as
// Glideslope.h. bmsTask: read the BMS -> update() -> take dataMutex -> store
// the Result fields into currentData -> give -> log if reseeded.
//
// The window holds integer mV (the Daly's unit); only the average rounds,
// and Result converts to volts once.
class CellSmoother
{
public:
    // Buffer capacities, not the pack's cell count (kPackCells).
    static constexpr int MAX_CELLS = 16;
    static constexpr int MAX_SAMPLES = kMaxVSamples;
    static_assert(MAX_CELLS >= kPackCells, "CellSmoother can't hold every cell of the pack");

    struct Result
    {
        float smoothedV[MAX_CELLS] = {};
        float minV = 0.0f, maxV = 0.0f, avgV = 0.0f;
        float rawMinV = 0.0f, rawMaxV = 0.0f;
        uint16_t rawSpreadMv = 0;
        int cells = 0;
        bool reseeded = false;
    };

    // rawMv: this read's per-cell voltages, in millivolts; n entries
    // (clamped to [0, MAX_CELLS]). windowSize: cfg.vSamples, clamped to
    // [1, MAX_SAMPLES]. n == 0 returns a zeroed Result without touching any
    // stored state - there's nothing to smooth or reseed from.
    Result update(const uint16_t *rawMv, int n, int windowSize)
    {
        if (windowSize < 1)
            windowSize = 1;
        if (windowSize > MAX_SAMPLES)
            windowSize = MAX_SAMPLES;

        int cells = n;
        if (cells < 0)
            cells = 0;
        if (cells > MAX_CELLS)
            cells = MAX_CELLS;

        Result r;
        r.cells = cells;
        if (cells == 0)
            return r;

        // Fill the whole moving-average window from the current reading
        // whenever the window size changes - including the very first call,
        // since lastWindowSize_ starts at -1 and never matches a real
        // windowSize. Without this, slots [windowSize..MAX_SAMPLES) keep
        // whatever was last written there; raising cfg.vSamples at runtime
        // would then average stale voltages back in for a whole window.
        if (windowSize != lastWindowSize_)
        {
            for (int i = 0; i < cells; i++)
                for (int j = 0; j < MAX_SAMPLES; j++)
                    buf_[i][j] = rawMv[i];
            index_ = 0;
            lastWindowSize_ = windowSize;
            r.reseeded = true;
        }

        uint32_t sumMv = 0;
        uint16_t localMinMv = 0xFFFF;
        uint16_t localMaxMv = 0;
        uint16_t rawMinMv = 0xFFFF;
        uint16_t rawMaxMv = 0;

        for (int i = 0; i < cells; i++)
        {
            buf_[i][index_] = rawMv[i];

            uint32_t cellSumMv = 0;
            for (int j = 0; j < windowSize; j++)
                cellSumMv += buf_[i][j];

            // Rounded, not floored (#70).
            uint16_t smoothedMv = (uint16_t)((cellSumMv + (uint32_t)windowSize / 2) / (uint32_t)windowSize);
            r.smoothedV[i] = smoothedMv / 1000.0f;

            sumMv += smoothedMv;
            if (smoothedMv < localMinMv)
                localMinMv = smoothedMv;
            if (smoothedMv > localMaxMv)
                localMaxMv = smoothedMv;

            // Raw (unsmoothed) min/max from this latest read - drives the
            // glideslope hard cutoff/alarm gate, independent of the
            // smoothed values above which drive the taper.
            if (rawMv[i] < rawMinMv)
                rawMinMv = rawMv[i];
            if (rawMv[i] > rawMaxMv)
                rawMaxMv = rawMv[i];
        }

        // Divides by the cells actually read, not MAX_CELLS (#70).
        r.avgV = (float)sumMv / (1000.0f * cells);
        r.minV = localMinMv / 1000.0f;
        r.maxV = localMaxMv / 1000.0f;
        r.rawMinV = rawMinMv / 1000.0f;
        r.rawMaxV = rawMaxMv / 1000.0f;

        // Raw spread, from the same unsmoothed read as rawMin/rawMax above -
        // drives Glideslope::spreadFactor().
        r.rawSpreadMv = rawMaxMv - rawMinMv;

        // Increment circular buffer index after processing all cells, once
        // per update() call, not per-cell.
        index_ = (index_ + 1) % windowSize;

        return r;
    }

private:
    uint16_t buf_[MAX_CELLS][MAX_SAMPLES] = {};
    int index_ = 0;
    int lastWindowSize_ = -1; // never matches a real windowSize -> first call always reseeds
};
