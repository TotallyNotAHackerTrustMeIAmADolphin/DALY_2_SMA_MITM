// Native unit tests for the pure Daly frame parsers (#30): `pio test -e native`.
// Includes the real include/DalyFrames.h - no mirrored copy to keep in sync.
// Frames are hand-built from the documented Daly UART protocol: 0xA5 start,
// address 0x40, command byte, length 0x08, 8 data bytes, checksum = low
// byte of the sum of the first 12 bytes.

#include <unity.h>
#include <cstring>
#include <cmath>
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

static void test_checksum_ok(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);
    TEST_ASSERT_TRUE(DalyFrames::checksumOk(frame));
}

static void test_checksum_bad(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);
    frame[12] += 1; // corrupt the checksum byte
    TEST_ASSERT_FALSE(DalyFrames::checksumOk(frame));
}

// --- buildRequest / checksum (#103) ---
// checksum = low byte of the sum of A5 40 <cmd> 08 00 00 00 00 00 00 00 00.

static void test_build_request_basic_info(void)
{
    // 0xA5+0x40+0x90+0x08 = 0x16D -> low byte 0x7D
    uint8_t expected[13] = {0xA5, 0x40, 0x90, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0x7D};
    uint8_t frame[13];
    DalyFrames::buildRequest(DalyFrames::BasicInfo, frame);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 13);
    TEST_ASSERT_TRUE(DalyFrames::checksumOk(frame));
}

static void test_build_request_mosfet_status(void)
{
    // 0xA5+0x40+0x93+0x08 = 0x170 -> low byte 0x80
    uint8_t expected[13] = {0xA5, 0x40, 0x93, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0x80};
    uint8_t frame[13];
    DalyFrames::buildRequest(DalyFrames::MosfetStatus, frame);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 13);
    TEST_ASSERT_TRUE(DalyFrames::checksumOk(frame));
}

static void test_build_request_cell_voltages(void)
{
    // 0xA5+0x40+0x95+0x08 = 0x172 -> low byte 0x82
    uint8_t expected[13] = {0xA5, 0x40, 0x95, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0x82};
    uint8_t frame[13];
    DalyFrames::buildRequest(DalyFrames::CellVoltages, frame);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 13);
    TEST_ASSERT_TRUE(DalyFrames::checksumOk(frame));
}

static void test_build_request_alarm_status(void)
{
    // 0xA5+0x40+0x98+0x08 = 0x175 -> low byte 0x85
    uint8_t expected[13] = {0xA5, 0x40, 0x98, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0x85};
    uint8_t frame[13];
    DalyFrames::buildRequest(DalyFrames::AlarmStatus, frame);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 13);
    TEST_ASSERT_TRUE(DalyFrames::checksumOk(frame));
}

// --- parseBasicInfo (cmd 0x90) ---
// packVoltage = ((data[0]<<8)|data[1]) / 10.0  -> raw 552 = 0x0228 -> 55.2V
// packCurrent = (((data[4]<<8)|data[5]) - 30000) / 10.0 -> raw 30050 -> 5.0A
// packSOC     = ((data[6]<<8)|data[7]) / 10.0  -> raw 855 = 0x0357 -> 85.5%

static void test_parse_basic_info(void)
{
    // 552 = 0x0228, 30050 = 0x7562, 855 = 0x0357
    uint8_t data[8] = {0x02, 0x28, 0x00, 0x00, 0x75, 0x62, 0x03, 0x57};
    DalyBasicInfo info;
    TEST_ASSERT_TRUE(DalyFrames::parseBasicInfo(data, info));
    TEST_ASSERT_EQUAL_FLOAT(55.2f, info.packVoltage);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, info.packCurrent);
    TEST_ASSERT_EQUAL_FLOAT(85.5f, info.packSOC);
}

static void test_parse_basic_info_negative_current(void)
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

static void test_parse_cell_frame(void)
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

