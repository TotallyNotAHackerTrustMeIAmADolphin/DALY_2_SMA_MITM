// Native unit tests for include/TelemetrySchema.h: `pio test -e native`.
// Includes the real header - no mirrored copy to keep in sync (#32).

#include <unity.h>
#include <string.h>
#include "TelemetrySchema.h"

using namespace TelemetrySchema;

// The exact CSV row-layout string from CLAUDE.md's SDLogger bullet,
// Cell1..16 expanded - keep these two in lockstep by hand; the point of
// this test is to catch the two drifting apart.
static const char *kExpectedHeader =
    "Timestamp,PackV,PackI,SOC,MinCellV,MaxCellV,ReqI,Mode,ForceCharge,"
    "MaintenanceActive,GridPresent,"
    "Cell1,Cell2,Cell3,Cell4,Cell5,Cell6,Cell7,Cell8,Cell9,Cell10,Cell11,"
    "Cell12,Cell13,Cell14,Cell15,Cell16,"
    "ChargeMOS,DischargeMOS,BmsProtection,CellOV1,CellOV2,PackOV1,PackOV2,"
    "MinCellRaw,MaxCellRaw,RawSpreadMv,Derate";

static int countFields(const char *csv)
{
    if (*csv == '\0')
        return 0;
    int n = 1;
    for (const char *p = csv; *p; p++)
        if (*p == ',')
            n++;
    return n;
}

// A single populated sample used by several tests below - all 16 cells
// read, every flag set to a non-default value so a formatter that silently
// no-ops would be caught by the row-string comparison.
static DashboardData makeSample()
{
    DashboardData d{};
    d.packVoltage = 52.35f;
    d.avgCellVoltage = 3.271f; // not a CSV column
    d.minCellVoltage = 3.201f;
    d.maxCellVoltage = 3.349f;
    d.minCellVoltageRaw = 3.195f;
    d.maxCellVoltageRaw = 3.360f;
    for (int i = 0; i < 16; i++)
        d.cellVoltages.push_back(3.300f + (float)i * 0.010f);
    d.cellSpreadRawMv = 95;
    d.derateFactor = 0.73f;
    d.packCurrent = -12.5f;
    d.packTemp = 0; // not a CSV column
    d.packSOC = 87.5f;
    d.requestedCurrent = 45.0f;
    d.smaChargeMode = "Bulk";
    d.forceCharge = true;
    d.maintenanceActive = false;
    d.isResetting = false; // not a CSV column
    d.gridPresent = true;
    d.chargeMosOn = true;
    d.dischargeMosOn = false;
    d.bmsProtectionActive = false;
    d.cellOvervoltLevel1 = false;
    d.cellOvervoltLevel2 = false;
    d.packOvervoltLevel1 = false;
    d.packOvervoltLevel2 = false;
    return d;
}

void setUp(void) {}
void tearDown(void) {}

// --- Header ---

