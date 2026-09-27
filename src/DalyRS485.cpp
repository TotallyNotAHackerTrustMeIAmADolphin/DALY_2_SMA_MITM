#include "DalyRS485.h"
#include <cstring>

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

void DalyRS485::setDebugCallback(LogSink cb)
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
                              unsigned long windowStartMs, unsigned long windowMs)
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
    return receiveFrame(cmd, payload, millis(), timeoutMs);
}

bool DalyRS485::readBasicInfo(DalyBasicInfo &info)
{
    uint8_t data[DalyFrames::kPayloadLen];

    if (query(DalyFrames::BasicInfo, data, kSingleFrameTimeoutMs))
        return DalyFrames::parseBasicInfo(data, info);
    return false;
}

bool DalyRS485::readCellVoltages(uint16_t *cellMv)
{
    DalyFrames::CellFrameCollector collector;

    for (int retry = 0; retry < kCellVoltageRetries; retry++)
    {
        collector.reset();
        _frameAssembler.reset();
        sendCommand(DalyFrames::CellVoltages); // send the command EXACTLY ONCE

        // One receiveFrame() call per frame, all sharing the same
        // kCellVoltageWindowMs budget off `start` - receiveFrame() itself
        // enforces the window, so a break on its false return is enough.
        unsigned long start = millis();
        while (!collector.complete())
        {
            uint8_t payload[DalyFrames::kPayloadLen];
            if (!receiveFrame(DalyFrames::CellVoltages, payload, start, kCellVoltageWindowMs))
                break; // window elapsed without another frame
            collector.accept(payload);
        }

        if (collector.complete())
        {
            const uint16_t *mv = collector.mv();

            // A checksum can still pass on a garbled value, and bmsTask
            // seeds its whole moving-average window from the first
            // successful read - so reject the whole read, same as a missed
            // frame, if any cell falls outside a plausible LiFePO4 range;
            // the stale-data fail-safe then drops limits to 0A.
            int badIndex;
            if (!DalyFrames::cellVoltagesPlausible(mv, kPackCells, badIndex))
            {
                if (!_cellVoltagesRejecting)
                {
                    logTo(_debugCb, "[BMS] Rejected cell frame: cell %d = %u mV outside 1.5-4.5 V\n", badIndex + 1, mv[badIndex]);
                    _cellVoltagesRejecting = true;
                }
                for (int i = 0; i < kPackCells; i++)
                    cellMv[i] = 0;
                return false;
            }

            if (_cellVoltagesRejecting)
            {
                logTo(_debugCb, "[BMS] Cell frames plausible again\n");
                _cellVoltagesRejecting = false;
            }

            for (int i = 0; i < kPackCells; i++)
                cellMv[i] = mv[i];
            return true; // We got them all!
        }

        logTo(_debugCb, "[DALY-LIB] Missed frames. Got %d/%d. Retrying...\n", collector.framesReceived(), collector.expectedFrames());
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
        logTo(_debugCb, "[DALY-LIB] 0x93 MOSFET bytes out of expected range (%d,%d). Ignoring frame.\n", data[1], data[2]);
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