static void test_parse_mosfet_status_accepted(void)
{
    // [0] charge/discharge status (1=charge), [1] charge MOS on,
    // [2] discharge MOS off, [3..7] life cycle/capacity - not decoded.
    uint8_t data[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    DalyMosfetStatus status;
    TEST_ASSERT_TRUE(DalyFrames::parseMosfetStatus(data, status));
    TEST_ASSERT_TRUE(status.chargeMosOn);
    TEST_ASSERT_FALSE(status.dischargeMosOn);
}

static void test_parse_mosfet_status_rejected_out_of_range(void)
{
    // data[1] = 2 is neither the documented 0 (off) nor 1 (on) -> reject
    // the frame rather than guess.
    uint8_t data[8] = {1, 2, 0, 0, 0, 0, 0, 0};
    DalyMosfetStatus status;
    TEST_ASSERT_FALSE(DalyFrames::parseMosfetStatus(data, status));
}

static void test_parse_mosfet_status_rejected_discharge_byte_out_of_range(void)
{
    uint8_t data[8] = {1, 0, 5, 0, 0, 0, 0, 0};
    DalyMosfetStatus status;
    TEST_ASSERT_FALSE(DalyFrames::parseMosfetStatus(data, status));
}

// --- parseAlarmStatus (cmd 0x98) ---

static void test_parse_alarm_status_all_zero(void)
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

static void test_parse_alarm_status_named_byte0_bits(void)
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

static void test_parse_alarm_status_fault_code_byte_not_scanned(void)
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
static void test_parse_alarm_status_every_named_bit_sets_rawbyte_and_any(void)
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

static void test_cell_voltages_plausible_all_good(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    int badIndex = -99;
    TEST_ASSERT_TRUE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(-1, badIndex);
}

static void test_cell_voltages_plausible_low_cell_fails_with_index(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[5] = 1.4f; // below kCellMinPlausibleMv (1.5V)
    int badIndex = -99;
    TEST_ASSERT_FALSE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(5, badIndex);
}

static void test_cell_voltages_plausible_high_cell_fails_with_index(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[11] = 4.6f; // above kCellMaxPlausibleMv (4.5V)
    int badIndex = -99;
    TEST_ASSERT_FALSE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(11, badIndex);
}

static void test_cell_voltages_plausible_nan_rejected(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[3] = NAN;
    int badIndex = -99;
    TEST_ASSERT_FALSE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(3, badIndex);
}

static void test_cell_voltages_plausible_negative_rejected(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[7] = -1.0f;
    int badIndex = -99;
    TEST_ASSERT_FALSE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(7, badIndex);
}

static void test_cell_voltages_plausible_huge_rejected(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[9] = 70.0f;
    int badIndex = -99;
    TEST_ASSERT_FALSE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(9, badIndex);
}

static void test_cell_voltages_plausible_exact_bounds_accepted(void)
{
    float cells[16];
    for (int i = 0; i < 16; i++)
        cells[i] = 3.30f;
    cells[0] = 1.5f;
    cells[1] = 4.5f;
    int badIndex = -99;
    TEST_ASSERT_TRUE(DalyFrames::cellVoltagesPlausible(cells, 16, badIndex));
    TEST_ASSERT_EQUAL_INT(-1, badIndex);
}

// --- FrameAssembler (#101) ---

// Feeds every byte of `bytes` into `fa` one at a time; returns the index
// (into `bytes`) of the byte that completed a frame (writing it into
// frameOut), or -1 if no frame completed.
static int feedAll(DalyFrames::FrameAssembler &fa, const uint8_t *bytes, int n, uint8_t frameOut[13])
{
    for (int i = 0; i < n; i++)
    {
        if (fa.feed(bytes[i], frameOut))
            return i;
    }
    return -1;
}

static void test_frame_assembler_clean_frame(void)
{
    uint8_t data[8] = {0x02, 0x28, 0, 0, 0x75, 0x62, 0x03, 0x57};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);

    DalyFrames::FrameAssembler fa;
    uint8_t out[13];
    int completedAt = feedAll(fa, frame, 13, out);
    TEST_ASSERT_EQUAL_INT(12, completedAt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, out, 13);
}

static void test_frame_assembler_stray_start_byte_before_real_frame(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);

    // A lone 0xA5 (no frame follows it) precedes the real frame. The old
    // byte-sync loops would treat it as frame start and, once the real
    // frame's first 12 bytes filled the window, fail checksum and throw
    // the whole window away - never re-trying at the real frame's actual
    // start. feed() must still find the real frame.
    uint8_t stream[14];
    stream[0] = 0xA5;
    for (int i = 0; i < 13; i++)
        stream[1 + i] = frame[i];

    DalyFrames::FrameAssembler fa;
    uint8_t out[13];
    int completedAt = feedAll(fa, stream, 14, out);
    TEST_ASSERT_EQUAL_INT(13, completedAt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, out, 13);
}