static void test_header_matches_claude_md(void)
{
    char buf[512];
    int n = formatHeader(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(kExpectedHeader, buf);
}

// --- index() ---

static void test_index_first_seven_columns(void)
{
    TEST_ASSERT_EQUAL(0, index("Timestamp"));
    TEST_ASSERT_EQUAL(1, index("PackV"));
    TEST_ASSERT_EQUAL(2, index("PackI"));
    TEST_ASSERT_EQUAL(3, index("SOC"));
    TEST_ASSERT_EQUAL(4, index("MinCellV"));
    TEST_ASSERT_EQUAL(5, index("MaxCellV"));
    TEST_ASSERT_EQUAL(6, index("ReqI"));
}

static void test_index_max_cell_raw(void)
{
    // Relative to PackOV2 (the last MOS/alarm column), not an absolute
    // literal: a new column appended after Derate (CLAUDE.md's documented
    // extension point) must not move these two.
    TEST_ASSERT_EQUAL(index("PackOV2") + 1, index("MinCellRaw"));
    TEST_ASSERT_EQUAL(index("MinCellRaw") + 1, index("MaxCellRaw"));
}

static void test_index_unknown_name(void)
{
    TEST_ASSERT_EQUAL(-1, index("nope"));
}

// --- kGraphColumns (#93): every name the Graphs page asks for must
// actually resolve, or readGraphSeries() would size its Accumulator off a
// SIZE_MAX index and silently emit an empty column.
static void test_graph_columns_all_resolve(void)
{
    for (size_t i = 0; i < kGraphColumnCount; i++)
        TEST_ASSERT_TRUE(index(kGraphColumns[i]) >= 0);
}

// --- count() / formatter output count ---

static void test_formatter_output_count_matches_columns_minus_timestamp(void)
{
    // Timestamp (index 0) has no real formatter (it's produced by the
    // logger from the row's write time, not from DashboardData) and
    // reports "nothing to write" like a not-yet-read cell would - every
    // other column, given a fully-populated sample, must produce output.
    DashboardData d = makeSample();
    int produced = 0;
    for (int i = 0; i < count(); i++)
    {
        char field[64];
        int n = kColumns[i].format(d, field, sizeof(field));
        if (n > 0)
            produced++;
    }
    TEST_ASSERT_EQUAL(count() - 1, produced);
}

static void test_row_field_count_matches_header_field_count(void)
{
    // formatRow() only builds the part of the row after Timestamp (the
    // logger prepends that itself); prepending a stand-in timestamp here
    // mirrors loggingTask()'s own "%s,%s\n" and lets the two field counts
    // be compared directly.
    DashboardData d = makeSample();
    char row[480];
    int n = formatRow(d, row, sizeof(row));
    TEST_ASSERT_TRUE(n > 0);

    char full[512];
    snprintf(full, sizeof(full), "2024-01-01 00:00:00,%s", row);
    TEST_ASSERT_EQUAL(countFields(kExpectedHeader), countFields(full));
}

// --- Known row string, hand-derived from makeSample() above ---
//
// PackV   %.2f  52.35     -> "52.35"
// PackI   %.2f  -12.5     -> "-12.50"
// SOC     %.1f  87.5      -> "87.5"
// MinCellV %.3f 3.201     -> "3.201"
// MaxCellV %.3f 3.349     -> "3.349"
// ReqI    %.1f  45.0      -> "45.0"
// Mode    %s    "Bulk"    -> "Bulk"
// ForceCharge         1   -> "1"
// MaintenanceActive   0   -> "0"
// GridPresent         1   -> "1"
// Cell1..16 %.3f, 3.300 + i*0.010 for i=0..15 -> "3.300".."3.450"
// ChargeMOS           1   -> "1"
// DischargeMOS        0   -> "0"
// BmsProtection       0   -> "0"
// CellOV1..PackOV2    0,0,0,0 -> "0","0","0","0"
// MinCellRaw %.3f 3.195   -> "3.195"
// MaxCellRaw %.3f 3.360   -> "3.360"
// RawSpreadMv %u  95      -> "95"
// Derate      %.2f 0.73   -> "0.73"
static const char *kExpectedRow =
    "52.35,-12.50,87.5,3.201,3.349,45.0,Bulk,1,0,1,"
    "3.300,3.310,3.320,3.330,3.340,3.350,3.360,3.370,3.380,3.390,3.400,"
    "3.410,3.420,3.430,3.440,3.450,"
    "1,0,0,0,0,0,0,"
    "3.195,3.360,95,0.73";

static void test_sample_formats_to_known_row(void)
{
    DashboardData d = makeSample();
    char row[480];
    int n = formatRow(d, row, sizeof(row));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(kExpectedRow, row);
}

// --- Missing-cell behavior (matches the pre-refactor cellCount bound) ---

static void test_missing_cells_are_omitted_not_padded(void)
{
    DashboardData d = makeSample();
    size_t totalCells = d.cellVoltages.size(); // 16, all read in makeSample()
    d.cellVoltages.resize(5); // only 5 of 16 cells read so far
    size_t missingCells = totalCells - d.cellVoltages.size();

    char row[480];
    formatRow(d, row, sizeof(row));

    // count() - 1 excludes Timestamp (formatRow only builds what follows
    // it); each missing cell omits one field rather than a blank
    // placeholder, so it comes straight off the total.
    TEST_ASSERT_EQUAL((int)(count() - 1 - missingCells), countFields(row));
    // The 5th cell (index 4) is 3.300+4*0.010=3.340, immediately followed
    // by ChargeMOS's "1" - no empty placeholders for the missing 11 cells.
    TEST_ASSERT_NOT_NULL(strstr(row, "3.340,1,0,0,0,0,0,0,3.195"));
}

// A default-constructed DashboardData (#80) formats deterministically: the
// pre-BMS state, no cells yet, no derating.
static void test_default_constructed_row(void)
{
    DashboardData d;
    char row[480];
    TEST_ASSERT_TRUE(formatRow(d, row, sizeof(row)) > 0);
    TEST_ASSERT_EQUAL_STRING("0.00,0.00,0.0,0.000,0.000,0.0,Unknown,0,0,0,"
                             "0,0,0,0,0,0,0,"
                             "0.000,0.000,0,1.00",
                             row);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_matches_claude_md);
    RUN_TEST(test_index_first_seven_columns);
    RUN_TEST(test_index_max_cell_raw);
    RUN_TEST(test_index_unknown_name);
    RUN_TEST(test_graph_columns_all_resolve);
    RUN_TEST(test_formatter_output_count_matches_columns_minus_timestamp);
    RUN_TEST(test_row_field_count_matches_header_field_count);
    RUN_TEST(test_sample_formats_to_known_row);
    RUN_TEST(test_missing_cells_are_omitted_not_padded);
    RUN_TEST(test_default_constructed_row);
    return UNITY_END();
}
