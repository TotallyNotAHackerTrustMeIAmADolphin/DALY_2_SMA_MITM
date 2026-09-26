#pragma once
#include <Arduino.h>
#include "driver/twai.h"
#include "SystemState.h"
#include "SMAFrames.h"
#include "LogSink.h"

// SMATxData is SMAFrames::SMATxData under its old name, same pattern as
// DalyRS485.h re-exposing DalyFrames.h's structs.
using SMATxData = SMAFrames::SMATxData;

class SMA_CAN
{
public:
    SMA_CAN();

    bool begin(gpio_num_t txPin, gpio_num_t rxPin, gpio_num_t sePin);

    void setDebugCallback(LogSink cb);
    void checkBusHealth();
    void readMessages(DashboardData &dashboardOut);
    void sendStatus(const SMATxData &data);

private:
    LogSink _debugCb;
    uint8_t _ticker35E;

    // Driver uninstalled (bus-off) or never started: checkBusHealth()
    // retries startDriver() every second while set.
    bool _driverDown;
    bool _startFailed; // last startDriver() failed; mutes repeat log lines
    unsigned long _recoveryTimer;
    gpio_num_t _txPin;
    gpio_num_t _rxPin;
    gpio_num_t _sePin;

    bool startDriver();
    void markDriverDown();
    void sendFrame(uint32_t id, uint8_t dlc, const uint8_t *data);
};