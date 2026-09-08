#include "DalyRS485.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>

DalyRS485::DalyRS485(HardwareSerial &serial)
    : _serial(&serial), _debugCb(nullptr) {}

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

    vTaskDelay(pdMS_TO_TICKS(50));
    _serial->begin(9600, SERIAL_8N1, rxPin, txPin);
}

void DalyRS485::setDebugCallback(DalyDebugCallback cb)
{
    _debugCb = cb;
}

void DalyRS485::sendCommand(uint8_t cmd)
{
    while (_serial->available())
    {
        _serial->read();
    }

    uint8_t frame[13] = {0xA5, 0x40, cmd, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t checksum = 0;
    for (int i = 0; i < 12; i++)
        checksum += frame[i];
    frame[12] = checksum;

    _serial->write(frame, 13);
    _serial->flush();

    // RTOS safe delay to allow the RS485 hardware stop-bit to physically leave the pin
    vTaskDelay(pdMS_TO_TICKS(2));
}

bool DalyRS485::receiveSingleFrame(uint8_t expectedCmd, uint8_t *dataOut, unsigned long timeout)
{
    unsigned long start = millis();
    uint8_t buf[13];
    int idx = 0;

    while (millis() - start < timeout)
    {
        if (_serial->available())
        {
            uint8_t c = _serial->read();
            if (idx == 0 && c != 0xA5)
                continue;

            buf[idx++] = c;
            if (idx == 13)
            {
                uint8_t checksum = 0;
                for (int i = 0; i < 12; i++)
                    checksum += buf[i];

                if (checksum == buf[12] && buf[2] == expectedCmd)
                {
                    std::memcpy(dataOut, &buf[4], 8);
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

bool DalyRS485::readBasicInfo(DalyBasicInfo &info)
{
    sendCommand(0x90);
    uint8_t data[8];

    if (receiveSingleFrame(0x90, data, 150))
    {
        info.packVoltage = ((data[0] << 8) | data[1]) / 10.0f;
        uint16_t currentOffset = (data[4] << 8) | data[5];
        info.packCurrent = (currentOffset - 30000) / 10.0f;
        info.packSOC = ((data[6] << 8) | data[7]) / 10.0f;
        return true;
    }
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

    for (int retry = 0; retry < 2; retry++)
    {
        // 1. Send the command EXACTLY ONCE
        sendCommand(0x95);

        int framesReceivedCount = 0;
        uint8_t framesMask = 0;
        unsigned long start = millis();

        uint8_t buf[13];
        int idx = 0;

        // 2. Sit quietly and catch all 6 frames as the BMS streams them out
        while (millis() - start < 800 && framesReceivedCount < expectedFrames)
        {
            if (_serial->available())
            {
                uint8_t c = _serial->read();
                if (idx == 0 && c != 0xA5)
                    continue;

                buf[idx++] = c;
                if (idx == 13)
                {
                    uint8_t checksum = 0;
                    for (int i = 0; i < 12; i++)
                        checksum += buf[i];

                    if (checksum == buf[12] && buf[2] == 0x95)
                    {
                        uint8_t frameNum = buf[4];

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
                                    {
                                        uint16_t mv = (buf[5 + i * 2] << 8) | buf[6 + i * 2];
                                        cellVoltages[cellIdx] = mv / 1000.0f;
                                    }
                                }
                            }
                        }
                    }
                    else if (checksum != buf[12])
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
            return true; // We got them all!
        }

        debugLog("[DALY-LIB] Missed frames. Got %d/%d. Retrying...\n", framesReceivedCount, expectedFrames);
        vTaskDelay(pdMS_TO_TICKS(100)); // Pause before retry
    }
    return false;
}

bool DalyRS485::readMosfetStatus(DalyMosfetStatus &status)
{
    sendCommand(0x93);
    uint8_t data[8];

    if (!receiveSingleFrame(0x93, data, 150))
        return false;

    // Daly UART "Status Info 2" (cmd 0x93) payload layout, from the same
    // community-documented protocol family as the 0x90/0x95 frames above
    // (e.g. syssi/esphome-daly-bms, patman15 Daly UART docs):
    //   [0] charge/discharge status (0=stall, 1=charge, 2=discharge)
    //   [1] charge MOSFET state (0=off, 1=on)
    //   [2] discharge MOSFET state (0=off, 1=on)
    //   [3] BMS life cycle count
    //   [4..7] remaining capacity, Ah*1000
    // NOT yet verified byte-for-byte against this specific pack's firmware -
    // sanity-check against a serial monitor / known MOSFET state after
    // flashing. Defensive: MOSFET bytes must be 0 or 1; anything else means
    // this frame isn't what we think it is, so report failure instead of
    // guessing.
    if (data[1] > 1 || data[2] > 1)
    {
        debugLog("[DALY-LIB] 0x93 MOSFET bytes out of expected range (%d,%d). Ignoring frame.\n", data[1], data[2]);
        return false;
    }

    status.chargeMosOn = (data[1] == 1);
    status.dischargeMosOn = (data[2] == 1);
    return true;
}

bool DalyRS485::readAlarmStatus(DalyAlarmStatus &status)
{
    sendCommand(0x98);
    uint8_t data[8];

    if (!receiveSingleFrame(0x98, data, 150))
        return false;

    // Daly UART "Alarm Info" (cmd 0x98) payload layout, same documented
    // protocol family as above. Byte 0 carries cell/pack voltage alarm
    // bits (bytes 1-6 carry temperature/current/SOC/other protection bits,
    // not decoded here since they're not relevant to this investigation):
    //   [0] bit0/1 = cell overvolt level1/2, bit2/3 = cell undervolt level1/2,
    //       bit4/5 = pack overvolt level1/2, bit6/7 = pack undervolt level1/2
    // NOT yet verified byte-for-byte against this specific pack's firmware -
    // sanity-check against a serial monitor after flashing, e.g. by
    // temporarily lowering cvMaxCharge below the pack's real voltage and
    // confirming bit0 sets.
    status.cellOvervoltLevel1 = data[0] & 0x01;
    status.cellOvervoltLevel2 = data[0] & 0x02;
    status.packOvervoltLevel1 = data[0] & 0x10;
    status.packOvervoltLevel2 = data[0] & 0x20;

    status.anyProtectionActive = false;
    for (int i = 0; i < 7; i++)
    {
        if (data[i] != 0)
        {
            status.anyProtectionActive = true;
            break;
        }
    }
    return true;
}