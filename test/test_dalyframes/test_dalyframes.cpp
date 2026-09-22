// Native unit tests for the pure Daly frame parsers (#30): `pio test -e native`.
// Includes the real include/DalyFrames.h - no mirrored copy to keep in sync.
// Frames are hand-built from the documented Daly UART protocol: 0xA5 start,
// address 0x40, command byte, length 0x08, 8 data bytes, checksum = low
// byte of the sum of the first 12 bytes.

#include <unity.h>
#include <cstring>
#include "DalyFrames.h"

using DalyFrames::DalyAlarmStatus;
using DalyFrames::DalyBasicInfo;
using DalyFrames::DalyMosfetStatus;

// Builds a well-formed 13-byte frame (correct checksum) for the given
// command and 8-byte payload.
static void buildFrame(uint8_t cmd, const uint8_t data[8], uint8_t frame[13])
{
    frame[0] = 0xA5;
    frame[1] = 0x40;
    frame[2] = cmd;
    frame[3] = 0x08;
    for (int i = 0; i < 8; i++)
        frame[4 + i] = data[i];

    uint8_t checksum = 0;
    for (int i = 0; i < 12; i++)
        checksum += frame[i];
    frame[12] = checksum;
}

void setUp(void) {}
void tearDown(void) {}

// --- checksumOk ---

void test_checksum_ok(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);
    TEST_ASSERT_TRUE(DalyFrames::checksumOk(frame));
}

void test_checksum_bad(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);
    frame[12] += 1; // corrupt the checksum byte
    TEST_ASSERT_FALSE(DalyFrames::checksumOk(frame));
}

// --- parseBasicInfo (cmd 0x90) ---
// packVoltage = ((data[0]<<8)|data[1]) / 10.0  -> raw 552 = 0x0228 -> 55.2V
// packCurrent = (((data[4]<<8)|data[5]) - 30000) / 10.0 -> raw 30050 -> 5.0A
// packSOC     = ((data[6]<<8)|data[7]) / 10.0  -> raw 855 = 0x0357 -> 85.5%

void test_parse_basic_info(void)
{
    // 552 = 0x0228, 30050 = 0x7562, 855 = 0x0357
    uint8_t data[8] = {0x02, 0x28, 0x00, 0x00, 0x75, 0x62, 0x03, 0x57};
    DalyBasicInfo info;
    TEST_ASSERT_TRUE(DalyFrames::parseBasicInfo(data, info));
    TEST_ASSERT_EQUAL_FLOAT(55.2f, info.packVoltage);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, info.packCurrent);
    TEST_ASSERT_EQUAL_FLOAT(85.5f, info.packSOC);
}

void test_parse_basic_info_negative_current(void)
{
    // currentOffset 29500 = 0x733C -> (29500-30000)/10.0 = -50.0A (discharge)
    uint8_t data[8] = {0x00, 0x00, 0x00, 0x00, 0x73, 0x3C, 0x00, 0x00};
    DalyBasicInfo info;
    TEST_ASSERT_TRUE(DalyFrames::parseBasicInfo(data, info));
    TEST_ASSERT_EQUAL_FLOAT(-50.0f, info.packCurrent);
}

// --- parseCellFrame (cmd 0x95) ---
// data[0] = 1-based frame number; data[1..2]/[3..4]/[5..6] = 3 cell mV,
// big-endian; data[7] unused.

void test_parse_cell_frame(void)
{
    // frame 2, cells 3350 (0x0D16), 3360 (0x0D20), 3370 (0x0D2A) mV
    uint8_t data[8] = {2, 0x0D, 0x16, 0x0D, 0x20, 0x0D, 0x2A, 0x00};
    uint8_t frameNo;
    uint16_t mv[3];
    TEST_ASSERT_TRUE(DalyFrames::parseCellFrame(data, frameNo, mv));
    TEST_ASSERT_EQUAL_UINT8(2, frameNo);
    TEST_ASSERT_EQUAL_UINT16(3350, mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3360, mv[1]);
    TEST_ASSERT_EQUAL_UINT16(3370, mv[2]);
}

// --- parseMosfetStatus (cmd 0x93) ---

