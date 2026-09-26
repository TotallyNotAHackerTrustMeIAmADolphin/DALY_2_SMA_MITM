// Native unit tests for the pure SMA/Victron CAN frame encode/decode and
// bus-off retry logic (#45): `pio test -e native`. Includes the real
// include/SMAFrames.h - no mirrored copy to keep in sync.

#include <unity.h>
#include "SMAFrames.h"

using SMAFrames::CanFrame;
using SMAFrames::RxUpdate;
using SMAFrames::SMATxData;
using SMAFrames::TxFrameSet;

void setUp(void) {}
void tearDown(void) {}

// Finds the frame with the given CAN id in a TxFrameSet, or nullptr if
// encodeStatus() didn't emit one this call.
static const CanFrame *findFrame(const TxFrameSet &set, uint32_t id)
{
    for (int i = 0; i < set.count; i++)
        if (set.frames[i].id == id)
            return &set.frames[i];
    return nullptr;
}

// --- encodeStatus (sendStatus's pre-#45 byte layout) ---

static void test_encode_status_normal_operation(void)
{
    // packVoltage 55.2V -> v_out = round(5520.0) = 5520 = 0x1590
    // packCurrent 5.0A  -> i_out = round(50.0)   = 50   = 0x0032
    // packTemp    25    -> 0x0019
    // packSOC     85.5% -> outSOC = round(85.5) = 86 = 0x0056 (not maintenance)
    // ccl 300 = 0x012C, dcl 250 = 0x00FA, cvl 5750 = 0x1676
    SMATxData data{};
    data.packVoltage = 55.2f;
    data.packCurrent = 5.0f;
    data.packTemp = 25;
    data.packSOC = 85.5f;
    data.ccl = 300;
    data.dcl = 250;
    data.cvl = 5750;
    data.dvl = 490; // 49.0 V (0.1 V units) -> bytes 6-7 (#63)
    data.maintenanceActive = false;
    data.isResetting = false;

    uint8_t nextTicker;
    TxFrameSet frameSet = SMAFrames::encodeStatus(data, /*tickerIn=*/0, nextTicker);

    TEST_ASSERT_EQUAL_UINT8(1, nextTicker);
    TEST_ASSERT_EQUAL_INT(4, frameSet.count); // no 0x35E/0x35F heartbeat this call

    const CanFrame *f351 = findFrame(frameSet, 0x351);
    TEST_ASSERT_NOT_NULL(f351);
    TEST_ASSERT_EQUAL_UINT8(8, f351->dlc);
    uint8_t expected351[8] = {0x76, 0x16, 0x2C, 0x01, 0xFA, 0x00, 0xEA, 0x01};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected351, f351->data, 8);

    const CanFrame *f355 = findFrame(frameSet, 0x355);
    TEST_ASSERT_NOT_NULL(f355);
    TEST_ASSERT_EQUAL_UINT8(4, f355->dlc);
    uint8_t expected355[8] = {0x56, 0x00, 100, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected355, f355->data, 8);

    const CanFrame *f356 = findFrame(frameSet, 0x356);
    TEST_ASSERT_NOT_NULL(f356);
    TEST_ASSERT_EQUAL_UINT8(6, f356->dlc);
    uint8_t expected356[8] = {0x90, 0x15, 0x32, 0x00, 0x19, 0x00, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected356, f356->data, 8);

    const CanFrame *f359 = findFrame(frameSet, 0x359);
    TEST_ASSERT_NOT_NULL(f359);
    TEST_ASSERT_EQUAL_UINT8(8, f359->dlc);
    uint8_t expected359[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected359, f359->data, 8);
}

static void test_encode_status_maintenance_sets_soc_sentinel_and_real_dvl(void)
{
    SMATxData data{};
    data.packVoltage = 50.0f;
    data.packCurrent = 0.0f;
    data.packTemp = 20;
    data.packSOC = 42.0f; // must be ignored in favor of the outSOC=2 sentinel
    data.ccl = 10;
    data.dcl = 10;
    data.cvl = 5500;
    data.dvl = 480;
    data.maintenanceActive = true;
    data.isResetting = false;

    uint8_t nextTicker;
    TxFrameSet frameSet = SMAFrames::encodeStatus(data, 0, nextTicker);

    const CanFrame *f355 = findFrame(frameSet, 0x355);
    TEST_ASSERT_NOT_NULL(f355);
    TEST_ASSERT_EQUAL_UINT8(2, f355->data[0]); // outSOC=2 maintenance sentinel
    TEST_ASSERT_EQUAL_UINT8(0, f355->data[1]);

    const CanFrame *f351 = findFrame(frameSet, 0x351);
    TEST_ASSERT_NOT_NULL(f351);
    // Maintenance sends the real DVL like normal operation (#63), not the
    // old 0x70 status byte.
    TEST_ASSERT_EQUAL_UINT8(0xE0, f351->data[6]); // 480 = 0x01E0 (48.0 V)
    TEST_ASSERT_EQUAL_UINT8(0x01, f351->data[7]);

    const CanFrame *f359 = findFrame(frameSet, 0x359);
    TEST_ASSERT_NOT_NULL(f359);
    TEST_ASSERT_EQUAL_UINT8(0x10, f359->data[0]); // maintenance bit set
}

