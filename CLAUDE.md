# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Dual-core ESP32 firmware (PlatformIO/Arduino framework) that acts as a Man-In-The-Middle bridge between a **Daly Smart BMS** (RS485) and an **SMA Sunny Island Inverter** (CAN). Target hardware is the LilyGO T-CAN485 board. Instead of trusting the BMS's coarse SOC-based current limits, it computes smooth Charge/Discharge Current Limits (the "Glideslope") directly from individual cell voltages and reports those to the inverter over CAN, along with a live web dashboard.

This firmware controls high-power charging/discharging of a real battery pack. Treat `calculateCCL`/`calculateDCL` in `src/main.cpp` and the CAN protocol implementation as safety-critical — verify glideslope math carefully on any change.

## Build & Run (PlatformIO)

- **Build:** `pio run`
- **Upload via USB:** `pio run -t upload` (platformio.ini defaults to OTA upload; override `--upload-port` for a USB serial port)
- **Upload via OTA:** `pio run -t upload --upload-port <DEVICE_IP>` (default configured IP is `192.168.178.56`, port 3232)
- **Serial monitor:** `pio run -t monitor` (115200 baud)
- **Clean:** `pio run -t clean`
- **Run unit tests:** `pio test` (Unity framework; runs on-device/native per `test/` — see below)

### First-time setup
1. Copy `include/secrets_example.h` to `include/secrets.h` and fill in `ssid`/`password`. This file is gitignored — never commit real credentials.
2. Static IP is hardcoded near the top of `src/main.cpp` (`local_IP`, `gateway`, `subnet`); adjust to match the target network.

## Architecture

### Dual-core FreeRTOS split
- **Core 0 (`bmsTask` in `src/main.cpp`):** Polls the Daly BMS over RS485 every 2s (basic info + per-cell voltages), applies a per-cell moving-average smoothing filter (`cellBuffers`, window size `cfg.vSamples`), and writes results into the shared `currentData`.
- **Core 1 (Arduino `loop()`):** Drives the SMA CAN heartbeat (every 250ms), OTA handling, and CAN bus health checks. The Async Web Server / SSE run on their own task via `ESPAsyncWebServer`.
- **Shared state:** `currentData` (`DashboardData`) and `cfg` (`SystemConfig`), both defined in `include/SystemState.h`, are shared between cores and guarded by `dataMutex` (a `SemaphoreHandle_t`). Any new cross-core field must be read/written inside `xSemaphoreTake(dataMutex, ...)` / `xSemaphoreGive(dataMutex)`, following the existing pattern in `bmsTask` and `loop()`.

### Glideslope current-limit logic
`calculateCCL()` and `calculateDCL()` in `src/main.cpp` are the core control algorithm:
- Below `cvStartTaper`/above `cvStartDTaper`: full `maxChargeA`/`maxDischargeA`.
- Between the start-taper and alarm-gate voltages: linear interpolation down to trickle/limp current.
- At/beyond the alarm gate: fixed trickle (`trickleA`) or limp (`limpDischargeA`) current.
- At/beyond the hard max/min voltage: 0A.
- A BMS comms timeout (`cfg.bmsTimeout` seconds since `lastSuccessfulBmsRead`) forces 0A as a fail-safe, checked first in both functions.
- Maintenance mode (`currentData.maintenanceActive`, driven by either manual UI toggle or automatic low-voltage winter maintenance detection in `loop()`) overrides normal taper behavior.

`test/test_glideslope.cpp` unit-tests this taper math using a duplicated/mocked copy of the logic (it cannot include `main.cpp` directly) — when changing the real `calculateCCL`/`calculateDCL`, keep the test's mirrored logic in sync.

