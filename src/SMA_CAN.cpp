#include "SMA_CAN.h"
#include <cstdarg>
#include <cstdio>

namespace
{
    constexpr uint32_t kTwaiQueueLen = 10; // both TWAI queues
    // Frames drained per readMessages() call, so a flooded bus can't
    // starve canTask's status TX; the rest wait in the queue.
    constexpr int kRxBatchMax = 10;
}

SMA_CAN::SMA_CAN() : _debugCb(nullptr), _ticker35E(0),
                     _driverDown(false), _startFailed(false), _recoveryTimer(0) {}

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

    if (_sePin != GPIO_NUM_NC)
    {
        pinMode(_sePin, OUTPUT);
        digitalWrite(_sePin, LOW); // LOW = High Speed TX Mode. HIGH = Sleep Mode.
    }

    // A failed boot start is retried by checkBusHealth() like a bus-off
    // recovery - the SMA must not be left without frames until reboot.
    bool ok = startDriver();
    if (!ok)
        markDriverDown();
    return ok;
}

bool SMA_CAN::startDriver()
{
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(_txPin, _rxPin, TWAI_MODE_NORMAL);
    g_config.tx_queue_len = kTwaiQueueLen;
    g_config.rx_queue_len = kTwaiQueueLen;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    bool ok = false;
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK)
    {
        if (twai_start() == ESP_OK)
            ok = true;
        else
            // Installed but not started: uninstall, or every later install
            // fails with ESP_ERR_INVALID_STATE and recovery never succeeds.
            twai_driver_uninstall();
    }

    // Edge-triggered: one failure line per retry streak is enough.
    if (ok)
        debugLog("[CAN] TWAI Driver installed and running at 500kbps.\n");
    else if (!_startFailed)
        debugLog("[CAN] Failed to initialize TWAI Driver - retrying every second.\n");
    _startFailed = !ok;
    return ok;
}

void SMA_CAN::markDriverDown()
{
    _driverDown = true;
    _recoveryTimer = millis();
}

void SMA_CAN::checkBusHealth()
{
    if (_driverDown)
    {
        if (SMAFrames::shouldRetryBusRecovery(millis(), _driverDown, _recoveryTimer))
        {
            if (!_startFailed)
                debugLog("[CAN] Reinstalling TWAI Driver...\n");
            if (startDriver())
                _driverDown = false;
            else
                _recoveryTimer = millis();
        }
        return;
    }

    twai_status_info_t twai_stat;
    if (twai_get_status_info(&twai_stat) != ESP_OK)
        return;

    if (twai_stat.state == TWAI_STATE_BUS_OFF)
    {
        markDriverDown();
        debugLog("[CAN] Bus-Off! Bypassing ESP-IDF bug with a nuclear driver reset...\n");
        twai_driver_uninstall();
    }
}

void SMA_CAN::sendFrame(uint32_t id, uint8_t dlc, const uint8_t *data)
{
    if (_driverDown)
        return;

    twai_status_info_t twai_stat;
    if (twai_get_status_info(&twai_stat) != ESP_OK || twai_stat.state != TWAI_STATE_RUNNING)
    {
        return;
    }

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
    if (_driverDown)
        return;

    uint8_t nextTicker;
    SMAFrames::TxFrameSet frameSet = SMAFrames::encodeStatus(data, _ticker35E, nextTicker);
    _ticker35E = nextTicker;

    for (int i = 0; i < frameSet.count; i++)
    {
        const SMAFrames::CanFrame &f = frameSet.frames[i];
        sendFrame(f.id, f.dlc, f.data);
    }
}

void SMA_CAN::readMessages(DashboardData &dashboardOut)
{
    if (_driverDown)
        return;

    twai_message_t in_msg;
    int msgCount = 0;

    // Count first, so the 11th frame isn't dequeued and then dropped
    // unread once the batch limit is hit.
    while (msgCount < kRxBatchMax && twai_receive(&in_msg, 0) == ESP_OK)
    {
        msgCount++;

        SMAFrames::RxUpdate update;
        if (SMAFrames::decodeFrame(in_msg.identifier, in_msg.data, in_msg.data_length_code, update))
        {
            if (update.hasChargeMode)
                dashboardOut.smaChargeMode = update.chargeMode;
            if (update.hasGridPresent)
                dashboardOut.gridPresent = update.gridPresent;
        }
    }
}