static void test_encode_status_resetting_sends_zero_dvl(void)
{
    // The cluster reset sends DVL 0x0000 for its hold, whatever the real
    // DVL and maintenance state (#63) - exactly the bytes it sent before
    // DVL was encoded, since the reset relies on them.
    SMATxData data{};
    data.dvl = 480;
    data.maintenanceActive = true;
    data.isResetting = true;
    data.packSOC = 50.0f;

    uint8_t nextTicker;
    TxFrameSet frameSet = SMAFrames::encodeStatus(data, 0, nextTicker);

    const CanFrame *f351 = findFrame(frameSet, 0x351);
    TEST_ASSERT_NOT_NULL(f351);
    TEST_ASSERT_EQUAL_UINT8(0x00, f351->data[6]);
    TEST_ASSERT_EQUAL_UINT8(0x00, f351->data[7]);
}

static void test_encode_status_negative_current(void)
{
    // packCurrent -12.3A -> i_out = round(-123.0) = -123 = 0xFF85 (int16_t)
    SMATxData data{};
    data.packCurrent = -12.3f;

    uint8_t nextTicker;
    TxFrameSet frameSet = SMAFrames::encodeStatus(data, 0, nextTicker);

    const CanFrame *f356 = findFrame(frameSet, 0x356);
    TEST_ASSERT_NOT_NULL(f356);
    TEST_ASSERT_EQUAL_UINT8(0x85, f356->data[2]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, f356->data[3]);
}

static void test_encode_status_ticker_rollover_emits_heartbeat_frames(void)
{
    // tickerIn=10 -> tickerIn+1=11 > 10 -> resets to 0 and adds the
    // 0x35E ("SMA" ascii id) / 0x35F (manufacturer data) heartbeat pair.
    SMATxData data{};

    uint8_t nextTicker;
    TxFrameSet frameSet = SMAFrames::encodeStatus(data, /*tickerIn=*/10, nextTicker);

    TEST_ASSERT_EQUAL_UINT8(0, nextTicker);
    TEST_ASSERT_EQUAL_INT(6, frameSet.count);

    const CanFrame *f35E = findFrame(frameSet, 0x35E);
    TEST_ASSERT_NOT_NULL(f35E);
    TEST_ASSERT_EQUAL_UINT8(8, f35E->dlc);
    uint8_t expected35E[8] = {'S', 0, 'M', 0, 'A', 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected35E, f35E->data, 8);

    const CanFrame *f35F = findFrame(frameSet, 0x35F);
    TEST_ASSERT_NOT_NULL(f35F);
    TEST_ASSERT_EQUAL_UINT8(8, f35F->dlc);
    uint8_t expected35F[8] = {3, 0, 0, 0, 0x48, 0x03, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected35F, f35F->data, 8);
}

static void test_encode_status_ticker_not_yet_due_no_heartbeat(void)
{
    SMATxData data{};
    uint8_t nextTicker;
    TxFrameSet frameSet = SMAFrames::encodeStatus(data, /*tickerIn=*/9, nextTicker);

    TEST_ASSERT_EQUAL_UINT8(10, nextTicker);
    TEST_ASSERT_EQUAL_INT(4, frameSet.count);
    TEST_ASSERT_NULL(findFrame(frameSet, 0x35E));
    TEST_ASSERT_NULL(findFrame(frameSet, 0x35F));
}

// --- decodeFrame (readMessages's 0x305 mode / 0x300 grid decoding) ---

static void test_decode_0x305_mode_bulk(void)
{
    uint8_t data[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x305, data, 8, update));
    TEST_ASSERT_TRUE(update.hasChargeMode);
    TEST_ASSERT_EQUAL_STRING("Bulk", update.chargeMode);
    TEST_ASSERT_FALSE(update.hasGridPresent);
}

static void test_decode_0x305_mode_absorption(void)
{
    uint8_t data[8] = {2, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x305, data, 8, update));
    TEST_ASSERT_EQUAL_STRING("Absorption", update.chargeMode);
}

static void test_decode_0x305_mode_float(void)
{
    uint8_t data[8] = {3, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x305, data, 8, update));
    TEST_ASSERT_EQUAL_STRING("Float", update.chargeMode);
}

