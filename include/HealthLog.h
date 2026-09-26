#pragma once

// When a periodic health sample is worth a log line. Kept free of Arduino/
// FreeRTOS/ESP-IDF dependencies, same as StatusFrame.h/BmsEvents.h/
// RollbackConfirm.h, so test/test_healthlog runs this code natively.
//
// Diagnostics::logHealth() still samples every 10 minutes, but a steady
// device used to write an identical heap/stack line 144 times a day. Only
// the things that can actually go wrong are logged now: the heap low-water
// mark or largest free block shrinking (a leak or fragmentation), a task's
// stack high-water mark shrinking (a task heading for overflow), plus one
// baseline line after boot and a daily heartbeat so a long quiet log still
// shows the device alive with its current numbers. WiFi RSSI rides along in
// the line but never triggers one - WiFi transitions have their own
// edge-triggered [WIFI] lines.

#include <stdint.h>

namespace HealthLog
{
    // Order matches the stack fields in Diagnostics::logHealth()'s line.
    enum StackTask
    {
        kLoop,
        kBms,
        kCan,
        kSd,
        kAsyncTcp,
        kEvents,
        kNumTasks
    };

    struct Sample
    {
        uint32_t freeHeap = 0;
        uint32_t minFreeHeap = 0; // low-water mark since boot, never rises
        uint32_t maxBlock = 0;    // largest allocatable block
        // Stack high-water marks in bytes; 0 = task not found (not started
        // yet, e.g. async_tcp before the network is up).
        uint32_t stackLeft[kNumTasks] = {};
    };

    // Log when the heap low-water mark has dropped this much since the last
    // logged line.
    constexpr uint32_t kHeapDropBytes = 4096;
    // Log when the largest free block has shrunk this much (fragmentation).
    constexpr uint32_t kBlockDropBytes = 8192;
    // Log when any task's stack high-water mark has dropped this much.
    constexpr uint32_t kStackDropBytes = 256;
    // Log once when any task's remaining stack first falls below this.
    constexpr uint32_t kStackLowBytes = 512;
    // Heartbeat: log at least this often even when nothing moved.
    constexpr uint32_t kHeartbeatMs = 24UL * 60UL * 60UL * 1000UL;

    enum Reason
    {
        kNone,
        kBaseline,
        kHeapDrop,
        kBlockDrop,
        kStackDrop,
        kStackLow,
        kHeartbeat
    };

    inline const char *reasonName(Reason r)
    {
        switch (r)
        {
        case kBaseline:
            return "baseline";
        case kHeapDrop:
            return "heap low-water dropped";
        case kBlockDrop:
            return "largest block shrank";
        case kStackDrop:
            return "stack high-water dropped";
        case kStackLow:
            return "STACK LOW";
        case kHeartbeat:
            return "daily";
        default:
            return "";
        }
    }

    // Persistent between calls; owned by Diagnostics::logHealth() as a
    // function-local static. `last` is the most recently LOGGED sample, so
    // a slow drift is caught once it adds up to a threshold, not lost
    // because each 10-minute step was small.
    struct State
    {
        bool haveBaseline = false;
        uint32_t lastLogMs = 0;
        Sample last;
    };

    // Returns why this sample should be logged, or kNone. Updates state
    // only when it returns something other than kNone - except that a
    // task seen for the first time (its last value was 0) adopts its
    // current value, so a late-starting task is compared from its own
    // first reading instead of never.
    inline Reason decide(State &st, const Sample &s, uint32_t nowMs)
    {
        Reason r = kNone;

        if (!st.haveBaseline)
        {
            r = kBaseline;
        }
        else
        {
            // Stack low is the most urgent, so it wins the reason label.
            for (int i = 0; i < kNumTasks && r == kNone; i++)
            {
                uint32_t prev = st.last.stackLeft[i], cur = s.stackLeft[i];
                if (prev != 0 && cur != 0 && cur < kStackLowBytes && prev >= kStackLowBytes)
                    r = kStackLow;
            }
            for (int i = 0; i < kNumTasks && r == kNone; i++)
            {
                uint32_t prev = st.last.stackLeft[i], cur = s.stackLeft[i];
                if (prev != 0 && cur != 0 && cur < prev && prev - cur >= kStackDropBytes)
                    r = kStackDrop;
            }
            if (r == kNone && s.minFreeHeap < st.last.minFreeHeap &&
                st.last.minFreeHeap - s.minFreeHeap >= kHeapDropBytes)
                r = kHeapDrop;
            if (r == kNone && s.maxBlock < st.last.maxBlock &&
                st.last.maxBlock - s.maxBlock >= kBlockDropBytes)
                r = kBlockDrop;
            // Unsigned subtraction keeps this right across millis() wrap.
            if (r == kNone && (uint32_t)(nowMs - st.lastLogMs) >= kHeartbeatMs)
                r = kHeartbeat;
        }

        if (r != kNone)
        {
            st.haveBaseline = true;
            st.lastLogMs = nowMs;
            st.last = s;
        }
        else
        {
            for (int i = 0; i < kNumTasks; i++)
                if (st.last.stackLeft[i] == 0)
                    st.last.stackLeft[i] = s.stackLeft[i];
        }
        return r;
    }
}
