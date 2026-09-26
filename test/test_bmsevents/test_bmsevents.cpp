// Native unit tests for the bmsTask edge-triggered event logic (#44):
// `pio test -e native`. Includes the real include/BmsEvents.h - no mirrored
// copy to keep in sync. These tests pin down the exact semantics of the old
// inline bmsTask SOC-jump/MOSFET-edge/alarm-bit-diff code before it was
// extracted, so a future change to decide() that silently changes what gets
// logged fails here first. Mirrors test/test_statusframe's style.

#include <unity.h>
#include <cstdio>
#include <initializer_list>
#include "BmsEvents.h"
#include "DalyFrames.h"

using BmsEvents::decide;
using BmsEvents::Events;
using BmsEvents::State;
using DalyFrames::DalyAlarmStatus;
using DalyFrames::DalyBasicInfo;
using DalyFrames::DalyMosfetStatus;

void setUp(void) {}
void tearDown(void) {}

static DalyBasicInfo basicInfoWithSoc(float soc)
{
    DalyBasicInfo info;
    info.packVoltage = 55.0f;
    info.packCurrent = 0.0f;
    info.packSOC = soc;
    return info;
}

static DalyMosfetStatus mosfet(bool chargeOn, bool dischargeOn)
{
    DalyMosfetStatus m;
    m.chargeMosOn = chargeOn;
    m.dischargeMosOn = dischargeOn;
    return m;
}

// Builds an 8-byte 0x98 payload from up to 8 values (the rest zero) and
// runs it through the real parser, so a fixture can't drift from what
// bmsTask actually feeds decide().
static DalyAlarmStatus alarmFromBytes(std::initializer_list<uint8_t> bytes)
{
    uint8_t data[8] = {0};
    size_t i = 0;
    for (uint8_t b : bytes)
        data[i++] = b;
    DalyAlarmStatus a;
    DalyFrames::parseAlarmStatus(data, a);
    return a;
}

static DalyAlarmStatus zeroAlarm() { return alarmFromBytes({}); }

// --- SOC jump ---

struct SocJumpCase
{
    float from, to;
    bool jumped;
};

// The >10-point rule and the "jump to 100% from below 95%" special case,
// each exercised right at both edges (>10 vs ==10; lastSoc<95 vs ==95;
// soc>=99.9 vs just under it) - only a boundary shift can slip past both.
static const SocJumpCase kSocJumpCases[] = {
    {50.0f, 55.0f, false},  // +5: comfortably under the threshold
    {50.0f, 60.0f, false},  // +10.0 exactly: rule is strictly >, no jump
    {50.0f, 60.1f, true},   // just over +10
    {80.0f, 65.0f, true},   // -15: either direction
    {94.5f, 100.0f, true},  // 100%-recal: lastSoc<95, soc>=99.9
    {94.9f, 99.9f, true},   // ... right at both edges
    {95.0f, 99.9f, false},  // lastSoc==95, not <95: no special case
    {96.0f, 100.0f, false}, // lastSoc>=95, delta 4: no special case
    {90.0f, 99.89f, false}, // soc<99.9: no special case (delta 9.89)
};

static void test_soc_jump_table(void)
{
    for (const SocJumpCase &c : kSocJumpCases)
    {
        State st;
        DalyBasicInfo i1 = basicInfoWithSoc(c.from);
        decide(st, &i1, nullptr, nullptr);
        DalyBasicInfo i2 = basicInfoWithSoc(c.to);
        Events ev = decide(st, &i2, nullptr, nullptr);
        char msg[48];
        snprintf(msg, sizeof(msg), "%.2f -> %.2f", c.from, c.to);
        TEST_ASSERT_EQUAL_MESSAGE(c.jumped, ev.socJumped, msg);
        if (c.jumped)
        {
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(c.from, ev.socFrom, msg);
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(c.to, ev.socTo, msg);
        }
    }
}

