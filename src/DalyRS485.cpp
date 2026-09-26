#include "DalyRS485.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>

namespace
{
    // Named timing/retry constants (#103) - bare literals before this,
    // scattered across sendCommand()/receiveSingleFrame()/readCellVoltages().
    // Values unchanged; only naming them here.
    constexpr unsigned long kUartBaud = 9600;
    constexpr unsigned long kPowerOnSettleMs = 50;    // begin(): let RS485/CAN transceiver power settle
    constexpr unsigned long kStopBitSettleMs = 2;     // sendCommand(): let the stop bit leave the pin
    constexpr unsigned long kSingleFrameTimeoutMs = 150; // readBasicInfo/readMosfetStatus/readAlarmStatus
    constexpr unsigned long kCellVoltageWindowMs = 800;  // readCellVoltages(): time to catch all frames
    constexpr unsigned long kCellVoltageRetryPauseMs = 100;
    constexpr int kCellVoltageRetries = 2;
}

DalyRS485::DalyRS485(HardwareSerial &serial)
    : _serial(&serial), _debugCb(nullptr) {}

// Defined out-of-class (required for a non-constexpr reference static data
// member): binds to the single table owned by DalyFrames::kAlarmBitNames()
// (#30). See the declaration in DalyRS485.h for why this stays a static
// member instead of just pointing callers at DalyFrames directly.
const char *const (&DalyRS485::kAlarmBitNames)[7][8] = DalyFrames::kAlarmBitNames();

void DalyRS485::debugLog(const char *format, ...)
{
    if (!_debugCb)
        return;

    char loc_res[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_res, sizeof(loc_res), format, arg);
    va_end(arg);

    _debugCb(loc_res);
}

void DalyRS485::begin(int rxPin, int txPin, int sePin, int enPin, int pwr5vPin)
{
    if (pwr5vPin >= 0)
    {
        pinMode(pwr5vPin, OUTPUT);
        digitalWrite(pwr5vPin, HIGH);
    }
    if (sePin >= 0)
    {
        pinMode(sePin, OUTPUT);
        digitalWrite(sePin, HIGH);
    }
    if (enPin >= 0)
    {
        pinMode(enPin, OUTPUT);
        digitalWrite(enPin, HIGH);
    }

    vTaskDelay(pdMS_TO_TICKS(kPowerOnSettleMs));
    _serial->begin(kUartBaud, SERIAL_8N1, rxPin, txPin);
}

void DalyRS485::setDebugCallback(DalyDebugCallback cb)
{
    _debugCb = cb;
}

void DalyRS485::sendCommand(DalyFrames::Cmd cmd)
{
    while (_serial->available())
    {
        _serial->read();
    }

    uint8_t frame[DalyFrames::kFrameLen];
    DalyFrames::buildRequest(cmd, frame);

    _serial->write(frame, DalyFrames::kFrameLen);
    _serial->flush();

    // RTOS safe delay to allow the RS485 hardware stop-bit to physically leave the pin
    vTaskDelay(pdMS_TO_TICKS(kStopBitSettleMs));
}

bool DalyRS485::receiveSingleFrame(DalyFrames::Cmd expectedCmd, uint8_t *dataOut, unsigned long timeout)
{
    unsigned long start = millis();
    uint8_t buf[DalyFrames::kFrameLen];
    int idx = 0;

    while (millis() - start < timeout)
    {
        if (_serial->available())
        {
            uint8_t c = _serial->read();
            if (idx == 0 && c != DalyFrames::kStartByte)
                continue;

            buf[idx++] = c;
            if (idx == DalyFrames::kFrameLen)
            {
                if (DalyFrames::checksumOk(buf) && buf[2] == static_cast<uint8_t>(expectedCmd))
                {
                    std::memcpy(dataOut, &buf[4], DalyFrames::kPayloadLen);
                    return true;
                }
                idx = 0;
            }
        }
        else
        {
            // Drop CPU usage to 1% while waiting for serial bits
            vTaskDelay(1);
        }
    }
    return false;
}

bool DalyRS485::query(DalyFrames::Cmd cmd, uint8_t payload[DalyFrames::kPayloadLen], unsigned long timeoutMs)
{
    sendCommand(cmd);
    return receiveSingleFrame(cmd, payload, timeoutMs);
}

bool DalyRS485::readBasicInfo(DalyBasicInfo &info)
{
    uint8_t data[DalyFrames::kPayloadLen];

    if (query(DalyFrames::BasicInfo, data, kSingleFrameTimeoutMs))
        return DalyFrames::parseBasicInfo(data, info);
    return false;
}