static void test_frame_assembler_bad_checksum_then_good_frame(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t badFrame[13];
    buildFrame(0x90, data, badFrame);
    badFrame[12] += 1; // corrupt checksum, no 0xA5 hidden inside

    uint8_t goodFrame[13];
    buildFrame(0x93, data, goodFrame);

    uint8_t stream[26];
    for (int i = 0; i < 13; i++)
        stream[i] = badFrame[i];
    for (int i = 0; i < 13; i++)
        stream[13 + i] = goodFrame[i];

    DalyFrames::FrameAssembler fa;
    uint8_t out[13];
    int completedAt = feedAll(fa, stream, 26, out);
    TEST_ASSERT_EQUAL_INT(25, completedAt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(goodFrame, out, 13);
}

static void test_frame_assembler_back_to_back_frames(void)
{
    uint8_t data1[8] = {1, 0x0D, 0x16, 0x0D, 0x20, 0x0D, 0x2A, 0};
    uint8_t frame1[13];
    buildFrame(0x95, data1, frame1);

    uint8_t data2[8] = {2, 0x0D, 0x30, 0x0D, 0x3A, 0x0D, 0x44, 0};
    uint8_t frame2[13];
    buildFrame(0x95, data2, frame2);

    uint8_t stream[26];
    for (int i = 0; i < 13; i++)
        stream[i] = frame1[i];
    for (int i = 0; i < 13; i++)
        stream[13 + i] = frame2[i];

    DalyFrames::FrameAssembler fa;
    uint8_t out[13];

    int firstAt = feedAll(fa, stream, 13, out);
    TEST_ASSERT_EQUAL_INT(12, firstAt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame1, out, 13);

    int secondAt = feedAll(fa, &stream[13], 13, out);
    TEST_ASSERT_EQUAL_INT(12, secondAt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame2, out, 13);
}

static void test_frame_assembler_false_start_mid_garbage_finds_real_frame(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);

    // A false-start 0xA5 sits a few bytes before the real frame, so the
    // bad 13-byte window that starts at the false 0xA5 contains the first
    // few bytes of the real frame. feed() must discard just the leading
    // byte, rescan, and land back on the real frame's own 0xA5.
    uint8_t stream[16];
    stream[0] = 0xA5; // false start
    stream[1] = 0x11;
    stream[2] = 0x22;
    for (int i = 0; i < 13; i++)
        stream[3 + i] = frame[i];

    DalyFrames::FrameAssembler fa;
    uint8_t out[13];
    int completedAt = feedAll(fa, stream, 16, out);
    TEST_ASSERT_EQUAL_INT(15, completedAt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, out, 13);
}

static void test_frame_assembler_checksum_failed_flag(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);
    frame[12] += 1;

    DalyFrames::FrameAssembler fa;
    uint8_t out[13];
    bool sawFailureFlag = false;
    for (int i = 0; i < 13; i++)
    {
        bool completed = fa.feed(frame[i], out);
        TEST_ASSERT_FALSE(completed);
        if (i == 12)
            sawFailureFlag = fa.checksumFailedOnLastFeed();
        else
            TEST_ASSERT_FALSE(fa.checksumFailedOnLastFeed());
    }
    TEST_ASSERT_TRUE(sawFailureFlag);
}

static void test_frame_assembler_reset_clears_partial_state(void)
{
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[13];
    buildFrame(0x90, data, frame);

    DalyFrames::FrameAssembler fa;
    uint8_t out[13];
    // Feed only the first 5 bytes, then reset - the partial frame must not
    // bleed into the next read.
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_FALSE(fa.feed(frame[i], out));

    fa.reset();

    int completedAt = feedAll(fa, frame, 13, out);
    TEST_ASSERT_EQUAL_INT(12, completedAt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, out, 13);
}

// --- CellFrameCollector (#102) ---

// Builds an 8-byte cell-voltage payload: frame number + 3 big-endian mV
// values (matches parseCellFrame()'s layout).
static void buildCellPayload(uint8_t frameNo, uint16_t mv0, uint16_t mv1, uint16_t mv2, uint8_t out[8])
{
    out[0] = frameNo;
    out[1] = (uint8_t)(mv0 >> 8);
    out[2] = (uint8_t)mv0;
    out[3] = (uint8_t)(mv1 >> 8);
    out[4] = (uint8_t)mv1;
    out[5] = (uint8_t)(mv2 >> 8);
    out[6] = (uint8_t)mv2;
    out[7] = 0;
}