static void test_soc_no_jump_on_first_ever_reading(void)
{
    // Baseline: the very first reading after boot must never be reported as
    // a jump, however far from 0 it is.
    State st;
    DalyBasicInfo info = basicInfoWithSoc(85.0f);
    Events ev = decide(st, &info, nullptr, nullptr);
    TEST_ASSERT_FALSE(ev.socJumped);
}

static void test_soc_jump_does_not_repeat_next_reading(void)
{
    State st;
    DalyBasicInfo i1 = basicInfoWithSoc(50.0f);
    decide(st, &i1, nullptr, nullptr);
    DalyBasicInfo i2 = basicInfoWithSoc(70.0f);
    Events ev2 = decide(st, &i2, nullptr, nullptr);
    TEST_ASSERT_TRUE(ev2.socJumped);

    DalyBasicInfo i3 = basicInfoWithSoc(71.0f); // small change from the new baseline
    Events ev3 = decide(st, &i3, nullptr, nullptr);
    TEST_ASSERT_FALSE(ev3.socJumped);
}

// --- MOSFET transitions ---

static void test_mosfet_no_event_on_first_ever_reading(void)
{
    State st;
    DalyMosfetStatus m = mosfet(true, true);
    Events ev = decide(st, nullptr, &m, nullptr);
    TEST_ASSERT_FALSE(ev.chargeMosChanged);
    TEST_ASSERT_FALSE(ev.dischargeMosChanged);
}

struct MosfetCase
{
    bool c1, d1, c2, d2; // baseline reading, then the reading that follows it
    bool chargeChanged, chargeOn, dischargeChanged, dischargeOn;
};

static const MosfetCase kMosfetCases[] = {
    {true, true, true, true, false, false, false, false},  // unchanged
    {true, true, false, true, true, false, false, false},  // charge on -> off
    {false, true, true, true, true, true, false, false},   // charge off -> on
    {true, true, false, false, true, false, true, false},  // both change at once
};

static void test_mosfet_transitions(void)
{
    for (const MosfetCase &c : kMosfetCases)
    {
        State st;
        DalyMosfetStatus m1 = mosfet(c.c1, c.d1);
        decide(st, nullptr, &m1, nullptr);
        DalyMosfetStatus m2 = mosfet(c.c2, c.d2);
        Events ev = decide(st, nullptr, &m2, nullptr);
        TEST_ASSERT_EQUAL(c.chargeChanged, ev.chargeMosChanged);
        if (c.chargeChanged)
            TEST_ASSERT_EQUAL(c.chargeOn, ev.chargeMosOn);
        TEST_ASSERT_EQUAL(c.dischargeChanged, ev.dischargeMosChanged);
        if (c.dischargeChanged)
            TEST_ASSERT_EQUAL(c.dischargeOn, ev.dischargeMosOn);
    }
}

static void test_mosfet_discharge_on_to_off_and_back(void)
{
    State st;
    DalyMosfetStatus m1 = mosfet(true, true);
    decide(st, nullptr, &m1, nullptr);

    DalyMosfetStatus m2 = mosfet(true, false);
    Events ev2 = decide(st, nullptr, &m2, nullptr);
    TEST_ASSERT_FALSE(ev2.chargeMosChanged);
    TEST_ASSERT_TRUE(ev2.dischargeMosChanged);
    TEST_ASSERT_FALSE(ev2.dischargeMosOn);

    DalyMosfetStatus m3 = mosfet(true, true);
    Events ev3 = decide(st, nullptr, &m3, nullptr);
    TEST_ASSERT_TRUE(ev3.dischargeMosChanged);
    TEST_ASSERT_TRUE(ev3.dischargeMosOn);
}

// --- Alarm bit diff ---

static void test_alarm_no_events_on_first_ever_reading(void)
{
    // Baseline: an all-nonzero first-ever alarm read must not be reported
    // as a burst of "SET" events (byte 0 alone here sets 8 bits).
    State st;
    DalyAlarmStatus a = alarmFromBytes({0xFF});
    Events ev = decide(st, nullptr, nullptr, &a);
    TEST_ASSERT_EQUAL_INT(0, ev.alarmBitCount);
}