bool DalyRS485::readCellVoltages(uint8_t expectedCells, std::vector<float> &cellVoltages)
{
    if (cellVoltages.size() != expectedCells)
    {
        cellVoltages.clear();
        cellVoltages.resize(expectedCells, 0.0f);
    }

    int expectedFrames = (expectedCells + 2) / 3;

    for (int retry = 0; retry < kCellVoltageRetries; retry++)
    {
        // 1. Send the command EXACTLY ONCE
        sendCommand(DalyFrames::CellVoltages);

        int framesReceivedCount = 0;
        uint8_t framesMask = 0;
        unsigned long start = millis();

        uint8_t buf[DalyFrames::kFrameLen];
        int idx = 0;

        // 2. Sit quietly and catch all 6 frames as the BMS streams them out
        while (millis() - start < kCellVoltageWindowMs && framesReceivedCount < expectedFrames)
        {
            if (_serial->available())
            {
                uint8_t c = _serial->read();
                if (idx == 0 && c != DalyFrames::kStartByte)
                    continue;

                buf[idx++] = c;
                if (idx == DalyFrames::kFrameLen)
                {
                    if (DalyFrames::checksumOk(buf) && buf[2] == static_cast<uint8_t>(DalyFrames::CellVoltages))
                    {
                        uint8_t frameNum;
                        uint16_t mv[3];
                        DalyFrames::parseCellFrame(&buf[4], frameNum, mv);

                        if (frameNum > 0 && frameNum <= expectedFrames)
                        {
                            // Check if we haven't seen this specific frame yet
                            if (!(framesMask & (1 << frameNum)))
                            {
                                framesMask |= (1 << frameNum);
                                framesReceivedCount++;

                                for (int i = 0; i < 3; i++)
                                {
                                    int cellIdx = (frameNum - 1) * 3 + i;
                                    if (cellIdx < expectedCells)
                                        cellVoltages[cellIdx] = mv[i] / 1000.0f;
                                }
                            }
                        }
                    }
                    else if (!DalyFrames::checksumOk(buf))
                    {
                        debugLog("[DALY-LIB] Stream checksum failed. Continuing...\n");
                    }
                    idx = 0; // Reset for the next frame in the stream
                }
            }
            else
            {
                // 3. Keep the Web Server flying while we wait for bytes!
                vTaskDelay(1);
            }
        }

        if (framesReceivedCount == expectedFrames)
        {
            // All frames parsed cleanly on the wire (checksums passed), but a
            // checksum can still pass on a garbled value. bmsTask seeds its
            // whole moving-average window from the first successful read, so
            // one implausible cell here would get full weight for an entire
            // smoothing window. Reject the whole read - same as a missed
            // frame - if any cell falls outside a physically plausible
            // LiFePO4 range; the caller's stale-data fail-safe then drops
            // limits to 0A instead of trusting the value.
            static bool rejecting = false;
            int badIndex;
            if (!DalyFrames::cellVoltagesPlausible(cellVoltages.data(), expectedCells, badIndex))
            {
                if (!rejecting)
                {
                    uint16_t mv = (uint16_t)(cellVoltages[badIndex] * 1000.0f + 0.5f);
                    debugLog("[BMS] Rejected cell frame: cell %d = %u mV outside 1.5-4.5 V\n", badIndex + 1, mv);
                    rejecting = true;
                }
                cellVoltages.assign(expectedCells, 0.0f);
                return false;
            }

            if (rejecting)
            {
                debugLog("[BMS] Cell frames plausible again\n");
                rejecting = false;
            }

            return true; // We got them all!
        }

        debugLog("[DALY-LIB] Missed frames. Got %d/%d. Retrying...\n", framesReceivedCount, expectedFrames);
        vTaskDelay(pdMS_TO_TICKS(kCellVoltageRetryPauseMs)); // Pause before retry
    }
    return false;
}

bool DalyRS485::readMosfetStatus(DalyMosfetStatus &status)
{
    uint8_t data[DalyFrames::kPayloadLen];

    if (!query(DalyFrames::MosfetStatus, data, kSingleFrameTimeoutMs))
        return false;

    // See DalyFrames::parseMosfetStatus() for the 0x93 byte layout this
    // rejects on and its provenance/caveats.
    if (!DalyFrames::parseMosfetStatus(data, status))
    {
        debugLog("[DALY-LIB] 0x93 MOSFET bytes out of expected range (%d,%d). Ignoring frame.\n", data[1], data[2]);
        return false;
    }
    return true;
}

bool DalyRS485::readAlarmStatus(DalyAlarmStatus &status)
{
    uint8_t data[DalyFrames::kPayloadLen];

    if (!query(DalyFrames::AlarmStatus, data, kSingleFrameTimeoutMs))
        return false;

    // See DalyFrames::parseAlarmStatus() for the 0x98 byte layout this
    // decodes and its provenance/caveats.
    return DalyFrames::parseAlarmStatus(data, status);
}