### Module layout
- `src/DalyRS485.cpp` / `include/DalyRS485.h` — Daly BMS RS485 protocol (9600 baud): basic pack info + per-cell voltage reads (`0x90`/`0x95`), plus the BMS's own hardware protection state via `readMosfetStatus()` (`0x93`: charge/discharge MOSFET on/off) and `readAlarmStatus()` (`0x98`: cell/pack overvoltage alarm bits) — polled each `bmsTask` cycle, independent of `calculateCCL`/`calculateDCL`, so a BMS-initiated cutoff is visible even if our own glideslope never would have tripped. Both parse defensively (reject the frame rather than trust an out-of-range byte) since the exact `0x93`/`0x98` byte layout is from the documented Daly protocol family, not verified against every pack's firmware.
- `src/SMA_CAN.cpp` / `include/SMA_CAN.h` — SMA/Victron CAN protocol (500kbps), status frame TX (CCL/DCL/CVL/DVL, SOC, etc.), bus-off detection and recovery ("nuclear" driver suspend/resume if the CAN cable is unplugged and reattached).
- `src/WebDashboard.cpp` / `include/WebDashboard.h` — `ESPAsyncWebServer` routes, Server-Sent Events for live telemetry push, the UI action callback (`handleUIAction` in `main.cpp`) for toggles like manual maintenance force and SMA cluster reset, and the `/api/logs/*` routes backing the Logs/Graphs pages (list, tail-view, full-file download, decimated graph series) — see `findLogFile()` for the shared filename-whitelist/path-traversal guard these all use.
- `src/SDLogger.cpp` / `include/SDLogger.h` — background SD-card logger: a queue-fed FreeRTOS task writes BMS/SMA telemetry to daily-rotated CSV files and system events to a parallel `.log` file. All direct (non-queued) SD/SPI access — the writer task's own file writes, and every web-route read (list/tail/download/graph) — is guarded by `SDLogger::sdMutex()`, a second mutex independent of `dataMutex`. Routes that need the card for longer than a quick read (full-file download, graph decimation) hold `sdMutex_` for that whole operation; see the comments on `/api/logs/download` and `/api/logs/graph` in `WebDashboard.cpp` for the accepted tradeoff (the writer task drops samples it can't log during that window) and how each bounds its worst case. The CSV row layout is `Timestamp,PackV,PackI,SOC,MinCellV,MaxCellV,ReqI,Mode,ForceCharge,MaintenanceActive,GridPresent,Cell1..16,ChargeMOS,DischargeMOS,BmsProtection,CellOV1,CellOV2,PackOV1,PackOV2` — `readGraphSeries()` hardcodes extraction of raw columns 0-6 (`Timestamp`..`ReqI`) for the Graphs page, so any new telemetry field must be appended after the existing columns (currently after `Cell16`), never inserted earlier in the row.
- `include/WebPages.h` — dashboard, config, logs, and graphs pages' HTML/JS/CSS stored as `PROGMEM` string literals (`index_html`, `config_html`, `logs_html`, `graphs_html`); no separate frontend build step. The nav bar (markup + CSS) shared by all four pages is defined once as the `NAV_CSS`/`NAV_BAR` macros at the top of the file and spliced into each page literal via adjacent string-literal concatenation (compile-time, zero runtime cost) — edit those macros, not the individual pages, when changing navigation. The dashboard's live console (`#console`) seeds itself from the newest `.log` file's tail via `/api/logs/list` + `/api/logs/content` on page load (the SSE `log` channel has no replay/backlog, so without this a fresh page load shows nothing until the next event fires), then switches to the live SSE feed. `graphs_html` charts via Chart.js loaded from a CDN (browser-side only, no firmware cost) — keep each chart single-axis (see the `fix: split dual-axis...` commit) rather than reintroducing a dual-axis chart.
- `include/SystemState.h` — the two shared structs: `SystemConfig` (persisted to NVS; voltage setpoints, current limits, sample count, timeout) and `DashboardData` (live telemetry pushed to web UI and used for CAN TX; also carries the Daly MOSFET/alarm fields — `chargeMosOn`, `dischargeMosOn`, `bmsProtectionActive`, `cellOvervoltLevel1/2`, `packOvervoltLevel1/2`).
- `include/pin_config.h` — all hardware pin mappings for the LilyGO T-CAN485. The board requires the `5V_EN` pin driven high to power the RS485/CAN transceivers.
- `src/main.cpp` — wiring of the above: WiFi/NTP/OTA setup, the two-core task split, `netLog`, and the glideslope calculations.

### Logging
Use `netLog(fmt, ...)` (defined in `src/main.cpp`) for all application-level log output — it fans out to Serial, Telnet (`TelnetStream`, connect via `telnet <device-ip>`), the web UI's live console via SSE, and (via `SDLogger::logEvent`) the SD card's daily `.log` file, simultaneously. Don't use bare `Serial.print` for anything user-relevant — if a module can't reach `netLog` directly (e.g. `SDLogger` itself, to avoid a circular dependency on `main.cpp`), wire a `setDebugCallback`-style callback to it instead, matching the existing pattern in `DalyRS485`/`SMA_CAN`/`SDLogger`. For state that's polled continuously but only interesting on change (e.g. the Daly MOSFET/alarm status in `bmsTask`), log edge-triggered — only on transition, with a baseline flag so the first read after boot doesn't log a spurious "changed" event — rather than on every poll, so a stuck-on condition doesn't spam the log queue.

## Engineering conventions

- Prioritize technical accuracy and directness over conversational filler; if a proposed change is technically flawed, say so and explain why.
- Keep responses focused on code/logs/logic.