static void test_alarm_no_events_when_unchanged(void)
{
    State st;
    DalyAlarmStatus a1 = zeroAlarm();
    decide(st, nullptr, nullptr, &a1);
    DalyAlarmStatus a2 = zeroAlarm();
    Events ev = decide(st, nullptr, nullptr, &a2);
    TEST_ASSERT_EQUAL_INT(0, ev.alarmBitCount);
    TEST_ASSERT_FALSE(ev.faultCodeChanged);
}

static void test_alarm_bit_set_decoded_to_name(void)
{
    // Byte 0 bit 0 = "Cell overvoltage Level 1" (kAlarmBitNames()[0][0]).
    State st;
    DalyAlarmStatus a1 = zeroAlarm();
    decide(st, nullptr, nullptr, &a1);

    DalyAlarmStatus a2 = alarmFromBytes({0x01});
    Events ev = decide(st, nullptr, nullptr, &a2);

    TEST_ASSERT_EQUAL_INT(1, ev.alarmBitCount);
    TEST_ASSERT_EQUAL_INT(0, ev.alarmBits[0].byteIndex);
    TEST_ASSERT_EQUAL_INT(0, ev.alarmBits[0].bitIndex);
    TEST_ASSERT_TRUE(ev.alarmBits[0].set);
    TEST_ASSERT_NOT_NULL(ev.alarmBits[0].name);
    TEST_ASSERT_EQUAL_STRING("Cell overvoltage Level 1", ev.alarmBits[0].name);
}

static void test_alarm_bit_cleared_decoded_to_name(void)
{
    State st;
    DalyAlarmStatus a1 = alarmFromBytes({0x01});
    decide(st, nullptr, nullptr, &a1);

    DalyAlarmStatus a2 = zeroAlarm(); // bit clears
    Events ev = decide(st, nullptr, nullptr, &a2);

    TEST_ASSERT_EQUAL_INT(1, ev.alarmBitCount);
    TEST_ASSERT_FALSE(ev.alarmBits[0].set);
    TEST_ASSERT_EQUAL_STRING("Cell overvoltage Level 1", ev.alarmBits[0].name);
}

static void test_alarm_undefined_bit_has_null_name(void)
{
    // Byte 3 bits 4-7 are undefined in kAlarmBitNames() - still reported,
    // with name == nullptr so the caller falls back to byte.bit logging.
    State st;
    DalyAlarmStatus a1 = zeroAlarm();
    decide(st, nullptr, nullptr, &a1);

    DalyAlarmStatus a2 = alarmFromBytes({0, 0, 0, 0x10}); // byte 3, bit 4
    Events ev = decide(st, nullptr, nullptr, &a2);

    TEST_ASSERT_EQUAL_INT(1, ev.alarmBitCount);
    TEST_ASSERT_EQUAL_INT(3, ev.alarmBits[0].byteIndex);
    TEST_ASSERT_EQUAL_INT(4, ev.alarmBits[0].bitIndex);
    TEST_ASSERT_TRUE(ev.alarmBits[0].set);
    TEST_ASSERT_NULL(ev.alarmBits[0].name);
}

static void test_alarm_multiple_bits_across_bytes_same_reading(void)
{
    State st;
    DalyAlarmStatus a1 = zeroAlarm();
    decide(st, nullptr, nullptr, &a1);

    // bit 0 -> "Cell overvoltage Level 1", byte 2 bit 2 -> "Discharge
    // overcurrent Level 1".
    DalyAlarmStatus a2 = alarmFromBytes({0x01, 0, 0x04});
    Events ev = decide(st, nullptr, nullptr, &a2);

    TEST_ASSERT_EQUAL_INT(2, ev.alarmBitCount);
    TEST_ASSERT_EQUAL_STRING("Cell overvoltage Level 1", ev.alarmBits[0].name);
    TEST_ASSERT_EQUAL_INT(2, ev.alarmBits[1].byteIndex);
    TEST_ASSERT_EQUAL_INT(2, ev.alarmBits[1].bitIndex);
    TEST_ASSERT_EQUAL_STRING("Discharge overcurrent Level 1", ev.alarmBits[1].name);
}

