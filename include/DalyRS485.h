#pragma once
#include <Arduino.h>
#include <vector>

// Define a function pointer type for passing debug strings
typedef void (*DalyDebugCallback)(const char *msg);

struct DalyBasicInfo
{
    float packVoltage;
    float packCurrent;
    float packSOC;
};

struct DalyMosfetStatus
{
    bool chargeMosOn;
    bool dischargeMosOn;
};

// Subset of the Daly "Alarm Info" (cmd 0x98) protection bitfield - only the
// bits relevant to diagnosing an SMA-side "battery voltage out of range"
// fault are decoded (see readAlarmStatus() for the byte layout/provenance).
struct DalyAlarmStatus
{
    bool cellOvervoltLevel1;
    bool cellOvervoltLevel2;
    bool packOvervoltLevel1;
    bool packOvervoltLevel2;
    bool anyProtectionActive; // true if any byte in the alarm frame is nonzero
};

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

private:
    HardwareSerial *_serial;
    DalyDebugCallback _debugCb; // Stores the callback function

    void sendCommand(uint8_t cmd);
    bool receiveSingleFrame(uint8_t expectedCmd, uint8_t *dataOut, unsigned long timeout = 150);

    // Internal variadic logger (works exactly like printf)
    void debugLog(const char *format, ...);
};