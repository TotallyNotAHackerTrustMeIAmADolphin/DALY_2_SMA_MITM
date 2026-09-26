#pragma once

// One ordered table of telemetry column descriptors driving the CSV header
// and row, built from DashboardData (#32). The SSE JSON is a separate,
// hand-written format (TelemetryJson.h, #95) with different precision and
// key order, not derived from this table. Pure, so test/test_telemetryschema
// runs it natively.
//
// Column::format()'s snprintf spec for each field is exact and byte-stable:
// existing CSV files on the SD card, and the dashboard JS parsing the SSE
// JSON, depend on it. Treat a format-string change here with the same care
// as Glideslope.h's math - it changes every future row on disk.
//
// Index order is part of the contract, covered by the native test:
// Timestamp=0, PackV=1, PackI=2, SOC=3, MinCellV=4, MaxCellV=5, ReqI=6.
// Callers (readGraphSeries()) look columns up by name via index(), never by
// memorizing position.

#include <stdio.h>
#include <string.h>
#include "DashboardData.h"

namespace TelemetrySchema
{
    struct Column
    {
        const char *csvName;
        // snprintf-style: returns the number of characters written
        // (excluding the NUL), or <= 0 to mean "nothing to write, omit this
        // field from the row entirely" (used only by the CellN family, for
        // a cell slot that hasn't been read yet this boot).
        int (*format)(const DashboardData &d, char *buf, size_t len);
    };

    // Timestamp is produced by the logger from the row's write time, not
    // from DashboardData - it carries no formatter. formatRow() below
    // starts at index 1 for that reason; this placeholder exists only so
    // every table entry has a valid, callable format function.
    inline int formatTimestampPlaceholder(const DashboardData &, char *, size_t) { return 0; }

