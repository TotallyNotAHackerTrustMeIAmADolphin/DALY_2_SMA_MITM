// Native unit tests for the bmsTask edge-triggered event logic (#44):
// `pio test -e native`. Includes the real include/BmsEvents.h - no mirrored
// copy to keep in sync. These tests pin down the exact semantics of the old
// inline bmsTask SOC-jump/MOSFET-edge/alarm-bit-diff code before it was
// extracted, so a future change to decide() that silently changes what gets
// logged fails here first. Mirrors test/test_statusframe's style.

#include <unity.h>
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

// All-zero alarm payload (no protection bits, fault code 0).
static DalyAlarmStatus zeroAlarm()
{
    DalyAlarmStatus a;
    for (int i = 0; i < 8; i++)
        a.rawBytes[i] = 0;
    a.cellOvervoltLevel1 = false;
    a.cellOvervoltLevel2 = false;
    a.packOvervoltLevel1 = false;
    a.packOvervoltLevel2 = false;
    a.anyProtectionActive = false;
    return a;
}

// --- SOC jump ---

static void test_soc_no_jump_on_first_ever_reading(void)
{
    // Baseline: the very first reading after boot must never be reported as
    // a jump, however far from 0 it is.
    State st;
    DalyBasicInfo info = basicInfoWithSoc(85.0f);
    Events ev = decide(st, &info, nullptr, nullptr);
    TEST_ASSERT_FALSE(ev.socJumped);
    TEST_ASSERT_TRUE(st.haveSoc);
    TEST_ASSERT_EQUAL_FLOAT(85.0f, st.lastSoc);
}

static void test_soc_no_jump_on_small_change(void)
{
    State st;
    DalyBasicInfo i1 = basicInfoWithSoc(50.0f);
    decide(st, &i1, nullptr, nullptr);

    DalyBasicInfo i2 = basicInfoWithSoc(55.0f); // +5, not > 10
    Events ev = decide(st, &i2, nullptr, nullptr);
    TEST_ASSERT_FALSE(ev.socJumped);
}

static void test_soc_jump_over_10_points(void)
{
    // >10 points either direction is reported, regardless of proximity to
    // 100%.
    State st;
    DalyBasicInfo i1 = basicInfoWithSoc(50.0f);
    decide(st, &i1, nullptr, nullptr);

    DalyBasicInfo i2 = basicInfoWithSoc(61.0f); // +11
    Events ev = decide(st, &i2, nullptr, nullptr);
    TEST_ASSERT_TRUE(ev.socJumped);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, ev.socFrom);
    TEST_ASSERT_EQUAL_FLOAT(61.0f, ev.socTo);
    TEST_ASSERT_EQUAL_FLOAT(61.0f, st.lastSoc); // state still updates
}

static void test_soc_jump_over_10_points_downward(void)
{
    State st;
    DalyBasicInfo i1 = basicInfoWithSoc(80.0f);
    decide(st, &i1, nullptr, nullptr);

    DalyBasicInfo i2 = basicInfoWithSoc(65.0f); // -15
    Events ev = decide(st, &i2, nullptr, nullptr);
    TEST_ASSERT_TRUE(ev.socJumped);
    TEST_ASSERT_EQUAL_FLOAT(80.0f, ev.socFrom);
    TEST_ASSERT_EQUAL_FLOAT(65.0f, ev.socTo);
}

static void test_soc_jump_to_100_from_below_95(void)
{
    // Special case: jump to >=99.9% from below 95% is reported even though
    // it's a <=10-point change (e.g. 94.5 -> 100.0 is only 5.5 points).
    State st;
    DalyBasicInfo i1 = basicInfoWithSoc(94.5f);
    decide(st, &i1, nullptr, nullptr);

    DalyBasicInfo i2 = basicInfoWithSoc(100.0f);
    Events ev = decide(st, &i2, nullptr, nullptr);
    TEST_ASSERT_TRUE(ev.socJumped);
    TEST_ASSERT_EQUAL_FLOAT(94.5f, ev.socFrom);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, ev.socTo);
}

static void test_soc_no_jump_to_100_from_above_95(void)
{
    // 96.0 -> 100.0: within 10 points AND lastSoc(96.0) is not < 95.0, so
    // the 100%-recalibration special case must not fire either.
    State st;
    DalyBasicInfo i1 = basicInfoWithSoc(96.0f);
    decide(st, &i1, nullptr, nullptr);

    DalyBasicInfo i2 = basicInfoWithSoc(100.0f);
    Events ev = decide(st, &i2, nullptr, nullptr);
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
    TEST_ASSERT_TRUE(st.haveMosfetBaseline);
}

static void test_mosfet_no_event_when_unchanged(void)
{
    State st;
    DalyMosfetStatus m1 = mosfet(true, true);
    decide(st, nullptr, &m1, nullptr);
    DalyMosfetStatus m2 = mosfet(true, true);
    Events ev = decide(st, nullptr, &m2, nullptr);
    TEST_ASSERT_FALSE(ev.chargeMosChanged);
    TEST_ASSERT_FALSE(ev.dischargeMosChanged);
}

static void test_mosfet_charge_on_to_off(void)
{
    State st;
    DalyMosfetStatus m1 = mosfet(true, true);
    decide(st, nullptr, &m1, nullptr);

    DalyMosfetStatus m2 = mosfet(false, true);
    Events ev = decide(st, nullptr, &m2, nullptr);
    TEST_ASSERT_TRUE(ev.chargeMosChanged);
    TEST_ASSERT_FALSE(ev.chargeMosOn);
    TEST_ASSERT_FALSE(ev.dischargeMosChanged);
}

