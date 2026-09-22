# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Dual-core ESP32 firmware (PlatformIO/Arduino framework) that acts as a Man-In-The-Middle bridge between a **Daly Smart BMS** (RS485) and an **SMA Sunny Island Inverter** (CAN). Target hardware is the LilyGO T-CAN485 board. Instead of trusting the BMS's coarse SOC-based current limits, it computes smooth Charge/Discharge Current Limits (the "Glideslope") directly from individual cell voltages and reports those to the inverter over CAN, along with a live web dashboard.

This firmware controls high-power charging/discharging of a real battery pack. Treat `calculateCCL`/`calculateDCL` in `src/main.cpp` and the CAN protocol implementation as safety-critical — verify glideslope math carefully on any change.

## Build & Run (PlatformIO)

- **Build:** `pio run`
- **Upload via USB:** `pio run -t upload` (platformio.ini defaults to OTA upload; override `--upload-port` for a USB serial port)
- **Upload via OTA:** `pio run -t upload` (`upload_port` is derived automatically from `include/secrets.h`'s `local_IP` by `scripts/extract_upload_ip.py`, a PlatformIO `pre:` build script — pass `--upload-port <DEVICE_IP>` only to override it, e.g. targeting a different device); OTA port is 3232
- **Serial monitor:** `pio run -t monitor` (115200 baud)
- **Clean:** `pio run -t clean`
- **Run unit tests:** `pio test -e native` (Unity, runs on the host). The device env has `test_ignore = *` on purpose: with `espota` as its upload protocol, a device-side `pio test` would OTA-flash the test firmware over the live bridge.

### First-time setup
1. Copy `include/secrets_example.h` to `include/secrets.h` and fill in `ssid`/`password` **and** the network block (`local_IP`, `gateway`, `subnet`, `primaryDNS`, `secondaryDNS`) to match your network. This file is gitignored — never commit real credentials or your network layout. All network config lives in this one file now; don't add new hardcoded IPs back into `src/main.cpp` or `platformio.ini`.

## Architecture

### Dual-core FreeRTOS split
- **Core 0 (`bmsTask` in `src/main.cpp`):** Polls the Daly BMS over RS485 every ~2.4s (basic info + per-cell voltages + MOSFET/alarm status), applies a per-cell moving-average smoothing filter (`cellBuffers`, window size `cfg.vSamples`, seeded from the first real reading), and writes results into the shared `currentData`.
- **Core 1 (`canTask`):** Drives the SMA CAN heartbeat (every 250ms), reads SMA frames and handles CAN bus health / bus-off recovery. It sends **nothing** until the BMS has delivered basic info and cell voltages once (`haveBasicInfo && haveCellData`), so no placeholder values ever reach the SMA.
- **Core 1 (Arduino `loop()`):** OTA handling, SD telemetry every 10s (also gated on real BMS data), the one-time boot diagnostics and the 10-minute health log. The Async Web Server / SSE run on their own task (`async_tcp`) via `ESPAsyncWebServer`.
- **Boot order (`setup()`):** config (NVS) and SD → BMS + CAN drivers and both tasks → `setupNetwork()` (blocks up to ~15s for WiFi + NTP) → OTA, Telnet, web server → `netReady = true`. BMS/CAN deliberately start before the network so the SMA isn't left without frames while WiFi connects. Nothing that uses the async TCP stack (web server, Telnet, SSE) may run before `setupNetwork()`: `WebDashboard::loadConfig()` is split from `WebDashboard::begin()` for that reason.
- **Shared state:** `currentData` (`DashboardData`, `include/SystemState.h`), `cfg` (`SystemConfig`, `include/SystemConfig.h`) and the BMS freshness flags (`haveBasicInfo`/`haveCellData`, `lastBasicInfoRead`/`lastCellRead`) are shared between tasks and guarded by `dataMutex` (a `SemaphoreHandle_t`). Any new cross-task field must be read/written inside `xSemaphoreTake(dataMutex, ...)` / `xSemaphoreGive(dataMutex)`, following the existing pattern in `bmsTask` and `canTask`.
- **Network output lock:** `netOutMutex` serializes every call into TelnetStream and the SSE event source (`netLog`'s network sinks and `pushTelemetry`). Neither library is safe to call from several tasks at once. It is the innermost lock: never take another mutex while holding it.

### Glideslope current-limit logic
The math lives in `include/Glideslope.h` (`Glideslope::calculateCCL`/`calculateDCL`/`isFresh`): pure functions with no Arduino/FreeRTOS dependencies. `calculateCCL()`/`calculateDCL()` in `src/main.cpp` are thin wrappers that pass in `cfg`, the voltage, `bmsDataFresh()` and the maintenance flag (caller holds `dataMutex`). The core control algorithm:
- Below `cvStartTaper`/above `cvStartDTaper`: full `maxChargeA`/`maxDischargeA`.
- Between the start-taper and alarm-gate voltages: linear interpolation down to trickle/limp current.
- At/beyond the alarm gate: fixed trickle (`trickleA`) or limp (`limpDischargeA`) current.
- At/beyond the hard max/min voltage: 0A.
- Stale or missing BMS data forces 0A as a fail-safe, checked first in both functions. `bmsDataFresh()` requires **both** basic info and cell voltages to have been read at least once and within `cfg.bmsTimeout` seconds. "Never read" counts as stale (before, `lastSuccessfulBmsRead == 0` let full limits through for the first 60s after boot).
- Maintenance mode (`currentData.maintenanceActive`, driven by either manual UI toggle or automatic low-voltage winter maintenance detection in `loop()`) overrides normal taper behavior.

`test/test_glideslope/test_glideslope.cpp` includes the real `Glideslope.h` and runs natively (`pio test -e native`). Change the math only in `Glideslope.h` and add a test case with it.

### Module layout
- `src/DalyRS485.cpp` / `include/DalyRS485.h` — Daly BMS RS485 protocol (9600 baud): basic pack info + per-cell voltage reads (`0x90`/`0x95`), plus the BMS's own hardware protection state via `readMosfetStatus()` (`0x93`: charge/discharge MOSFET on/off) and `readAlarmStatus()` (`0x98`: cell/pack overvoltage alarm bits) — polled each `bmsTask` cycle, independent of `calculateCCL`/`calculateDCL`, so a BMS-initiated cutoff is visible even if our own glideslope never would have tripped. Both parse defensively (reject the frame rather than trust an out-of-range byte) since the exact `0x93`/`0x98` byte layout is from the documented Daly protocol family, not verified against every pack's firmware.
- `src/SMA_CAN.cpp` / `include/SMA_CAN.h` — SMA/Victron CAN protocol (500kbps), status frame TX (CCL/DCL/CVL/DVL, SOC, etc.), bus-off detection and recovery ("nuclear" driver suspend/resume if the CAN cable is unplugged and reattached).
- `src/WebDashboard.cpp` / `include/WebDashboard.h` — `ESPAsyncWebServer` routes, Server-Sent Events for live telemetry push, the UI action callback (`handleUIAction` in `main.cpp`) for toggles like manual maintenance force and SMA cluster reset, and the `/api/logs/*` routes backing the Logs/Graphs pages (list, tail-view, full-file download, decimated graph series) — see `findLogFile()` for the shared filename-whitelist/path-traversal guard these all use.
- `src/SDLogger.cpp` / `include/SDLogger.h` — background SD-card logger: a queue-fed FreeRTOS task writes BMS/SMA telemetry to daily-rotated CSV files and system events to a parallel `.log` file. All direct (non-queued) SD/SPI access — the writer task's own file writes, and every web-route read (list/tail/download/graph) — is guarded by `SDLogger::sdMutex()`, a second mutex independent of `dataMutex`. Routes that need the card for longer than a quick read (full-file download, graph decimation) hold `sdMutex_` for that whole operation; see the comments on `/api/logs/download` and `/api/logs/graph` in `WebDashboard.cpp` for the accepted tradeoff (the writer task drops samples it can't log during that window) and how each bounds its worst case. The CSV row layout is `Timestamp,PackV,PackI,SOC,MinCellV,MaxCellV,ReqI,Mode,ForceCharge,MaintenanceActive,GridPresent,Cell1..16,ChargeMOS,DischargeMOS,BmsProtection,CellOV1,CellOV2,PackOV1,PackOV2` — `readGraphSeries()` hardcodes extraction of raw columns 0-6 (`Timestamp`..`ReqI`) for the Graphs page, so any new telemetry field must be appended after the existing columns (currently after `Cell16`), never inserted earlier in the row.
- `include/WebPages.h` — dashboard, config, logs, and graphs pages' HTML/JS/CSS stored as `PROGMEM` string literals (`index_html`, `config_html`, `logs_html`, `graphs_html`); no separate frontend build step. The nav bar (markup + CSS) shared by all four pages is defined once as the `NAV_CSS`/`NAV_BAR` macros at the top of the file and spliced into each page literal via adjacent string-literal concatenation (compile-time, zero runtime cost) — edit those macros, not the individual pages, when changing navigation. The dashboard's live console (`#console`) seeds itself from the newest `.log` file's tail via `/api/logs/list` + `/api/logs/content` on page load (the SSE `log` channel has no replay/backlog, so without this a fresh page load shows nothing until the next event fires), then switches to the live SSE feed. `graphs_html` charts via Chart.js loaded from a CDN (browser-side only, no firmware cost) — keep each chart single-axis (see the `fix: split dual-axis...` commit) rather than reintroducing a dual-axis chart.
- `include/SystemState.h` — the two shared structs: `SystemConfig` (persisted to NVS; voltage setpoints, current limits, sample count, timeout) and `DashboardData` (live telemetry pushed to web UI and used for CAN TX; also carries the Daly MOSFET/alarm fields — `chargeMosOn`, `dischargeMosOn`, `bmsProtectionActive`, `cellOvervoltLevel1/2`, `packOvervoltLevel1/2`).
- `include/pin_config.h` — all hardware pin mappings for the LilyGO T-CAN485. The board requires the `5V_EN` pin driven high to power the RS485/CAN transceivers.
- `src/main.cpp` — wiring of the above: WiFi/NTP/OTA setup, the two-core task split, `netLog`, and the glideslope calculations.

### Diagnostics
- **Boot:** once NTP has set the clock (or after 60s), `loop()` calls `logBootDiagnostics()`. It logs `esp_reset_reason()` (PANIC / task WDT / BROWNOUT / software…), the running firmware's ELF sha256, and a summary of the core dump stored in flash (crashing task, PC, backtrace, and the crashing firmware's ELF sha).
- **Core dumps:** this Arduino core's prebuilt sdkconfig writes an ELF core dump to the `coredump` flash partition on every panic, and OTA doesn't erase it. `GET /api/coredump` streams it; decode it with the **matching** `firmware.elf`: `espcoredump.py info_corefile -t raw -c coredump.bin firmware.elf`. A dump stays until the next crash overwrites it, so compare its ELF sha with the running one. Keep the `firmware.elf` of every build you deploy.
- **Health:** every 10 min `logHealth()` logs free / minimum / largest-block heap and the remaining stack (bytes) of `loop`, `BMS_Task`, `CAN_Task`, `SD_LogTask` and `async_tcp`.

### Logging
Use `netLog(fmt, ...)` (defined in `src/main.cpp`) for all application-level log output — it fans out to Serial, Telnet (`TelnetStream`, connect via `telnet <device-ip>`), the web UI's live console via SSE, and (via `SDLogger::logEvent`) the SD card's daily `.log` file, simultaneously. Don't use bare `Serial.print` for anything user-relevant — if a module can't reach `netLog` directly (e.g. `SDLogger` itself, to avoid a circular dependency on `main.cpp`), wire a `setDebugCallback`-style callback to it instead, matching the existing pattern in `DalyRS485`/`SMA_CAN`/`SDLogger`. For state that's polled continuously but only interesting on change (e.g. the Daly MOSFET/alarm status in `bmsTask`), log edge-triggered — only on transition, with a baseline flag so the first read after boot doesn't log a spurious "changed" event — rather than on every poll, so a stuck-on condition doesn't spam the log queue.

## Engineering conventions

- Prioritize technical accuracy and directness over conversational filler; if a proposed change is technically flawed, say so and explain why.
- Keep responses focused on code/logs/logic.

## Agent skills

### Issue tracker

Issues live in this repo's GitHub Issues, via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.
