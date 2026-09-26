#pragma once
#include <Arduino.h>
#include <vector>
#include "DalyFrames.h"

typedef void (*DalyDebugCallback)(const char *msg);

// The plain data structs and pure byte-level parsers live in DalyFrames.h,
// so both compile and run under `pio test -e native`. These using-
// declarations keep the unqualified names working for existing callers.
using DalyFrames::DalyAlarmStatus;
using DalyFrames::DalyBasicInfo;
using DalyFrames::DalyMosfetStatus;

class DalyRS485
{
public:
    explicit DalyRS485(HardwareSerial &serial);

    void begin(int rxPin, int txPin, int sePin = -1, int enPin = -1, int pwr5vPin = -1);

    void setDebugCallback(DalyDebugCallback cb);

    bool readBasicInfo(DalyBasicInfo &info);
    bool readCellVoltages(uint8_t expectedCells, std::vector<float> &cellVoltages);
    bool readMosfetStatus(DalyMosfetStatus &status);
    bool readAlarmStatus(DalyAlarmStatus &status);

    // Reference to the table owned by DalyFrames::kAlarmBitNames(), kept as
    // a static member here so callers keep using
    // `DalyRS485::kAlarmBitNames[b][bit]`.
    static const char *const (&kAlarmBitNames)[7][8];

    // See DalyFrames::kCellMinPlausibleMv/kCellMaxPlausibleMv for the
    // rationale; readCellVoltages() rejects the whole read when it fails.
    static constexpr uint16_t kCellMinPlausibleMv = DalyFrames::kCellMinPlausibleMv;
    static constexpr uint16_t kCellMaxPlausibleMv = DalyFrames::kCellMaxPlausibleMv;

private:
    HardwareSerial *_serial;
    DalyDebugCallback _debugCb; // Stores the callback function
    DalyFrames::FrameAssembler _frameAssembler;

    // Edge-triggered flag for the "Rejected cell frame"/"plausible again"
    // log pair in readCellVoltages() - a member so state isn't shared
    // across DalyRS485 instances.
    bool _cellVoltagesRejecting = false;

    void sendCommand(DalyFrames::Cmd cmd);

    // Receives bytes until either a checksum-valid frame for `expected`
    // completes (payload filled, true returned) or the window
    // [windowStartMs, windowStartMs + windowMs) elapses. A checksum-valid
    // frame for a different command is discarded and the wait continues;
    // only a checksum failure optionally logs, via logChecksumFailures.
    // windowStartMs/windowMs are elapsed-based so a caller collecting
    // several frames can share one overall budget (see readCellVoltages()).
    bool receiveFrame(DalyFrames::Cmd expected, uint8_t payload[DalyFrames::kPayloadLen],
                       unsigned long windowStartMs, unsigned long windowMs, bool logChecksumFailures);

    // Sends `cmd` and waits up to timeoutMs for its single-frame reply,
    // writing the 8-byte payload into `payload` on success. One attempt,
    // no retry.
    bool query(DalyFrames::Cmd cmd, uint8_t payload[DalyFrames::kPayloadLen], unsigned long timeoutMs);

    void debugLog(const char *format, ...) __attribute__((format(printf, 2, 3)));
};