static void test_mosfet_charge_off_to_on(void)
{
    State st;
    DalyMosfetStatus m1 = mosfet(false, true);
    decide(st, nullptr, &m1, nullptr);

    DalyMosfetStatus m2 = mosfet(true, true);
    Events ev = decide(st, nullptr, &m2, nullptr);
    TEST_ASSERT_TRUE(ev.chargeMosChanged);
    TEST_ASSERT_TRUE(ev.chargeMosOn);
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

static void test_mosfet_both_change_same_reading(void)
{
    State st;
    DalyMosfetStatus m1 = mosfet(true, true);
    decide(st, nullptr, &m1, nullptr);

    DalyMosfetStatus m2 = mosfet(false, false);
    Events ev = decide(st, nullptr, &m2, nullptr);
    TEST_ASSERT_TRUE(ev.chargeMosChanged);
    TEST_ASSERT_FALSE(ev.chargeMosOn);
    TEST_ASSERT_TRUE(ev.dischargeMosChanged);
    TEST_ASSERT_FALSE(ev.dischargeMosOn);
}

// --- Alarm bit diff ---

static void test_alarm_no_events_on_first_ever_reading(void)
{
    // Baseline: an all-nonzero first-ever alarm read must not be reported
    // as 56 "SET" events.
    State st;
    DalyAlarmStatus a = zeroAlarm();
    a.rawBytes[0] = 0xFF;
    Events ev = decide(st, nullptr, nullptr, &a);
    TEST_ASSERT_EQUAL_INT(0, ev.alarmBitCount);
    TEST_ASSERT_TRUE(st.haveAlarmBaseline);
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

    DalyAlarmStatus a2 = zeroAlarm();
    a2.rawBytes[0] = 0x01;
    a2.cellOvervoltLevel1 = true;
    a2.anyProtectionActive = true;
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
    DalyAlarmStatus a1 = zeroAlarm();
    a1.rawBytes[0] = 0x01;
    a1.cellOvervoltLevel1 = true;
    a1.anyProtectionActive = true;
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

    DalyAlarmStatus a2 = zeroAlarm();
    a2.rawBytes[3] = 0x10; // bit 4
    a2.anyProtectionActive = true;
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

    DalyAlarmStatus a2 = zeroAlarm();
    a2.rawBytes[0] = 0x01; // bit 0 -> "Cell overvoltage Level 1"
    a2.rawBytes[2] = 0x04; // bit 2 -> "Discharge overcurrent Level 1"
    a2.cellOvervoltLevel1 = true;
    a2.anyProtectionActive = true;
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

    DalyAlarmStatus a2 = zeroAlarm();
    a2.rawBytes[7] = 5; // fault code changes, no protection bit changes
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

    DalyAlarmStatus a2 = zeroAlarm();
    a2.rawBytes[0] = 0x01;
    a2.cellOvervoltLevel1 = true;
    a2.anyProtectionActive = true;
    a2.rawBytes[7] = 5;
    decide(st, nullptr, nullptr, &a2); // sets the new baseline

    DalyAlarmStatus a3 = zeroAlarm();
    a3.rawBytes[0] = 0x01;
    a3.cellOvervoltLevel1 = true;
    a3.anyProtectionActive = true;
    a3.rawBytes[7] = 5;
    Events ev3 = decide(st, nullptr, nullptr, &a3);
    TEST_ASSERT_EQUAL_INT(0, ev3.alarmBitCount);
    TEST_ASSERT_FALSE(ev3.faultCodeChanged);
}

// --- Independence: a call with only one reading present must not disturb
// the other two states' baselines ---

static void test_readings_are_independent_of_each_other(void)
{
    State st;
    // Only a MOSFET reading this call - SOC/alarm baselines untouched.
    DalyMosfetStatus m = mosfet(true, true);
    decide(st, nullptr, &m, nullptr);
    TEST_ASSERT_TRUE(st.haveMosfetBaseline);
    TEST_ASSERT_FALSE(st.haveSoc);
    TEST_ASSERT_FALSE(st.haveAlarmBaseline);

    // Now an SOC-only call - must not report a jump (no SOC baseline yet)
    // and must not touch the MOSFET baseline just established.
    DalyBasicInfo info = basicInfoWithSoc(50.0f);
    Events ev = decide(st, &info, nullptr, nullptr);
    TEST_ASSERT_FALSE(ev.socJumped);
    TEST_ASSERT_TRUE(st.haveSoc);
    TEST_ASSERT_TRUE(st.haveMosfetBaseline);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_soc_no_jump_on_first_ever_reading);
    RUN_TEST(test_soc_no_jump_on_small_change);
    RUN_TEST(test_soc_jump_over_10_points);
    RUN_TEST(test_soc_jump_over_10_points_downward);
    RUN_TEST(test_soc_jump_to_100_from_below_95);
    RUN_TEST(test_soc_no_jump_to_100_from_above_95);
    RUN_TEST(test_soc_jump_does_not_repeat_next_reading);

    RUN_TEST(test_mosfet_no_event_on_first_ever_reading);
    RUN_TEST(test_mosfet_no_event_when_unchanged);
    RUN_TEST(test_mosfet_charge_on_to_off);
    RUN_TEST(test_mosfet_charge_off_to_on);
    RUN_TEST(test_mosfet_discharge_on_to_off_and_back);
    RUN_TEST(test_mosfet_both_change_same_reading);

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
