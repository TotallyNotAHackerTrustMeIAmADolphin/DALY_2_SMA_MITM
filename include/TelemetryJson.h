#pragma once
#include "DashboardData.h"
#include "SystemConfig.h" // kPackCells
#include "BoundedWriter.h"

// The SSE "data" event's JSON (#95), via bounded appends instead of an
// unchecked strcat loop. Hand-written, not derived from
// TelemetrySchema::kColumns: several keys/precisions genuinely differ from
// their CSV column (e.g. "i" is one decimal here, two in the CSV's PackI).
namespace TelemetryJson
{
    // Returns false if it doesn't fit (buf is left however far append() got).
    inline bool format(const DashboardData &d, char *buf, size_t len)
    {
        BoundedWriter w(buf, len);
        w.append("{\"v\":%.2f,\"cv\":%.3f,\"minC\":%.3f,\"maxC\":%.3f,\"minCellRaw\":%.3f,\"maxCellRaw\":%.3f,"
                  "\"i\":%.1f,\"reqI\":%.1f,\"soc\":%.1f,\"smam\":\"%s\",\"maint\":%d,\"force\":%d,\"isR\":%d,"
                  "\"spreadMv\":%u,\"derate\":%.2f,\"cells\":[",
                  d.packVoltage, d.avgCellVoltage, d.minCellVoltage, d.maxCellVoltage,
                  d.minCellVoltageRaw, d.maxCellVoltageRaw, d.packCurrent, d.requestedCurrent, d.packSOC,
                  d.smaChargeMode, (int)d.maintenanceActive, (int)d.forceCharge, (int)d.isResetting,
                  (unsigned)d.cellSpreadRawMv, d.derateFactor);

        size_t n = d.cellVoltages.size();
        if (n > (size_t)kPackCells)
            n = kPackCells; // the pack has kPackCells cells; a longer vector is a bug, not more data
        for (size_t i = 0; i < n; i++)
            w.append(i ? ",%.3f" : "%.3f", d.cellVoltages[i]);
        w.append("]}");
        return w.ok();
    }
}
