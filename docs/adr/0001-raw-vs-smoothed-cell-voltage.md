# 0001 - Raw cell voltage drives the cutoff/gate, smoothed drives the taper

## Status
Accepted.

## Context
On 2026-09-22, under a 222A charge step, Cell 16's raw voltage reached
~3.5-3.6V while the (then 20-sample, ~48s window) smoothed value only reached
3.416V. The bridge kept reporting CCL=363A because the taper compared against
the smoothed value alone - by the time the average caught up, the cell had
already spent tens of seconds above the safe threshold. This is what issues
#9 and #8 tracked, and #8 also produced an SMA cluster fault, partly because
`cvMaxCharge`'s margin below the Daly's own 3.65V overvoltage protection was
too narrow at the time.

## Decision
`DashboardData` carries both a smoothed max/min cell voltage
(`maxCellVoltage`/`minCellVoltage`, averaged over `cfg.vSamples` readings,
default 12) and the unsmoothed value from the latest BMS read
(`maxCellVoltageRaw`/`minCellVoltageRaw`). `Glideslope::calculateCCL`/
`calculateDCL` (`include/Glideslope.h`) evaluate the **hard cutoff**
(`cvMaxCharge`/`cvMinDischarge` -> 0A) and the **alarm gate**
(`cvHighAlarmGate`/`cvLowAlarmGate` -> trickle/limp) against the **raw**
value, so a fast per-cell spike trips them immediately. The **taper** between
the start-taper and gate voltages keeps using the **smoothed** value so the
CCL/DCL doesn't jitter on ordinary per-read noise.

## Consequence
Once the smoothed value has reached the gate, the taper's slope clamp can't
give back more than trickle/limp even if the raw value has since dropped back
below it. This is intended, not a bug: it stops the reported limit from
bouncing back up on a single quiet read while the pack is still recovering.

Separately, `cvMaxCharge`'s configurable maximum is fixed at
`kDalyOvervoltageV - kMinChargeMarginV` (3.65V - 0.10V = 3.550V,
`include/SystemConfig.h`) so it can never be set closer to the Daly's own
protection than that margin - the same #8 incident found the margin that
existed at the time was already a contributing factor, so it must not be
narrowed further.