static void test_collector_16_cells_exact_mv_values(void)
{
    DalyFrames::CellFrameCollector c;
    c.reset(16);

    // frame 1: cells 1-3, frame 2: cells 4-6, ..., frame 6: cell 16 only
    // (partial - its other two slots are padding and must be ignored).
    uint16_t expected[16] = {3300, 3301, 3302, 3303, 3304, 3305, 3306, 3307,
                              3308, 3309, 3310, 3311, 3312, 3313, 3314, 3315};
    uint8_t payload[8];
    for (int frame = 1; frame <= 6; frame++)
    {
        int base = (frame - 1) * 3;
        uint16_t v0 = expected[base];
        uint16_t v1 = base + 1 < 16 ? expected[base + 1] : 0xFFFF; // padding for the partial frame
        uint16_t v2 = base + 2 < 16 ? expected[base + 2] : 0xFFFF;
        buildCellPayload((uint8_t)frame, v0, v1, v2, payload);
        TEST_ASSERT_TRUE(c.accept(payload));
        TEST_ASSERT_EQUAL_INT(frame == 6, c.complete());
    }

    TEST_ASSERT_TRUE(c.complete());
    const uint16_t *mv = c.mv();
    for (int i = 0; i < 16; i++)
        TEST_ASSERT_EQUAL_UINT16(expected[i], mv[i]);
}

static void test_collector_partial_last_frame_ignores_padding(void)
{
    // 16 cells -> frame 6 carries only cell 16; its 2nd/3rd mv slots
    // (cellIdx 16, 17) must never be written anywhere reachable.
    DalyFrames::CellFrameCollector c;
    c.reset(16);
    uint8_t payload[8];
    for (int frame = 1; frame <= 5; frame++)
    {
        buildCellPayload((uint8_t)frame, 3300, 3300, 3300, payload);
        c.accept(payload);
    }
    buildCellPayload(6, 3399, 0xBEEF, 0xBEEF, payload); // padding values that must be dropped
    TEST_ASSERT_TRUE(c.accept(payload));
    TEST_ASSERT_TRUE(c.complete());
    TEST_ASSERT_EQUAL_UINT16(3399, c.mv()[15]);
}

static void test_collector_duplicate_frame_ignored(void)
{
    DalyFrames::CellFrameCollector c;
    c.reset(6); // 2 frames
    uint8_t payload[8];

    buildCellPayload(1, 100, 200, 300, payload);
    TEST_ASSERT_TRUE(c.accept(payload));

    // Same frame again, different (bogus) values - must be ignored, not
    // recounted, and must not overwrite the already-stored values.
    buildCellPayload(1, 999, 999, 999, payload);
    TEST_ASSERT_FALSE(c.accept(payload));
    TEST_ASSERT_EQUAL_INT(1, c.framesReceived());
    TEST_ASSERT_EQUAL_UINT16(100, c.mv()[0]);

    buildCellPayload(2, 400, 500, 600, payload);
    TEST_ASSERT_TRUE(c.accept(payload));
    TEST_ASSERT_TRUE(c.complete());
}

static void test_collector_frame_number_zero_rejected(void)
{
    DalyFrames::CellFrameCollector c;
    c.reset(6);
    uint8_t payload[8];
    buildCellPayload(0, 100, 200, 300, payload);
    TEST_ASSERT_FALSE(c.accept(payload));
    TEST_ASSERT_EQUAL_INT(0, c.framesReceived());
    TEST_ASSERT_FALSE(c.complete());
}

static void test_collector_frame_number_beyond_needed_rejected(void)
{
    // 6 cells -> 2 frames needed; frame 3 is out of range.
    DalyFrames::CellFrameCollector c;
    c.reset(6);
    uint8_t payload[8];
    buildCellPayload(3, 100, 200, 300, payload);
    TEST_ASSERT_FALSE(c.accept(payload));
    TEST_ASSERT_EQUAL_INT(0, c.framesReceived());
}

static void test_collector_completes_only_when_all_frames_in(void)
{
    DalyFrames::CellFrameCollector c;
    c.reset(9); // 3 frames
    uint8_t payload[8];

    buildCellPayload(1, 100, 200, 300, payload);
    c.accept(payload);
    TEST_ASSERT_FALSE(c.complete());

    buildCellPayload(2, 400, 500, 600, payload);
    c.accept(payload);
    TEST_ASSERT_FALSE(c.complete());

    buildCellPayload(3, 700, 800, 900, payload);
    c.accept(payload);
    TEST_ASSERT_TRUE(c.complete());
}

