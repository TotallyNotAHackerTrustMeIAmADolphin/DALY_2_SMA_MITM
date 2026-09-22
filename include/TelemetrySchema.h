#pragma once

// One ordered table of telemetry column descriptors driving the CSV header,
// the CSV row and (for the fields whose formatting genuinely matches - see
// WebDashboard::broadcastTelemetry) the SSE JSON, all built from
// DashboardData (#32). Pure - no Arduino/FreeRTOS dependencies, like
// SystemConfig.h/Glideslope.h - so test/test_telemetryschema compiles and
// runs *this* code natively (`pio test -e native`) instead of a mirrored
// copy of it.
//
// Column::format() reproduces the exact snprintf spec each field used
// before this refactor (see the CSV row-layout sentence in CLAUDE.md's
// SDLogger bullet) byte for byte: existing CSV files on the SD card, and
// the dashboard JS that parses the SSE JSON, depend on those bytes staying
// identical. Treat a change to a format string here with the same care as
// Glideslope.h's math - it changes every future row on disk.
//
// Index semantics are part of the contract and covered by the native test:
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
        const char *jsonKey; // nullptr = not present in the SSE JSON
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
    // 16 hand-written functions. Returns 0 - omit this field entirely, same
    // as the pre-refactor loop bounded to min(16, cellVoltages.size()) - if
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

    // Ordered exactly like the pre-refactor CSV row (see CLAUDE.md's
    // SDLogger bullet). jsonKey mirrors WebDashboard::broadcastTelemetry's
    // SSE JSON key for the fields shared between the two outputs with
    // *identical* formatting; nullptr where a CSV column either has no JSON
    // counterpart (Timestamp; GridPresent; the per-cell columns - JSON
    // bundles cells as one "cells" array instead; the MOS/alarm columns) or
    // where the JSON's formatting of that same field genuinely differs from
    // its CSV formatting - PackI is %.2f in CSV but %.1f in JSON's "i" - see
    // broadcastTelemetry's comment for why that one field stays hand-written
    // there instead of sharing this column's formatter.
    static const Column kColumns[] = {
        {"Timestamp", nullptr, formatTimestampPlaceholder},
        {"PackV", "v", formatPackV},
        {"PackI", nullptr, formatPackI},
        {"SOC", "soc", formatSOC},
        {"MinCellV", "minC", formatMinCellV},
        {"MaxCellV", "maxC", formatMaxCellV},
        {"ReqI", "reqI", formatReqI},
        {"Mode", "smam", formatMode},
        {"ForceCharge", "force", formatForceCharge},
        {"MaintenanceActive", "maint", formatMaintenanceActive},
        {"GridPresent", nullptr, formatGridPresent},
        {"Cell1", nullptr, formatCell<1>},
        {"Cell2", nullptr, formatCell<2>},
        {"Cell3", nullptr, formatCell<3>},
        {"Cell4", nullptr, formatCell<4>},
        {"Cell5", nullptr, formatCell<5>},
        {"Cell6", nullptr, formatCell<6>},
        {"Cell7", nullptr, formatCell<7>},
        {"Cell8", nullptr, formatCell<8>},
        {"Cell9", nullptr, formatCell<9>},
        {"Cell10", nullptr, formatCell<10>},
        {"Cell11", nullptr, formatCell<11>},
        {"Cell12", nullptr, formatCell<12>},
        {"Cell13", nullptr, formatCell<13>},
        {"Cell14", nullptr, formatCell<14>},
        {"Cell15", nullptr, formatCell<15>},
        {"Cell16", nullptr, formatCell<16>},
        {"ChargeMOS", nullptr, formatChargeMOS},
        {"DischargeMOS", nullptr, formatDischargeMOS},
        {"BmsProtection", nullptr, formatBmsProtection},
        {"CellOV1", nullptr, formatCellOV1},
        {"CellOV2", nullptr, formatCellOV2},
        {"PackOV1", nullptr, formatPackOV1},
        {"PackOV2", nullptr, formatPackOV2},
        {"MinCellRaw", "minCellRaw", formatMinCellRaw},
        {"MaxCellRaw", "maxCellRaw", formatMaxCellRaw},
        {"RawSpreadMv", "spreadMv", formatRawSpreadMv},
        {"Derate", "derate", formatDerate},
    };

    inline int count() { return (int)(sizeof(kColumns) / sizeof(kColumns[0])); }

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
    // many cells have actually been read this boot). Matches the
    // pre-refactor logTelemetry()'s behavior of bounding the cell loop to
    // min(16, cellVoltages.size()) and appending every other field
    // unconditionally. Returns the number of characters written into buf
    // (excluding the NUL).
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
