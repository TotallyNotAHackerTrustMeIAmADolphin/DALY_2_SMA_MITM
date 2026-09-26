#pragma once
#include <Arduino.h>
#include "driver/twai.h"
#include "SystemState.h"
#include "SMAFrames.h"

typedef void (*SMADebugCallback)(const char *msg);

// SMATxData is SMAFrames::SMATxData under its old name (#45) - same pattern
// as DalyRS485.h re-exposing DalyFrames.h's structs, so existing callers
// (main.cpp) are unchanged.
using SMATxData = SMAFrames::SMATxData;

class SMA_CAN
{
public:
    SMA_CAN();

    // --> ADDED sePin HERE <--
    bool begin(gpio_num_t txPin, gpio_num_t rxPin, gpio_num_t sePin);

    void setDebugCallback(SMADebugCallback cb);
    void checkBusHealth();
    void readMessages(DashboardData &dashboardOut);
    void sendStatus(const SMATxData &data);

private:
    SMADebugCallback _debugCb;
    uint8_t _ticker35E;

    bool _wasBusOff;
    unsigned long _recoveryTimer;
    gpio_num_t _txPin;
    gpio_num_t _rxPin;
    gpio_num_t _sePin; // Added

    void debugLog(const char *format, ...) __attribute__((format(printf, 2, 3)));
    void sendFrame(uint32_t id, uint8_t dlc, const uint8_t *data);
};