static void test_collector_frames_out_of_order(void)
{
    DalyFrames::CellFrameCollector c;
    c.reset(9); // 3 frames
    uint8_t payload[8];

    buildCellPayload(3, 700, 800, 900, payload);
    TEST_ASSERT_TRUE(c.accept(payload));
    buildCellPayload(1, 100, 200, 300, payload);
    TEST_ASSERT_TRUE(c.accept(payload));
    buildCellPayload(2, 400, 500, 600, payload);
    TEST_ASSERT_TRUE(c.accept(payload));

    TEST_ASSERT_TRUE(c.complete());
    const uint16_t *mv = c.mv();
    TEST_ASSERT_EQUAL_UINT16(100, mv[0]);
    TEST_ASSERT_EQUAL_UINT16(500, mv[4]);
    TEST_ASSERT_EQUAL_UINT16(900, mv[8]);
}

static void test_collector_reset_between_reads(void)
{
    DalyFrames::CellFrameCollector c;
    c.reset(6);
    uint8_t payload[8];
    buildCellPayload(1, 100, 200, 300, payload);
    c.accept(payload);
    TEST_ASSERT_EQUAL_INT(1, c.framesReceived());

    // A fresh read must not see the previous read's frame as already
    // received, and its stale mv values must not leak forward either.
    c.reset(6);
    TEST_ASSERT_EQUAL_INT(0, c.framesReceived());
    TEST_ASSERT_FALSE(c.complete());
    TEST_ASSERT_EQUAL_UINT16(0, c.mv()[0]);

    buildCellPayload(1, 111, 222, 333, payload);
    TEST_ASSERT_TRUE(c.accept(payload));
    TEST_ASSERT_EQUAL_UINT16(111, c.mv()[0]);
}

static void test_collector_24_cells_mask_beyond_uint8(void)
{
    // 24 cells -> 8 frames. Frame 8's bit is 1<<8 = 256, which does not
    // fit a uint8_t mask (the old inline framesMask's type) - this is
    // exactly the latent truncation bug #102 called out. Prove the
    // uint32_t mask handles it correctly.
    DalyFrames::CellFrameCollector c;
    c.reset(24);
    uint8_t payload[8];
    for (int frame = 1; frame <= 8; frame++)
    {
        uint16_t base = (uint16_t)(3000 + frame * 10);
        buildCellPayload((uint8_t)frame, base, base, base, payload);
        TEST_ASSERT_TRUE(c.accept(payload));
    }
    TEST_ASSERT_TRUE(c.complete());
    TEST_ASSERT_EQUAL_INT(8, c.framesReceived());

    // Frame 8 covers cellIdx 21-23 (3 cells, 24 is a multiple of 3 - no
    // partial frame here); check it landed correctly despite the high bit.
    TEST_ASSERT_EQUAL_UINT16(3080, c.mv()[21]);
    TEST_ASSERT_EQUAL_UINT16(3080, c.mv()[23]);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_checksum_ok);
    RUN_TEST(test_checksum_bad);
    RUN_TEST(test_build_request_basic_info);
    RUN_TEST(test_build_request_mosfet_status);
    RUN_TEST(test_build_request_cell_voltages);
    RUN_TEST(test_build_request_alarm_status);
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
    RUN_TEST(test_cell_voltages_plausible_nan_rejected);
    RUN_TEST(test_cell_voltages_plausible_negative_rejected);
    RUN_TEST(test_cell_voltages_plausible_huge_rejected);
    RUN_TEST(test_cell_voltages_plausible_exact_bounds_accepted);
    RUN_TEST(test_frame_assembler_clean_frame);
    RUN_TEST(test_frame_assembler_stray_start_byte_before_real_frame);
    RUN_TEST(test_frame_assembler_bad_checksum_then_good_frame);
    RUN_TEST(test_frame_assembler_back_to_back_frames);
    RUN_TEST(test_frame_assembler_false_start_mid_garbage_finds_real_frame);
    RUN_TEST(test_frame_assembler_checksum_failed_flag);
    RUN_TEST(test_frame_assembler_reset_clears_partial_state);
    RUN_TEST(test_collector_16_cells_exact_mv_values);
    RUN_TEST(test_collector_partial_last_frame_ignores_padding);
    RUN_TEST(test_collector_duplicate_frame_ignored);
    RUN_TEST(test_collector_frame_number_zero_rejected);
    RUN_TEST(test_collector_frame_number_beyond_needed_rejected);
    RUN_TEST(test_collector_completes_only_when_all_frames_in);
    RUN_TEST(test_collector_frames_out_of_order);
    RUN_TEST(test_collector_reset_between_reads);
    RUN_TEST(test_collector_24_cells_mask_beyond_uint8);
    return UNITY_END();
}
