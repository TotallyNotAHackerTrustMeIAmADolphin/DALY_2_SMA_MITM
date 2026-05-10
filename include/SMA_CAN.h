#pragma once
#include <Arduino.h>
#include "driver/twai.h"
#include "SystemState.h"

typedef void (*SMADebugCallback)(const char *msg);

struct SMATxData
{
    float packVoltage;
    float packCurrent;
    int16_t packTemp;
    float packSOC;
    uint16_t ccl;
    uint16_t dcl;
    uint16_t cvl;
    uint16_t dvl;
    bool maintenanceActive;
    bool isResetting;
};

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
    uint16_t _lastSmaErrorCode;

    bool _wasBusOff;
    unsigned long _recoveryTimer;
    gpio_num_t _txPin;
    gpio_num_t _rxPin;
    gpio_num_t _sePin; // Added

    void debugLog(const char *format, ...);
    void sendFrame(uint32_t id, uint8_t dlc, uint8_t *data);
};