static void test_decode_0x305_mode_equalize(void)
{
    uint8_t data[8] = {4, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x305, data, 8, update));
    TEST_ASSERT_EQUAL_STRING("Equalize", update.chargeMode);
}

static void test_decode_0x305_mode_unrecognized_byte_is_unknown(void)
{
    // #104: anything but 1-4 used to fall through to "Equalize", so a 0 or
    // garbage byte showed as an equalize charge on the dashboard.
    const uint8_t modes[] = {0, 5, 99, 0xFF};
    for (uint8_t m : modes)
    {
        uint8_t data[8] = {m, 0, 0, 0, 0, 0, 0, 0};
        RxUpdate update;
        TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x305, data, 8, update));
        TEST_ASSERT_EQUAL_STRING("Unknown", update.chargeMode);
    }
}

static void test_decode_0x305_zero_dlc_ignored(void)
{
    uint8_t data[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_FALSE(SMAFrames::decodeFrame(0x305, data, 0, update));
    TEST_ASSERT_FALSE(update.hasChargeMode);
}

static void test_decode_0x300_grid_present_true(void)
{
    uint8_t data[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x300, data, 8, update));
    TEST_ASSERT_TRUE(update.hasGridPresent);
    TEST_ASSERT_TRUE(update.gridPresent);
    TEST_ASSERT_FALSE(update.hasChargeMode);
}

static void test_decode_0x300_grid_present_false(void)
{
    uint8_t data[8] = {0x00, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x300, data, 8, update));
    TEST_ASSERT_TRUE(update.hasGridPresent);
    TEST_ASSERT_FALSE(update.gridPresent);
}

static void test_decode_0x300_only_bit0_matters(void)
{
    // Other bits set, bit0 clear -> still reads as grid absent.
    uint8_t data[8] = {0xFE, 0, 0, 0, 0, 0, 0, 0};
    RxUpdate update;
    TEST_ASSERT_TRUE(SMAFrames::decodeFrame(0x300, data, 8, update));
    TEST_ASSERT_FALSE(update.gridPresent);
}

static void test_decode_unknown_id_not_decoded(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    RxUpdate update;
    TEST_ASSERT_FALSE(SMAFrames::decodeFrame(0x123, data, 8, update));
    TEST_ASSERT_FALSE(update.hasChargeMode);
    TEST_ASSERT_FALSE(update.hasGridPresent);
}

// --- shouldRetryBusRecovery (checkBusHealth's _wasBusOff/_recoveryTimer) ---

static void test_retry_not_due_before_one_second(void)
{
    TEST_ASSERT_FALSE(SMAFrames::shouldRetryBusRecovery(500, true, 0));
}

static void test_retry_due_after_one_second(void)
{
    TEST_ASSERT_TRUE(SMAFrames::shouldRetryBusRecovery(1001, true, 0));
}

static void test_retry_boundary_exactly_one_second_not_yet_due(void)
{
    // Pre-#45 uses a strict `>`, so exactly 1000ms elapsed is not yet due -
    // preserved exactly, not rounded to >=.
    TEST_ASSERT_FALSE(SMAFrames::shouldRetryBusRecovery(1000, true, 0));
}

static void test_retry_never_due_when_bus_not_off(void)
{
    TEST_ASSERT_FALSE(SMAFrames::shouldRetryBusRecovery(5000, false, 0));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_status_normal_operation);
    RUN_TEST(test_encode_status_maintenance_sets_soc_sentinel_and_real_dvl);
    RUN_TEST(test_encode_status_resetting_sends_zero_dvl);
    RUN_TEST(test_encode_status_negative_current);
    RUN_TEST(test_encode_status_ticker_rollover_emits_heartbeat_frames);
    RUN_TEST(test_encode_status_ticker_not_yet_due_no_heartbeat);
    RUN_TEST(test_decode_0x305_mode_bulk);
    RUN_TEST(test_decode_0x305_mode_absorption);
    RUN_TEST(test_decode_0x305_mode_float);
    RUN_TEST(test_decode_0x305_mode_equalize);
    RUN_TEST(test_decode_0x305_mode_unrecognized_byte_is_unknown);
    RUN_TEST(test_decode_0x305_zero_dlc_ignored);
    RUN_TEST(test_decode_0x300_grid_present_true);
    RUN_TEST(test_decode_0x300_grid_present_false);
    RUN_TEST(test_decode_0x300_only_bit0_matters);
    RUN_TEST(test_decode_unknown_id_not_decoded);
    RUN_TEST(test_retry_not_due_before_one_second);
    RUN_TEST(test_retry_due_after_one_second);
    RUN_TEST(test_retry_boundary_exactly_one_second_not_yet_due);
    RUN_TEST(test_retry_never_due_when_bus_not_off);
    return UNITY_END();
}