static void test_alarm_fault_code_change_reported_separately_from_bits(void)
{
    State st;
    DalyAlarmStatus a1 = zeroAlarm();
    decide(st, nullptr, nullptr, &a1);

    // Fault code (byte 7) changes, no protection bit changes.
    DalyAlarmStatus a2 = alarmFromBytes({0, 0, 0, 0, 0, 0, 0, 5});
    Events ev = decide(st, nullptr, nullptr, &a2);

    TEST_ASSERT_EQUAL_INT(0, ev.alarmBitCount);
    TEST_ASSERT_TRUE(ev.faultCodeChanged);
    TEST_ASSERT_EQUAL_UINT8(0, ev.faultCodeFrom);
    TEST_ASSERT_EQUAL_UINT8(5, ev.faultCodeTo);
}

static void test_alarm_events_do_not_repeat_next_unchanged_reading(void)
{
    State st;
    DalyAlarmStatus a1 = zeroAlarm();
    decide(st, nullptr, nullptr, &a1);

    DalyAlarmStatus a2 = alarmFromBytes({0x01, 0, 0, 0, 0, 0, 0, 5});
    decide(st, nullptr, nullptr, &a2); // sets the new baseline

    DalyAlarmStatus a3 = alarmFromBytes({0x01, 0, 0, 0, 0, 0, 0, 5});
    Events ev3 = decide(st, nullptr, nullptr, &a3);
    TEST_ASSERT_EQUAL_INT(0, ev3.alarmBitCount);
    TEST_ASSERT_FALSE(ev3.faultCodeChanged);
}

// --- Independence: a call with only one reading present must not disturb
// the other two states' baselines ---

static void test_readings_are_independent_of_each_other(void)
{
    State st;
    // Only a MOSFET reading this call.
    DalyMosfetStatus m1 = mosfet(true, true);
    decide(st, nullptr, &m1, nullptr);

    // An SOC-only call in between must not report a jump (no SOC baseline
    // yet) or disturb the MOSFET baseline just established.
    DalyBasicInfo info = basicInfoWithSoc(50.0f);
    Events ev = decide(st, &info, nullptr, nullptr);
    TEST_ASSERT_FALSE(ev.socJumped);

    // The real MOSFET transition below is still detected against m1, not
    // silently treated as a fresh baseline.
    DalyMosfetStatus m2 = mosfet(false, true);
    Events evMos = decide(st, nullptr, &m2, nullptr);
    TEST_ASSERT_TRUE(evMos.chargeMosChanged);
    TEST_ASSERT_FALSE(evMos.chargeMosOn);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_soc_jump_table);
    RUN_TEST(test_soc_no_jump_on_first_ever_reading);
    RUN_TEST(test_soc_jump_does_not_repeat_next_reading);

    RUN_TEST(test_mosfet_no_event_on_first_ever_reading);
    RUN_TEST(test_mosfet_transitions);
    RUN_TEST(test_mosfet_discharge_on_to_off_and_back);

    RUN_TEST(test_alarm_no_events_on_first_ever_reading);
    RUN_TEST(test_alarm_no_events_when_unchanged);
    RUN_TEST(test_alarm_bit_set_decoded_to_name);
    RUN_TEST(test_alarm_bit_cleared_decoded_to_name);
    RUN_TEST(test_alarm_undefined_bit_has_null_name);
    RUN_TEST(test_alarm_multiple_bits_across_bytes_same_reading);
    RUN_TEST(test_alarm_fault_code_change_reported_separately_from_bits);
    RUN_TEST(test_alarm_events_do_not_repeat_next_unchanged_reading);

    RUN_TEST(test_readings_are_independent_of_each_other);
    return UNITY_END();
}
