#include "DalyRS485.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>

namespace
{
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
// member): binds to the single table owned by DalyFrames::kAlarmBitNames().
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

bool DalyRS485::receiveFrame(DalyFrames::Cmd expected, uint8_t payload[DalyFrames::kPayloadLen],
                              unsigned long windowStartMs, unsigned long windowMs, bool logChecksumFailures)
{
    uint8_t frame[DalyFrames::kFrameLen];

    while (millis() - windowStartMs < windowMs)
    {
        if (_serial->available())
        {
            uint8_t c = _serial->read();
            if (_frameAssembler.feed(c, frame))
            {
                if (frame[2] == static_cast<uint8_t>(expected))
                {
                    std::memcpy(payload, &frame[4], DalyFrames::kPayloadLen);
                    return true;
                }
                // A checksum-valid frame for a different command: not ours,
                // no bytes lost - keep waiting for `expected`.
            }
            else if (logChecksumFailures && _frameAssembler.checksumFailedOnLastFeed())
            {
                debugLog("[DALY-LIB] Stream checksum failed. Continuing...\n");
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
    _frameAssembler.reset();
    sendCommand(cmd);
    return receiveFrame(cmd, payload, millis(), timeoutMs, /*logChecksumFailures=*/false);
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
    // The collector holds at most kMaxCollectorCells; reading mv() past
    // that would be out of bounds.
    if (expectedCells > DalyFrames::kMaxCollectorCells)
        return false;

    if (cellVoltages.size() != expectedCells)
    {
        cellVoltages.clear();
        cellVoltages.resize(expectedCells, 0.0f);
    }

    DalyFrames::CellFrameCollector collector;

    for (int retry = 0; retry < kCellVoltageRetries; retry++)
    {
        collector.reset(expectedCells);
        _frameAssembler.reset();
        sendCommand(DalyFrames::CellVoltages); // send the command EXACTLY ONCE

        // One receiveFrame() call per frame, all sharing the same
        // kCellVoltageWindowMs budget off `start`.
        unsigned long start = millis();
        while (millis() - start < kCellVoltageWindowMs && !collector.complete())
        {
            uint8_t payload[DalyFrames::kPayloadLen];
            if (!receiveFrame(DalyFrames::CellVoltages, payload, start, kCellVoltageWindowMs, /*logChecksumFailures=*/true))
                break; // window elapsed without another frame
            collector.accept(payload);
        }

        if (collector.complete())
        {
            const uint16_t *mv = collector.mv();
            for (int i = 0; i < expectedCells; i++)
                cellVoltages[i] = mv[i] / 1000.0f;

            // A checksum can still pass on a garbled value, and bmsTask
            // seeds its whole moving-average window from the first
            // successful read - so reject the whole read, same as a missed
            // frame, if any cell falls outside a plausible LiFePO4 range;
            // the stale-data fail-safe then drops limits to 0A.
            int badIndex;
            if (!DalyFrames::cellVoltagesPlausible(cellVoltages.data(), expectedCells, badIndex))
            {
                if (!_cellVoltagesRejecting)
                {
                    uint16_t mv = (uint16_t)(cellVoltages[badIndex] * 1000.0f + 0.5f);
                    debugLog("[BMS] Rejected cell frame: cell %d = %u mV outside 1.5-4.5 V\n", badIndex + 1, mv);
                    _cellVoltagesRejecting = true;
                }
                cellVoltages.assign(expectedCells, 0.0f);
                return false;
            }

            if (_cellVoltagesRejecting)
            {
                debugLog("[BMS] Cell frames plausible again\n");
                _cellVoltagesRejecting = false;
            }

            return true; // We got them all!
        }

        debugLog("[DALY-LIB] Missed frames. Got %d/%d. Retrying...\n", collector.framesReceived(), collector.expectedFrames());
        vTaskDelay(pdMS_TO_TICKS(kCellVoltageRetryPauseMs)); // Pause before retry
    }
    return false;
}

bool DalyRS485::readMosfetStatus(DalyMosfetStatus &status)
{
    uint8_t data[DalyFrames::kPayloadLen];

    if (!query(DalyFrames::MosfetStatus, data, kSingleFrameTimeoutMs))
        return false;

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

    return DalyFrames::parseAlarmStatus(data, status);
}
