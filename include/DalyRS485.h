#pragma once
#include <Arduino.h>
#include "DalyFrames.h"
#include "LogSink.h"

// The plain data structs and pure byte-level parsers live in DalyFrames.h,
// so both compile and run under `pio test -e native`. These using-
// declarations keep the unqualified names below working.
using DalyFrames::DalyAlarmStatus;
using DalyFrames::DalyBasicInfo;
using DalyFrames::DalyMosfetStatus;

class DalyRS485
{
public:
    explicit DalyRS485(HardwareSerial &serial);

    void begin(int rxPin, int txPin, int sePin = -1, int enPin = -1, int pwr5vPin = -1);

    void setDebugCallback(LogSink cb);

    bool readBasicInfo(DalyBasicInfo &info);
    // cellMv must have at least expectedCells entries; filled in millivolts
    // on success, zeroed on a plausibility-gate rejection.
    bool readCellVoltages(uint8_t expectedCells, uint16_t *cellMv);
    bool readMosfetStatus(DalyMosfetStatus &status);
    bool readAlarmStatus(DalyAlarmStatus &status);

private:
    HardwareSerial *_serial;
    LogSink _debugCb;
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
};