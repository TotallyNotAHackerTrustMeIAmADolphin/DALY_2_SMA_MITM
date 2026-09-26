# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project overview

Dual-core ESP32 firmware (PlatformIO/Arduino) acting as a Man-In-The-Middle
bridge between a **Daly Smart BMS** (RS485) and an **SMA Sunny Island
Inverter** (CAN), on a LilyGO T-CAN485 board. Instead of trusting the BMS's
coarse SOC-based current limits, it computes smooth Charge/Discharge Current
Limits (the "Glideslope") from cell voltages and reports those to the
inverter over CAN, alongside a live web dashboard.

Controls high-power charging/discharging of a real battery pack.
`include/Glideslope.h`, `include/StatusFrame.h` and the SMA CAN protocol
(`include/SMAFrames.h`/`src/SMA_CAN.cpp`) are safety-critical — verify any
change against `test/test_glideslope/`, `test/test_statusframe/` and
`test/test_canpath/`.

## Build / test / flash runbook

- **Build:** `pio run`
- **First-time setup:** copy `include/secrets_example.h` to
  `include/secrets.h`, fill in `ssid`/`password` and the network block
  (`local_IP`, `gateway`, `subnet`, `primaryDNS`, `secondaryDNS`).
  Gitignored — never commit it or hardcode a network value into
  `src/main.cpp`/`platformio.ini` instead (`docs/adr/0002-...md`). Keep its
  globals `static` (a bare global has external linkage, fails to link a
  second TU).
- **Upload via USB:** `pio run -e lilygo-t-can485-usb -t upload` (esptool;
  `--upload-port /dev/ttyUSB0` to override autodetect). First flash, and a
  rollback-capable bootloader. Default env is OTA-only.
- **Upload via OTA:** `pio run -t upload`. `upload_port` comes from
  `include/secrets.h`'s `local_IP` via `scripts/extract_upload_ip.py` (a
  `pre:` build script) — `--upload-port <IP>` only to target a different
  device. Port 3232. Wait for `[SYS] Firmware confirmed ...` before the
  next OTA (see OTA rollback below).
- **Serial monitor:** `pio run -t monitor` (115200 baud)
- **Clean:** `pio run -t clean`
- **Unit tests:** `pio test -e native` (Unity, host-side). Device env has
  `test_ignore = *`: its upload protocol is `espota`, so a device-side
  `pio test` would OTA-flash the test firmware over the live bridge. Never
  run `pio test` without `-e native`.

## Hard rules

- Cross-task fields: guard with `dataMutex` via RAII `MutexLock`
  (`include/MutexLock.h`) + a named `k...LockTimeout`, never raw
  take/give; decide/copy under the lock, log after release.
- `netOutMutex` (SSE log + telemetry) is innermost — never take another
  mutex while holding it. `WifiEventLatch` (`include/WifiEvents.h`) is
  lock-free (atomics), the one documented exception to `dataMutex`.
- Nothing using async TCP (web server, SSE) runs before `setupNetwork()`
  returns.
- Glideslope math lives only in `include/Glideslope.h`; add a
  `test/test_glideslope/` case per change.
- Hard cutoff/alarm gate use **raw** cell voltage; the taper between them
  uses **smoothed** — never swap (`docs/adr/0001-...md`).
- Maintenance mode never bypasses the hard cutoff/alarm gate, only the
  taper/full-current branch.
- Every current limit goes through `toDeciAmps()`; CVL/DVL through
  `toDeciVolts()` — floor at 0, saturate at 65535, map NaN/negative to 0,
  never a bare `(uint16_t)` cast.
- Stale/missing/NaN cell-voltage or threshold data forces 0A
  (`Glideslope::isFresh`, via `decide()`).
- `cvMaxCharge`'s max is fixed at `kDalyOvervoltageV - kMinChargeMarginV`
  (3.550V) — never widen (`docs/adr/0001-...md`).
