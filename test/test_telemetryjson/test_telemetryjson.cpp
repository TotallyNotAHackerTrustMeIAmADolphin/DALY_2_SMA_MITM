// Native unit tests for TelemetryJson::format() (#95): `pio test -e native`.
// Includes the real header - no mirrored copy.

#include <unity.h>
#include <string.h>
#include "TelemetryJson.h"

void setUp(void) {}
void tearDown(void) {}

static DashboardData sample()
{
    DashboardData d;
    d.packVoltage = 52.34f;
    d.avgCellVoltage = 3.271f;
    d.minCellVoltage = 3.201f;
    d.maxCellVoltage = 3.350f;
    d.minCellVoltageRaw = 3.195f;
    d.maxCellVoltageRaw = 3.360f;
    d.packCurrent = 12.5f;
    d.requestedCurrent = 250.0f;
    d.packSOC = 87.5f;
    d.smaChargeMode = "Bulk";
    d.forceCharge = true;
    d.maintenanceActive = false;
    d.isResetting = false;
    d.cellSpreadRawMv = 45;
    d.derateFactor = 0.85f;
    float cells[16] = {3.201f, 3.210f, 3.215f, 3.220f, 3.225f, 3.230f, 3.235f, 3.240f,
                        3.245f, 3.250f, 3.255f, 3.260f, 3.265f, 3.270f, 3.275f, 3.350f};
    for (float c : cells)
        d.cellVoltages.push_back(c);
    return d;
}

// Byte-exact against the pre-#95 hand-written cellsStr[256]+strcat version -
// derived from that code, running unchanged, before this refactor.
static void test_matches_pre_refactor_output_exactly(void)
{
    DashboardData d = sample();
    char buf[1024];
    TEST_ASSERT_TRUE(TelemetryJson::format(d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"v\":52.34,\"cv\":3.271,\"minC\":3.201,\"maxC\":3.350,\"minCellRaw\":3.195,\"maxCellRaw\":3.360,"
        "\"i\":12.5,\"reqI\":250.0,\"soc\":87.5,\"smam\":\"Bulk\",\"maint\":0,\"force\":1,\"isR\":0,"
        "\"spreadMv\":45,\"derate\":0.85,\"cells\":[3.201,3.210,3.215,3.220,3.225,3.230,3.235,3.240,"
        "3.245,3.250,3.255,3.260,3.265,3.270,3.275,3.350]}",
        buf);
}

static void test_empty_cells_array(void)
{
    DashboardData d = sample();
    d.cellVoltages.clear();
    char buf[1024];
    TEST_ASSERT_TRUE(TelemetryJson::format(d, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cells\":[]}"));
}

// Cell loop caps at kPackCells, matching the pre-refactor buffer's only
// real safety margin (16 cells always fit its cellsStr[256]).
static void test_cell_loop_caps_at_kPackCells(void)
{
    DashboardData d = sample();
    for (int i = 0; i < 4; i++)
        d.cellVoltages.push_back(9.999f); // 20 cells total

    char buf[1024];
    TEST_ASSERT_TRUE(TelemetryJson::format(d, buf, sizeof(buf)));
    TEST_ASSERT_NULL(strstr(buf, "9.999")); // the 17th+ cells never appear

    const char *marker = "\"cells\":[";
    const char *arrStart = strstr(buf, marker);
    TEST_ASSERT_NOT_NULL(arrStart);
    arrStart += strlen(marker);
    const char *arrEnd = strchr(arrStart, ']');
    TEST_ASSERT_NOT_NULL(arrEnd);

    int commas = 0;
    for (const char *p = arrStart; p < arrEnd; p++)
        if (*p == ',')
            commas++;
    TEST_ASSERT_EQUAL(kPackCells - 1, commas);
}

// 16 worst-case-width cells (large negative values, widest %.3f form) must
// still fit a realistic buffer and produce well-formed JSON.
static void test_worst_case_16_cells_fits_and_is_well_formed(void)
{
    DashboardData d = sample();
    d.cellVoltages.clear();
    for (int i = 0; i < 16; i++)
        d.cellVoltages.push_back(-9999.999f);

    char buf[1024];
    TEST_ASSERT_TRUE(TelemetryJson::format(d, buf, sizeof(buf)));

    size_t len = strlen(buf);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL('{', buf[0]);
    TEST_ASSERT_EQUAL('}', buf[len - 1]);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cells\":[-9999.999,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "-9999.999]}"));
}

// A buffer too small to hold even the fixed fields must fail, not overflow.
static void test_undersized_buffer_fails_cleanly(void)
{
    DashboardData d = sample();
    char buf[8];
    TEST_ASSERT_FALSE(TelemetryJson::format(d, buf, sizeof(buf)));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_matches_pre_refactor_output_exactly);
    RUN_TEST(test_empty_cells_array);
    RUN_TEST(test_cell_loop_caps_at_kPackCells);
    RUN_TEST(test_worst_case_16_cells_fits_and_is_well_formed);
    RUN_TEST(test_undersized_buffer_fails_cleanly);
    return UNITY_END();
}
