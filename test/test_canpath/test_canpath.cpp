// End-to-end CAN path (#71, #77): the exact chain canTask runs -
// DashboardData + BmsLink + UiCommands -> snapshotFrom() -> decide() ->
// SMATxData -> encodeStatus() - asserted on the 0x351 bytes that reach the
// Sunny Island. Catches a field swapped anywhere along the way (smoothed vs
// raw, min vs max, ccl vs dcl), which every layer's own tests would miss.

#include <unity.h>
#include "StatusFrame.h"
#include "SMAFrames.h"
#include "GlideslopeFixture.h"

using StatusFrame::BmsLink;
using StatusFrame::ControlState;
using StatusFrame::UiCommands;

static SystemConfig cfg;

void setUp(void) { cfg = glideslopeTestConfig(); }
void tearDown(void) {}

static const uint32_t kNow = 100000;

// Pack in the full-current region both ways, both reads fresh.
static DashboardData healthyPack()
{
    DashboardData d;
    d.packVoltage = 52.8f;
    d.packCurrent = -10.0f;
    d.packSOC = 60.0f;
    d.maxCellVoltage = d.maxCellVoltageRaw = 3.25f;
    d.minCellVoltage = d.minCellVoltageRaw = 3.25f;
    return d;
}

static BmsLink freshLink()
{
    BmsLink l;
    l.haveBasicInfo = l.haveCellData = true;
    l.lastBasicInfoMs = l.lastCellMs = kNow;
    return l;
}

static const SMAFrames::CanFrame &frame351(const SMAFrames::TxFrameSet &set)
{
    for (int i = 0; i < set.count; i++)
        if (set.frames[i].id == 0x351)
            return set.frames[i];
    TEST_FAIL_MESSAGE("no 0x351 frame");
    return set.frames[0];
}

// Runs one canTask tick and returns 0x351 as {cvl, ccl, dcl, dvl}.
struct Limits
{
    uint16_t cvl, ccl, dcl, dvl;
};

static Limits tick(const DashboardData &data, const BmsLink &link, const UiCommands &ui)
{
    ControlState ctrl;
    StatusFrame::Decision dec = StatusFrame::decide(cfg, StatusFrame::snapshotFrom(data, link, ui, kNow), ctrl);
    TEST_ASSERT_TRUE(dec.sendFrames);
    uint8_t ticker;
    SMAFrames::TxFrameSet set = SMAFrames::encodeStatus(dec.values, 0, ticker);
    const uint8_t *b = frame351(set).data;
    return {(uint16_t)(b[0] | b[1] << 8), (uint16_t)(b[2] | b[3] << 8),
            (uint16_t)(b[4] | b[5] << 8), (uint16_t)(b[6] | b[7] << 8)};
}

static void test_healthy_pack_full_limits_on_the_wire(void)
{
    Limits l = tick(healthyPack(), freshLink(), UiCommands{});
    TEST_ASSERT_EQUAL_UINT16(1000, l.ccl); // maxChargeA 100 A
    TEST_ASSERT_EQUAL_UINT16(2000, l.dcl); // maxDischargeA 200 A
    TEST_ASSERT_EQUAL_UINT16(560, l.cvl);  // 3.5 V x 16
    TEST_ASSERT_EQUAL_UINT16(480, l.dvl);  // 3.0 V x 16
}

static void test_raw_max_spike_zeroes_ccl_on_the_wire(void)
{
    DashboardData d = healthyPack();
    d.maxCellVoltage = 3.35f;    // smoothed: taper region
    d.maxCellVoltageRaw = 3.50f; // raw: cvMaxCharge -> 0 A
    Limits l = tick(d, freshLink(), UiCommands{});
    TEST_ASSERT_EQUAL_UINT16(0, l.ccl);
    TEST_ASSERT_EQUAL_UINT16(2000, l.dcl);
}

static void test_raw_min_sag_zeroes_dcl_on_the_wire(void)
{
    DashboardData d = healthyPack();
    d.minCellVoltage = 3.15f;    // smoothed: taper region
    d.minCellVoltageRaw = 3.00f; // raw: cvMinDischarge -> 0 A
    Limits l = tick(d, freshLink(), UiCommands{});
    TEST_ASSERT_EQUAL_UINT16(1000, l.ccl);
    TEST_ASSERT_EQUAL_UINT16(0, l.dcl);
}

static void test_smoothed_max_taper_on_the_wire(void)
{
    DashboardData d = healthyPack();
    d.maxCellVoltage = 3.35f;    // midway 3.3..3.4 -> 52.5 A
    d.maxCellVoltageRaw = 3.30f; // below the gate
    Limits l = tick(d, freshLink(), UiCommands{});
    TEST_ASSERT_EQUAL_UINT16(525, l.ccl);
}

static void test_stale_cells_zero_both_on_the_wire(void)
{
    BmsLink link = freshLink();
    link.lastCellMs = kNow - 70000; // > 60 s bmsTimeout
    Limits l = tick(healthyPack(), link, UiCommands{});
    TEST_ASSERT_EQUAL_UINT16(0, l.ccl);
    TEST_ASSERT_EQUAL_UINT16(0, l.dcl);
}

static void test_reset_request_zeroes_dvl_on_the_wire(void)
{
    UiCommands ui;
    ui.resetRequested = true;
    Limits l = tick(healthyPack(), freshLink(), ui);
    TEST_ASSERT_EQUAL_UINT16(0, l.dvl);
    TEST_ASSERT_EQUAL_UINT16(1000, l.ccl);
}

static void test_manual_maintenance_on_the_wire(void)
{
    // cvMaxCharge left at the fixture's 3.5V, so normal CVL (560) equals
    // kMaintCvlDeciV and the min() cap doesn't bite - see
    // test_maintenance_cvl_capped_below_normal_on_the_wire for the case
    // where it does (#60).
    UiCommands ui;
    ui.manualMaintForce = true;
    Limits l = tick(healthyPack(), freshLink(), ui);
    TEST_ASSERT_EQUAL_UINT16(200, l.ccl); // maintAmps 20 A
    TEST_ASSERT_EQUAL_UINT16(0, l.dcl);
    TEST_ASSERT_EQUAL_UINT16(560, l.cvl); // kMaintCvlDeciV
}

static void test_maintenance_cvl_capped_below_normal_on_the_wire(void)
{
    // #60: with cvMaxCharge lowered so normal CVL (55.2 V -> 552) is below
    // the fixed kMaintCvlDeciV (56.0 V -> 560), maintenance must not ask
    // for the higher voltage - min(560, 552) = 552 on the wire. This used
    // to read 560 unconditionally (the bypass this ticket fixes).
    TEST_ASSERT_TRUE(cfg.cvMaxCharge.set(3.45f));
    UiCommands ui;
    ui.manualMaintForce = true;
    Limits l = tick(healthyPack(), freshLink(), ui);
    TEST_ASSERT_EQUAL_UINT16(552, l.cvl);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_healthy_pack_full_limits_on_the_wire);
    RUN_TEST(test_raw_max_spike_zeroes_ccl_on_the_wire);
    RUN_TEST(test_raw_min_sag_zeroes_dcl_on_the_wire);
    RUN_TEST(test_smoothed_max_taper_on_the_wire);
    RUN_TEST(test_stale_cells_zero_both_on_the_wire);
    RUN_TEST(test_reset_request_zeroes_dvl_on_the_wire);
    RUN_TEST(test_manual_maintenance_on_the_wire);
    RUN_TEST(test_maintenance_cvl_capped_below_normal_on_the_wire);
    return UNITY_END();
}