- No CAN frames until the BMS has delivered basic info + cell voltages at
  least once (`bmsLink.ready()`, #20).
- New `SystemConfig` setting = `Setting<T>` member + entry in `all()` +
  `!!LABEL_<key>!!`/`!!IN_<key>!!` in `config_html` (a native test checks
  both).
- Never pass a `Setting` to `netLog`/`logTo`/`printf` directly (varargs
  skip its `T` conversion) — `-Werror=format` makes that a compile error.
- Log a VALUE with `formatSettingValue()`, a LIMIT with
  `formatSettingFixed()` — never the reverse.
- Use `netLog(fmt, ...)`, or a `LogSink` wired to `netLogLine`
  (`include/LogSink.h`). Log edge-triggered, never periodic spam.
- CSV columns (`include/TelemetrySchema.h`) are always appended, never
  inserted earlier in the row.
- Keep `graphs_html` charts single-axis.
- `verifyRollbackLater()` must keep returning `true` (see OTA rollback).
- Secrets/network config live only in `include/secrets.h`.
- `ESPAsyncWebServer`/`AsyncTCP` are pinned exactly in `platformio.ini`;
  bump deliberately (#11, lwIP thread).

## Architecture

| Task/core | Responsibility |
|---|---|
| `bmsTask` (core 0) | Polls the Daly BMS over RS485 every ~2.4s, feeds `CellSmoother`, stores under `dataMutex`. |
| `canTask` (core 1) | Every 250ms: under `dataMutex`, builds a `Snapshot`, calls `decide()`, writes `Decision` back; **outside** the lock, `inverter.sendStatus()` if `sendFrames`, then logs one-shot events. Also CAN RX/bus health every 50ms. |
| `loop()` (core 1) | OTA, SD telemetry every 10s, boot diagnostics, 10-min health log. |
| `async_tcp` | Async Web Server + SSE. |
| SD writer task | Queue-fed CSV/log writer (`SDLogger`). |
| WiFi event task | Records transitions into `WifiEventLatch`; no logging/I/O itself. |

**Boot order:** SD → config (`ConfigStore::load()`) → BMS+CAN drivers and
both tasks → `setupNetwork()` (~15s max) → OTA, web server → `netReady`.
BMS/CAN start first so the SMA isn't left without frames while WiFi
connects.

**Shared state:** `currentData` (`DashboardData`), `cfg` (`SystemConfig`),
`bmsLink` and `uiCommands` (written by `handleUIAction`, read by
`canTask`) are guarded by `dataMutex`. `WifiEventLatch` is the lock-free
exception.

## Module map

- `Glideslope.h` — CCL/DCL/isFresh/spreadFactor/toDeciAmps/toDeciVolts — `test_glideslope/`
- `StatusFrame.h` — `decide()` -> `Decision`, per-tick `canTask` logic — `test_statusframe/`, e2e `test_canpath/`
- `DalyFrames.h`+`DalyRS485.cpp` — Daly RS485, pure parser + UART adapter — `test_dalyframes/`
- `SMAFrames.h`+`SMA_CAN.cpp` — SMA/Victron CAN, pure codec + adapter — `test_smaframes/`
- `CellSmoother.h` — per-cell moving average, window `cfg.vSamples` (default 12) — `test_cellsmoother/`
- `BmsEvents.h` — `bmsTask`'s edge-triggered SOC/MOSFET/alarm detection — `test_bmsevents/`
- `RollbackConfirm.h` — OTA rollback decision behind `confirmImageIfReady()` — `test_rollbackconfirm/`
- `HealthLog.h` — `decide() -> Decision{reason, task}` for the health log — `test_healthlog/`
- `LocalClock.h` — the one wall-clock-valid check, used by `netLog()`, NTP wait, `SDLogger` — `test_localclock/`
- `WifiEvents.h` — `WifiEventLatch`: WiFi event → `[WIFI]` log-line state machine — `test_wifievents/`
- `ConfigForm.h` — pure `/save` parse/validate + change-log formatting — `test_configform/`
- `ConfigStore.h`+`.cpp` — NVS load/store; rejects a wrong-typed/out-of-range value, logs the default used instead
- `SettingFormat.h` — `formatSettingFixed()` (decimals, a LIMIT) / `formatSettingValue()` (round-trip, a VALUE) — `test_settingformat/`
- `SystemConfig.h` — each member a `Setting<T>`; `set()` range-gates every write; `validate()` checks two-setting rules — `test_systemconfig/`
- `DashboardData.h` — live telemetry for the web UI/CAN TX plus Daly MOSFET/alarm fields, Arduino-free
- `TelemetrySchema.h` — ordered CSV column table — `test_telemetryschema/`
- `TelemetryJson.h` — SSE JSON via `BoundedWriter`, capped at `kPackCells` — `test_telemetryjson/`
- `SDLogger.cpp` — SD writer task; access via `sdMutex_`; routes use `webLockTimeout()` (0 if already held); decimation/trim in `CsvDecimation.h`/`TailTrim.h`
- `WebDashboard.cpp` — `ESPAsyncWebServer` routes, SSE, `/toggleMaint`/`/resetSMA`, `/api/logs/*` (guarded by `findLogFile()`); `saveConfig()` uses `ConfigForm`/`ConfigStore`
- `Diagnostics.cpp` — boot diagnostics, health log, OTA rollback confirmation, coredump routes (below)
- `pin_config.h` — LilyGO T-CAN485 pin map (`5V_EN` driven high for RS485/CAN)
- `main.cpp` — wiring: WiFi/NTP/OTA setup, two-core task split, `netLog`, mutex timeouts

## Diagnostics runbook

- **Boot:** `logBootDiagnostics()` runs right after the SD logger starts,
  before the tasks — logs `esp_reset_reason()`, running ELF sha256, OTA
  partition/state, any rollback notice, core dump summary.
- **Core dumps:** `GET /api/coredump/summary` (JSON) then `GET
  /api/coredump` (raw) — summary must register first: AsyncURIMatcher's
  plain-string match also matches `"<uri>/..."` and would swallow it
  otherwise. Decode with the matching `firmware.elf`: `espcoredump.py
  info_corefile -t raw -c coredump.bin firmware.elf`. 404 = no dump, 409 =
  fails CRC. Keep every deployed build's `firmware.elf`.
- **OTA rollback:** see `docs/adr/0002-...md`. Logs `[SYS] Firmware
  confirmed ...` / `NOT confirmed ...`.
- **Health:** every 10 min, `logHealth()` samples heap, per-task stack
  (`loop`/`BMS_Task`/`CAN_Task`/`SD_LogTask`/`async_tcp`/`arduino_events`),
  SD drop/failure counters, WiFi RSSI — logs only when `HealthLog::decide()`
  returns a reason: boot baseline, `STACK LOW` (<512B), `TASK GONE`, a
  stack/heap/largest-block regression past threshold, `SD DROPS` (any
  increase), or a 24h heartbeat. Never reintroduce an unconditional
  periodic line.
- **WiFi:** `wifiEventHandler()` only records transitions into
  `WifiEventLatch`; `drainWifiEvents()` in `loop()` emits `[WIFI]
  Disconnected`/`Reconnected`/`Lost IP address`, edge-triggered.

## Engineering conventions

- Prioritize technical accuracy and directness over conversational filler;
  if a proposed change is technically flawed, say so and explain why.
- Keep responses focused on code/logs/logic.

### Model split: plan and review with the expensive model, delegate grunt work to cheap models

- **Planning, briefs, review: the most capable model** (the main session,
  currently Fable-class). Investigation, root-causing, deciding what to
  change, writing the worker briefs, and reviewing the workers' diffs before
  anything is committed all stay here. Don't delegate judgment.
- **Grunt work: cheap subagents** (Sonnet-class, `model: "sonnet"`).
  Well-specified edits, builds, running tests, scraping logs, mechanical
  verification, worktree builds of each PR tip. Dispatch independent
  workers in parallel, in one message.
- **Review: the capable model again.** Read the worker's diff yourself (or
  spawn `model: "fable"` reviewers by angle for a big diff) and verify each
  finding against the code before acting on it. Workers report; they don't
  self-approve.
- **Every brief is self-contained.** The worker has no conversation
  context: give it the repo path, the exact scope, the required report
  format, and the hard rules verbatim - never `pio run -t upload`, never
  `pio test` without `-e native` (the device env OTA-flashes the live
  bridge), no commits or pushes unless the brief says so, don't touch
  `include/secrets.h` contents.
- Firmware flashes are never delegated and never autonomous: the main
  session asks the user before each OTA upload.
- **Code review before merge: the `mattpocock-skills:code-review` skill**
  (`/code-review <fixed-point>`, e.g. `main`). It runs two parallel
  sub-agents and reports them side by side: *Standards* (this file's
  conventions plus its Fowler smell baseline) and *Spec* (does the diff do
  what the referenced issues asked; issues resolve via
  `docs/agents/issue-tracker.md`, so reference them in commit messages). For
  firmware diffs that touch tasks, mutexes, the glideslope or CAN output,
  add expensive-model angle reviewers on top (concurrency, safety-critical
  behaviour, web/diagnostics), since the two axes don't cover runtime
  correctness. Verify every finding against the code before writing a fix
  brief.

## Agent skills

### Issue tracker

Issues live in this repo's GitHub Issues, via the `gh` CLI. See
`docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary (`needs-triage`, `needs-info`,
`ready-for-agent`, `ready-for-human`, `wontfix`). See
`docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See
`docs/agents/domain.md`.