void test_parse_mosfet_status_accepted(void)
{
    // [0] charge/discharge status (1=charge), [1] charge MOS on,
    // [2] discharge MOS off, [3..7] life cycle/capacity - not decoded.
    uint8_t data[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    DalyMosfetStatus status;
    TEST_ASSERT_TRUE(DalyFrames::parseMosfetStatus(data, status));
    TEST_ASSERT_TRUE(status.chargeMosOn);
    TEST_ASSERT_FALSE(status.dischargeMosOn);
}

void test_parse_mosfet_status_rejected_out_of_range(void)
{
    // data[1] = 2 is neither the documented 0 (off) nor 1 (on) -> reject
    // the frame rather than guess.
    uint8_t data[8] = {1, 2, 0, 0, 0, 0, 0, 0};
    DalyMosfetStatus status;
    TEST_ASSERT_FALSE(DalyFrames::parseMosfetStatus(data, status));
}

void test_parse_mosfet_status_rejected_discharge_byte_out_of_range(void)
{
    uint8_t data[8] = {1, 0, 5, 0, 0, 0, 0, 0};
    DalyMosfetStatus status;
    TEST_ASSERT_FALSE(DalyFrames::parseMosfetStatus(data, status));
}

// --- parseAlarmStatus (cmd 0x98) ---

void test_parse_alarm_status_all_zero(void)
{
    uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    DalyAlarmStatus status;
    TEST_ASSERT_TRUE(DalyFrames::parseAlarmStatus(data, status));
    TEST_ASSERT_FALSE(status.anyProtectionActive);
    TEST_ASSERT_FALSE(status.cellOvervoltLevel1);
    TEST_ASSERT_FALSE(status.cellOvervoltLevel2);
    TEST_ASSERT_FALSE(status.packOvervoltLevel1);
    TEST_ASSERT_FALSE(status.packOvervoltLevel2);
    for (int i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_UINT8(0, status.rawBytes[i]);
}

void test_parse_alarm_status_named_byte0_bits(void)
{
    // bit0/1 = cell overvolt L1/L2, bit4/5 = pack overvolt L1/L2 (#25).
    uint8_t data[8] = {0x33, 0, 0, 0, 0, 0, 0, 0}; // 0011 0011: bits 0,1,4,5
    DalyAlarmStatus status;
    TEST_ASSERT_TRUE(DalyFrames::parseAlarmStatus(data, status));
    TEST_ASSERT_TRUE(status.cellOvervoltLevel1);
    TEST_ASSERT_TRUE(status.cellOvervoltLevel2);
    TEST_ASSERT_TRUE(status.packOvervoltLevel1);
    TEST_ASSERT_TRUE(status.packOvervoltLevel2);
    TEST_ASSERT_TRUE(status.anyProtectionActive);
}

void test_parse_alarm_status_fault_code_byte_not_scanned(void)
{
    // rawBytes[7] (the numeric fault code) is not a bitfield and must not
    // by itself flip anyProtectionActive - only bytes 0-6 are scanned.
    uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 42};
    DalyAlarmStatus status;
    TEST_ASSERT_TRUE(DalyFrames::parseAlarmStatus(data, status));
    TEST_ASSERT_FALSE(status.anyProtectionActive);
    TEST_ASSERT_EQUAL_UINT8(42, status.rawBytes[7]);
}

// Every named bit in kAlarmBitNames() (#25), set one at a time, must show
// up in the matching rawBytes bit and flip anyProtectionActive (all named
// bits live in bytes 0-6).
void test_parse_alarm_status_every_named_bit_sets_rawbyte_and_any(void)
{
    auto &names = DalyFrames::kAlarmBitNames();
    for (int b = 0; b < 7; b++)
    {
        for (int bit = 0; bit < 8; bit++)
        {
            if (!names[b][bit])
                continue; // undefined bit for this byte - not part of the table

            uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            data[b] = (uint8_t)(1 << bit);

            DalyAlarmStatus status;
            TEST_ASSERT_TRUE(DalyFrames::parseAlarmStatus(data, status));
            TEST_ASSERT_TRUE_MESSAGE((status.rawBytes[b] & (1 << bit)) != 0, names[b][bit]);
            TEST_ASSERT_TRUE_MESSAGE(status.anyProtectionActive, names[b][bit]);
        }
    }
}

// --- cellVoltagesPlausible ---

void test_cell_voltages_plausible_all_good(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    int badIndex = -99;
    TEST_ASSERT_TRUE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(-1, badIndex);
}

void test_cell_voltages_plausible_low_cell_fails_with_index(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[5] = 1.4f; // below kCellMinPlausibleMv (1.5V)
    int badIndex = -99;
    TEST_ASSERT_FALSE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(5, badIndex);
}

void test_cell_voltages_plausible_high_cell_fails_with_index(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[11] = 4.6f; // above kCellMaxPlausibleMv (4.5V)
    int badIndex = -99;
    TEST_ASSERT_FALSE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(11, badIndex);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_checksum_ok);
    RUN_TEST(test_checksum_bad);
    RUN_TEST(test_parse_basic_info);
    RUN_TEST(test_parse_basic_info_negative_current);
    RUN_TEST(test_parse_cell_frame);
    RUN_TEST(test_parse_mosfet_status_accepted);
    RUN_TEST(test_parse_mosfet_status_rejected_out_of_range);
    RUN_TEST(test_parse_mosfet_status_rejected_discharge_byte_out_of_range);
    RUN_TEST(test_parse_alarm_status_all_zero);
    RUN_TEST(test_parse_alarm_status_named_byte0_bits);
    RUN_TEST(test_parse_alarm_status_fault_code_byte_not_scanned);
    RUN_TEST(test_parse_alarm_status_every_named_bit_sets_rawbyte_and_any);
    RUN_TEST(test_cell_voltages_plausible_all_good);
    RUN_TEST(test_cell_voltages_plausible_low_cell_fails_with_index);
    RUN_TEST(test_cell_voltages_plausible_high_cell_fails_with_index);
    return UNITY_END();
}