    inline int formatPackV(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.2f", d.packVoltage); }
    inline int formatPackI(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.2f", d.packCurrent); }
    inline int formatSOC(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.1f", d.packSOC); }
    inline int formatMinCellV(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.3f", d.minCellVoltage); }
    inline int formatMaxCellV(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.3f", d.maxCellVoltage); }
    inline int formatReqI(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.1f", d.requestedCurrent); }
    inline int formatMode(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%s", d.smaChargeMode); }
    inline int formatForceCharge(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.forceCharge ? 1 : 0); }
    inline int formatMaintenanceActive(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.maintenanceActive ? 1 : 0); }
    inline int formatGridPresent(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.gridPresent ? 1 : 0); }

    // One descriptor family for Cell1..Cell16 (N is 1-based, matching the
    // CSV column name CellN, indexing into cellVoltages at N-1) rather than
    // 16 hand-written functions. Returns 0 - omit this field entirely - if
    // this cell slot hasn't actually been read yet (e.g. early in boot).
    template <int N>
    inline int formatCell(const DashboardData &d, char *buf, size_t len)
    {
        if ((size_t)(N - 1) >= d.cellVoltages.size())
            return 0;
        return snprintf(buf, len, "%.3f", d.cellVoltages[N - 1]);
    }

    inline int formatChargeMOS(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.chargeMosOn ? 1 : 0); }
    inline int formatDischargeMOS(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.dischargeMosOn ? 1 : 0); }
    inline int formatBmsProtection(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.bmsProtectionActive ? 1 : 0); }
    inline int formatCellOV1(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.cellOvervoltLevel1 ? 1 : 0); }
    inline int formatCellOV2(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.cellOvervoltLevel2 ? 1 : 0); }
    inline int formatPackOV1(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.packOvervoltLevel1 ? 1 : 0); }
    inline int formatPackOV2(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%d", d.packOvervoltLevel2 ? 1 : 0); }
    inline int formatMinCellRaw(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.3f", d.minCellVoltageRaw); }
    inline int formatMaxCellRaw(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.3f", d.maxCellVoltageRaw); }
    inline int formatRawSpreadMv(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%u", (unsigned)d.cellSpreadRawMv); }
    inline int formatDerate(const DashboardData &d, char *buf, size_t len) { return snprintf(buf, len, "%.2f", d.derateFactor); }

    // Column order is the CSV row layout - see CLAUDE.md's SDLogger bullet.
    static const Column kColumns[] = {
        {"Timestamp", formatTimestampPlaceholder},
        {"PackV", formatPackV},
        {"PackI", formatPackI},
        {"SOC", formatSOC},
        {"MinCellV", formatMinCellV},
        {"MaxCellV", formatMaxCellV},
        {"ReqI", formatReqI},
        {"Mode", formatMode},
        {"ForceCharge", formatForceCharge},
        {"MaintenanceActive", formatMaintenanceActive},
        {"GridPresent", formatGridPresent},
        {"Cell1", formatCell<1>},
        {"Cell2", formatCell<2>},
        {"Cell3", formatCell<3>},
        {"Cell4", formatCell<4>},
        {"Cell5", formatCell<5>},
        {"Cell6", formatCell<6>},
        {"Cell7", formatCell<7>},
        {"Cell8", formatCell<8>},
        {"Cell9", formatCell<9>},
        {"Cell10", formatCell<10>},
        {"Cell11", formatCell<11>},
        {"Cell12", formatCell<12>},
        {"Cell13", formatCell<13>},
        {"Cell14", formatCell<14>},
        {"Cell15", formatCell<15>},
        {"Cell16", formatCell<16>},
        {"ChargeMOS", formatChargeMOS},
        {"DischargeMOS", formatDischargeMOS},
        {"BmsProtection", formatBmsProtection},
        {"CellOV1", formatCellOV1},
        {"CellOV2", formatCellOV2},
        {"PackOV1", formatPackOV1},
        {"PackOV2", formatPackOV2},
        {"MinCellRaw", formatMinCellRaw},
        {"MaxCellRaw", formatMaxCellRaw},
        {"RawSpreadMv", formatRawSpreadMv},
        {"Derate", formatDerate},
    };

    inline int count() { return (int)(sizeof(kColumns) / sizeof(kColumns[0])); }

    // The columns the Graphs page (#93) plots - one ordered name list
    // shared by readGraphSeries() (resolves each into a kColumns index,
    // builds the CSV header from it, and sizes its Accumulator off it) and
    // graphs_html's JS (reads the served header row to map name -> column),
    // instead of each of those writing out the same seven names separately.
    static const char *const kGraphColumns[] = {
        "Timestamp", "PackV", "PackI", "SOC", "MinCellV", "MaxCellV", "ReqI",
    };
    constexpr size_t kGraphColumnCount = sizeof(kGraphColumns) / sizeof(kGraphColumns[0]);

    // Linear search is fine - called a handful of times per request
    // (readGraphSeries() resolves its column indices once, outside its
    // per-line loop), never in the hot per-sample logging path.
    inline int index(const char *csvName)
    {
        for (int i = 0; i < count(); i++)
            if (strcmp(kColumns[i].csvName, csvName) == 0)
                return i;
        return -1;
    }

    // Builds one CSV data row - everything after Timestamp, which the
    // logger prepends itself from the row's write time - by joining every
    // column's formatted text with commas, in table order, skipping columns
    // whose formatter reports nothing to write (a CellN slot beyond how
    // many cells have actually been read this boot). Returns the number of
    // characters written into buf (excluding the NUL).
    inline int formatRow(const DashboardData &d, char *buf, size_t len)
    {
        int written = 0;
        for (int i = 1; i < count(); i++)
        {
            char field[64];
            int n = kColumns[i].format(d, field, sizeof(field));
            if (n <= 0)
                continue;
            if ((size_t)written >= len)
                break;
            int m = snprintf(buf + written, len - (size_t)written, written > 0 ? ",%s" : "%s", field);
            if (m < 0)
                break;
            written += m;
        }
        return written;
    }

    // Joins every column's CSV name with commas - the CSV header line,
    // Timestamp included. Returns the number of characters written into
    // buf (excluding the NUL).
    inline int formatHeader(char *buf, size_t len)
    {
        int written = 0;
        for (int i = 0; i < count(); i++)
        {
            if ((size_t)written >= len)
                break;
            int m = snprintf(buf + written, len - (size_t)written, i > 0 ? ",%s" : "%s", kColumns[i].csvName);
            if (m < 0)
                break;
            written += m;
        }
        return written;
    }
}
