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

    // xTaskGetHandle() name plus the line's short "stack left: ..." name,
    // paired so the two can't drift apart - next to the enum so adding a
    // task is one entry here. Function-local static (like DalyFrames::
    // kAlarmBitNames()) rather than an inline namespace-scope array, since
    // the device build predates C++17 inline variables.
    struct TaskName
    {
        const char *full, *shortForm;
    };
    inline const TaskName (&taskNames())[kNumTasks]
    {
        static const TaskName names[] = {
            {"loopTask", "loop"}, {"BMS_Task", "bms"}, {"CAN_Task", "can"},
            {"SD_LogTask", "sd"}, {"async_tcp", "async_tcp"}, {"arduino_events", "events"}};
        static_assert(sizeof(names) / sizeof(names[0]) == kNumTasks, "taskNames must cover every StackTask");
        return names;
    }

    // stackLeft value for "task not found" (not started yet, e.g. async_tcp
    // before the network is up, or gone). Not 0: a found task whose stack
    // is completely used up reads 0, and that must count as STACK LOW.
    constexpr uint32_t kNoTask = 0xFFFFFFFFu;

    struct Sample
    {
        uint32_t freeHeap = 0;
        uint32_t minFreeHeap = 0; // low-water mark since boot, never rises
        uint32_t maxBlock = 0;    // largest allocatable block
        // Stack high-water marks in bytes, or kNoTask.
        uint32_t stackLeft[kNumTasks] = {kNoTask, kNoTask, kNoTask, kNoTask, kNoTask, kNoTask};
        // SDLogger::Stats' drop/failure counters - monotonic, so any
        // increase since the last logged line is real.
        uint32_t sdDroppedQueueFull = 0;
        uint32_t sdDroppedLockTimeout = 0;
        uint32_t sdWriteFailures = 0;
    };

    // Log when the heap low-water mark has dropped this much since the last
    // logged line.
    constexpr uint32_t kHeapDropBytes = 4096;
    // Log when the largest free block is this much below the best value
    // seen since the last logged line (fragmentation).
    constexpr uint32_t kBlockDropBytes = 8192;
    // Log when any task's stack high-water mark has dropped this much.
    constexpr uint32_t kStackDropBytes = 256;
    // Below this much stack left, a task is STACK LOW: logged once when it
    // first gets there, then on every further drop, however small - the
    // high-water mark only ever falls, so that is a handful of lines at
    // most, and exactly when the remaining headroom matters.
    constexpr uint32_t kStackLowBytes = 512;
    // Heartbeat: log at least this often even when nothing moved.
    constexpr uint32_t kHeartbeatMs = 24UL * 60UL * 60UL * 1000UL;

    enum Reason
    {
        kNone,
        kBaseline,
        kStackLow,
        kTaskGone,
        kStackDrop,
        kSdDrops,
        kHeapDrop,
        kBlockDrop,
        kHeartbeat
    };

    inline const char *reasonName(Reason r)
    {
        switch (r)
        {
        case kBaseline:
            return "baseline";
        case kStackLow:
            return "STACK LOW";
        case kTaskGone:
            return "TASK GONE";
        case kStackDrop:
            return "stack high-water dropped";
        case kSdDrops:
            return "SD DROPS";
        case kHeapDrop:
            return "heap low-water dropped";
        case kBlockDrop:
            return "largest block shrank";
        case kHeartbeat:
            return "daily";
        default:
            return "";
        }
    }

    // What decide() returns: why to log (kNone = don't), and for the
    // per-task reasons which task, so the line can name it.
    struct Decision
    {
        Reason reason = kNone;
        int task = -1;
    };

    // Persistent between calls; owned by Diagnostics::logHealth() as a
    // function-local static.
    struct State
    {
        bool haveBaseline = false;
        uint32_t lastLogMs = 0;
        // Reference values: heap and stack are those of the last LOGGED
        // line, so slow drift adds up to a threshold instead of being lost
        // in small 10-minute steps. maxBlock also ratchets UP between
        // lines (it recovers when buffers are freed), so a baseline taken
        // during a transient allocation doesn't hide later fragmentation.
        Sample ref;
    };

    // Priority when several things moved at once: the most urgent reason
    // labels the line (the line always carries every value anyway). One
    // mechanism for every candidate - consider() keeps whichever reason
    // sorts lowest, i.e. earliest in the Reason enum - rather than a
    // per-task "r < d.reason" comparison plus a separate "d.reason ==
    // kNone" chain for the rest.
    inline Decision decide(State &st, const Sample &s, uint32_t nowMs)
    {
        Decision d;

        if (!st.haveBaseline)
        {
            d.reason = kBaseline;
        }
        else
        {
            auto consider = [&](Reason r, int task = -1)
            {
                if (r != kNone && (d.reason == kNone || r < d.reason))
                {
                    d.reason = r;
                    d.task = task;
                }
            };

            for (int i = 0; i < kNumTasks; i++)
            {
                uint32_t prev = st.ref.stackLeft[i], cur = s.stackLeft[i];
                if (prev != kNoTask && cur == kNoTask)
                    consider(kTaskGone, i);
                else if (cur != kNoTask && cur < kStackLowBytes && (prev == kNoTask || cur < prev))
                    consider(kStackLow, i); // first reading already low, or low and still falling
                else if (prev != kNoTask && cur != kNoTask && cur < prev && prev - cur >= kStackDropBytes)
                    consider(kStackDrop, i);
            }
            // Unlike the drops below, any increase logs: one dropped
            // sample or write failure is already worth knowing about.
            if (s.sdDroppedQueueFull > st.ref.sdDroppedQueueFull ||
                s.sdDroppedLockTimeout > st.ref.sdDroppedLockTimeout ||
                s.sdWriteFailures > st.ref.sdWriteFailures)
                consider(kSdDrops);
            if (s.minFreeHeap < st.ref.minFreeHeap && st.ref.minFreeHeap - s.minFreeHeap >= kHeapDropBytes)
                consider(kHeapDrop);
            if (s.maxBlock < st.ref.maxBlock && st.ref.maxBlock - s.maxBlock >= kBlockDropBytes)
                consider(kBlockDrop);
            // Unsigned subtraction keeps this right across millis() wrap.
            if ((uint32_t)(nowMs - st.lastLogMs) >= kHeartbeatMs)
                consider(kHeartbeat);
        }

        if (d.reason != kNone)
        {
            st.haveBaseline = true;
            st.lastLogMs = nowMs;
            st.ref = s;
        }
        else
        {
            // Nothing logged: a task seen for the first time adopts its
            // reading (a healthy one - a low one returned kStackLow above),
            // and maxBlock ratchets up to the best value seen.
            for (int i = 0; i < kNumTasks; i++)
                if (st.ref.stackLeft[i] == kNoTask)
                    st.ref.stackLeft[i] = s.stackLeft[i];
            if (s.maxBlock > st.ref.maxBlock)
                st.ref.maxBlock = s.maxBlock;
        }
        return d;
    }
}
