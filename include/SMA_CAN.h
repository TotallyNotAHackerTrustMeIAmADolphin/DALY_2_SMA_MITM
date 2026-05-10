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
    int packSOC;
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

    bool begin(gpio_num_t txPin, gpio_num_t rxPin);
    void setDebugCallback(SMADebugCallback cb);
    void checkBusHealth();
    void readMessages(DashboardData &dashboardOut);
    void sendStatus(const SMATxData &data);

private:
    SMADebugCallback _debugCb;
    uint8_t _ticker35E;
    uint16_t _lastSmaErrorCode;
    bool _wasBusOff; // <-- ADDED: Tracks error state to prevent log flooding

    void debugLog(const char *format, ...);
    void sendFrame(uint32_t id, uint8_t dlc, uint8_t *data);
};