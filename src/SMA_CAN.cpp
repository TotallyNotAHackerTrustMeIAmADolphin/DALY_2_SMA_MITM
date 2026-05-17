#include "SMA_CAN.h"
#include <cstdarg>
#include <cstdio>

SMA_CAN::SMA_CAN() : _debugCb(nullptr), _ticker35E(0),
                     _wasBusOff(false), _recoveryTimer(0) {}

void SMA_CAN::setDebugCallback(SMADebugCallback cb)
{
    _debugCb = cb;
}

void SMA_CAN::debugLog(const char *format, ...)
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

bool SMA_CAN::begin(gpio_num_t txPin, gpio_num_t rxPin, gpio_num_t sePin)
{
    _txPin = txPin;
    _rxPin = rxPin;
    _sePin = sePin;

    // CRITICAL FIX: Wake up the CAN Transmitter!
    if (_sePin != GPIO_NUM_NC)
    {
        pinMode(_sePin, OUTPUT);
        digitalWrite(_sePin, LOW); // LOW = High Speed TX Mode. HIGH = Sleep Mode.
    }

    // Set back to NORMAL mode!
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(txPin, rxPin, TWAI_MODE_NORMAL);
    g_config.tx_queue_len = 10;
    g_config.rx_queue_len = 10;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK)
    {
        if (twai_start() == ESP_OK)
        {
            debugLog("[CAN] TWAI Driver installed and running at 500kbps.\n");
            return true;
        }
    }
    debugLog("[CAN] Failed to initialize TWAI Driver.\n");
    return false;
}

void SMA_CAN::checkBusHealth()
{
    if (_wasBusOff)
    {
        if (millis() - _recoveryTimer > 1000)
        {
            debugLog("[CAN] Reinstalling TWAI Driver...\n");
            if (begin(_txPin, _rxPin, _sePin))
            {
                _wasBusOff = false;
            }
            else
            {
                _recoveryTimer = millis();
            }
        }
        return;
    }

    twai_status_info_t twai_stat;
    if (twai_get_status_info(&twai_stat) != ESP_OK)
        return;

    if (twai_stat.state == TWAI_STATE_BUS_OFF)
    {
        _wasBusOff = true;
        _recoveryTimer = millis();
        debugLog("[CAN] Bus-Off! Bypassing ESP-IDF bug with a nuclear driver reset...\n");
        twai_driver_uninstall();
    }
}

void SMA_CAN::sendFrame(uint32_t id, uint8_t dlc, uint8_t *data)
{
    if (_wasBusOff)
        return;

    twai_status_info_t twai_stat;
    if (twai_get_status_info(&twai_stat) != ESP_OK || twai_stat.state != TWAI_STATE_RUNNING)
    {
        return;
    }

    if (twai_stat.msgs_to_tx >= 5)
        return;

    twai_message_t msg;
    msg.identifier = id;
    msg.extd = 0;
    msg.rtr = 0;
    msg.data_length_code = dlc;
    for (int i = 0; i < dlc; i++)
    {
        msg.data[i] = data[i];
    }

    twai_transmit(&msg, 0);
}

void SMA_CAN::sendStatus(const SMATxData &data)
{
    if (_wasBusOff)
        return;

    uint8_t frame[8];

    frame[0] = data.cvl & 0xFF;
    frame[1] = (data.cvl >> 8) & 0xFF;
    frame[2] = data.ccl & 0xFF;
    frame[3] = (data.ccl >> 8) & 0xFF;
    frame[4] = data.dcl & 0xFF;
    frame[5] = (data.dcl >> 8) & 0xFF;
    frame[6] = data.isResetting ? 0x00 : (data.maintenanceActive ? 0x70 : 0xC0);
    frame[7] = 0x00;
    sendFrame(0x351, 8, frame);

    uint16_t outSOC = data.maintenanceActive ? 2 : (uint16_t)round(data.packSOC);
    frame[0] = outSOC & 0xFF;
    frame[1] = (outSOC >> 8) & 0xFF;
    frame[2] = 100;
    frame[3] = 0;
    sendFrame(0x355, 4, frame);

    uint16_t v_out = (uint16_t)round(data.packVoltage * 100.0f);
    int16_t i_out = (int16_t)round(data.packCurrent * 10.0f);
    frame[0] = v_out & 0xFF;
    frame[1] = (v_out >> 8) & 0xFF;
    frame[2] = i_out & 0xFF;
    frame[3] = (i_out >> 8) & 0xFF;
    frame[4] = data.packTemp & 0xFF;
    frame[5] = (data.packTemp >> 8) & 0xFF;
    sendFrame(0x356, 6, frame);

    memset(frame, 0, 8);
    if (data.maintenanceActive)
        frame[0] |= 0x10;
    sendFrame(0x359, 8, frame);

    if (++_ticker35E > 10)
    {
        _ticker35E = 0;
        uint8_t smaId[8] = {'S', 0, 'M', 0, 'A', 0, 0, 0};
        sendFrame(0x35E, 8, smaId);
        uint8_t mfg[8] = {3, 0, 0, 0, 0x48, 0x03, 0, 0};
        sendFrame(0x35F, 8, mfg);
    }
}

void SMA_CAN::readMessages(DashboardData &dashboardOut)
{
    if (_wasBusOff)
        return;

    twai_message_t in_msg;
    int msgCount = 0;

    while (twai_receive(&in_msg, 0) == ESP_OK && msgCount < 10)
    {
        msgCount++;

        if (in_msg.identifier == 0x305 && in_msg.data_length_code > 0)
        {
            uint8_t m = in_msg.data[0];
            dashboardOut.smaChargeMode = (m == 1) ? "Bulk" : (m == 2) ? "Absorption"
                                                         : (m == 3)   ? "Float"
                                                                      : "Equalize";
        }
        if (in_msg.identifier == 0x300 && in_msg.data_length_code > 0)
        {
            dashboardOut.gridPresent = (in_msg.data[0] & 0x01);
        }
    }
}