#pragma once

#include <stdint.h>
#include "SystemConfig.h"

// Per-cell moving-average smoother for Daly BMS cell voltages. Pure: no
// Arduino/FreeRTOS dependency, so test/test_cellsmoother can include and
// run *this* header natively (`pio test -e native`), same pattern as
// Glideslope.h. bmsTask: read the BMS -> update() -> take dataMutex -> store
// the Result fields into currentData -> give -> log if reseeded.
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

    // rawV: this read's per-cell voltages, in volts; n entries (clamped to
    // [0, MAX_CELLS]). windowSize: cfg.vSamples, clamped to [1, MAX_SAMPLES].
    Result update(const float *rawV, int n, int windowSize)
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

        // Fill the whole moving-average window from the current reading
        // whenever the window size changes - including the very first call,
        // since lastWindowSize_ starts at -1 and never matches a real
        // windowSize. Without this, slots [windowSize..MAX_SAMPLES) keep
        // whatever was last written there; raising cfg.vSamples at runtime
        // would then average stale voltages back in for a whole window.
        if (windowSize != lastWindowSize_)
        {
            for (int i = 0; i < cells; i++)
            {
                uint16_t mv = (uint16_t)(rawV[i] * 1000.0f);
                for (int j = 0; j < MAX_SAMPLES; j++)
                    buf_[i][j] = mv;
            }
            index_ = 0;
            lastWindowSize_ = windowSize;
            r.reseeded = true;
        }

        float sum = 0.0f;
        float localMin = 10.0f;
        float localMax = 0.0f;
        float rawMin = 10.0f;
        float rawMax = 0.0f;

        for (int i = 0; i < cells; i++)
        {
            buf_[i][index_] = (uint16_t)(rawV[i] * 1000.0f);

            uint32_t cellSumMV = 0;
            for (int j = 0; j < windowSize; j++)
                cellSumMV += buf_[i][j];

            float smoothedV = (float)(cellSumMV / windowSize) / 1000.0f;
            r.smoothedV[i] = smoothedV;

            sum += smoothedV;
            if (smoothedV < localMin)
                localMin = smoothedV;
            if (smoothedV > localMax)
                localMax = smoothedV;

            // Raw (unsmoothed) min/max from this latest read - drives the
            // glideslope hard cutoff/alarm gate, independent of the
            // smoothed values above which drive the taper.
            if (rawV[i] < rawMin)
                rawMin = rawV[i];
            if (rawV[i] > rawMax)
                rawMax = rawV[i];
        }

        // Divides by MAX_CELLS, not cells: correct for the real 16-cell
        // pack this firmware always runs (readCellVoltages always resizes
        // its output to exactly MAX_CELLS).
        r.avgV = sum / MAX_CELLS;
        r.minV = localMin;
        r.maxV = localMax;
        r.rawMinV = rawMin;
        r.rawMaxV = rawMax;

        // Raw spread, from the same unsmoothed read as rawMin/rawMax above -
        // drives Glideslope::spreadFactor() in canTask. Hand-rolled
        // round-half-away-from-zero (this header is stdint.h-only, no
        // <math.h>).
        float spreadMv = (rawMax - rawMin) * 1000.0f;
        r.rawSpreadMv = (uint16_t)(spreadMv >= 0.0f ? spreadMv + 0.5f : spreadMv - 0.5f);

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
