#pragma once
#include <Arduino.h>
#include <vector>
#include "DalyFrames.h"

// Define a function pointer type for passing debug strings
typedef void (*DalyDebugCallback)(const char *msg);

// The plain data structs live in DalyFrames.h now (#30), alongside the pure
// byte-level parsers that fill them, so both compile and run under
// `pio test -e native`. These using-declarations keep the unqualified names
// working for existing callers (main.cpp) unchanged.
using DalyFrames::DalyAlarmStatus;
using DalyFrames::DalyBasicInfo;
using DalyFrames::DalyMosfetStatus;

class DalyRS485
{
public:
    explicit DalyRS485(HardwareSerial &serial);

    void begin(int rxPin, int txPin, int sePin = -1, int enPin = -1, int pwr5vPin = -1);

    // Allow the main application to attach a logging function
    void setDebugCallback(DalyDebugCallback cb);

    bool readBasicInfo(DalyBasicInfo &info);
    bool readCellVoltages(uint8_t expectedCells, std::vector<float> &cellVoltages);
    bool readMosfetStatus(DalyMosfetStatus &status);
    bool readAlarmStatus(DalyAlarmStatus &status);

    // Name table for the 0x98 alarm payload, bytes 0-6 (byte 7 is the
    // numeric fault code, not a bitfield). Indexed [byte][bit]; nullptr for
    // bits not defined in the documented protocol. Reference to the table
    // owned by DalyFrames::kAlarmBitNames() (#30) - kept as a static member
    // here so existing callers (main.cpp) keep using
    // `DalyRS485::kAlarmBitNames[b][bit]` unchanged.
    static const char *const (&kAlarmBitNames)[7][8];

    // Plausible cell-voltage envelope for a LiFePO4 cell - see
    // DalyFrames::kCellMinPlausibleMv/kCellMaxPlausibleMv (#30) for the
    // rationale. Anything outside 1.5-4.5 V is either a wiring/parse fault
    // or a pack that must not be charged/discharged anyway, so
    // readCellVoltages() rejects the whole read rather than feed the value
    // into bmsTask's moving-average filter (which seeds its entire window
    // from the first successful reading - one bad checksum-passing value
    // would get full weight for a whole window). Rejecting makes the data
    // go stale, which drops the glideslope limits to 0 A (fail-safe).
    static constexpr uint16_t kCellMinPlausibleMv = DalyFrames::kCellMinPlausibleMv;
    static constexpr uint16_t kCellMaxPlausibleMv = DalyFrames::kCellMaxPlausibleMv;

private:
    HardwareSerial *_serial;
    DalyDebugCallback _debugCb; // Stores the callback function

    void sendCommand(uint8_t cmd);
    bool receiveSingleFrame(uint8_t expectedCmd, uint8_t *dataOut, unsigned long timeout = 150);

    // Internal variadic logger (works exactly like printf)
    void debugLog(const char *format, ...);
};