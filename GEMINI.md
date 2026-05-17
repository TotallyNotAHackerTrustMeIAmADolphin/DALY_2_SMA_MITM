# 🔋 DALY to SMA MITM Bridge (GEMINI.md)

This project is a high-performance, dual-core ESP32 firmware designed to act as a Man-In-The-Middle (MITM) bridge between a **Daly Smart BMS** (RS485) and an **SMA Sunny Island Inverter** (CAN). It ensures battery safety and longevity by calculating dynamic current limits based on individual cell voltages.

## 🏗️ Project Overview

- **Hardware:** LilyGO T-CAN485 (ESP32-WROVER/WROOM).
- **Core Architecture:**
  - **Core 0 (BMS Task):** Handles slow RS485 polling of the Daly BMS (every 2 seconds).
  - **Core 1 (Main Loop):** Handles high-speed CAN communication (250ms heartbeat), OTA updates, and the Async Web Server.
- **Key Logic:** The "Glideslope" algorithm. It calculates Charge Current Limits (CCL) and Discharge Current Limits (DCL) by looking at the highest and lowest individual cell voltages, providing much smoother control than the BMS's standard SOC-based reporting.
- **Web UI:** A modern, mobile-responsive dashboard for live telemetry and real-time configuration of voltage setpoints.
- **Logging:** Unified logging system via `netLog` that broadcasts to Serial, Telnet (`TelnetStream`), and the Web UI console.

## 🚀 Building and Running

This project uses **PlatformIO**.

### Prerequisites
1.  **Configure Secrets:**
    - Copy `include/secrets_example.h` to `include/secrets.h`.
    - Fill in your `ssid` and `password`.
2.  **Static IP:**
    - The device is configured with a static IP (`192.168.178.56` by default) in `src/main.cpp`. Adjust this and the gateway/subnet if necessary.

### Key Commands
- **Build:** `pio run`
- **Upload (USB):** `pio run -t upload` (Note: `platformio.ini` defaults to OTA; you may need to override the upload port for USB).
- **Upload (OTA):** `pio run -t upload --upload-port <DEVICE_IP>`
- **Serial Monitor:** `pio run -t monitor`
- **Clean:** `pio run -t clean`

## 🛠️ Development Conventions

### Hardware & Pins
- Hardware pin mappings are strictly defined in `include/pin_config.h`.
- The LilyGO board requires the `5V_EN` pin to be high to power the RS485/CAN transceivers.

### CLI Operational Guidelines
- **Self-Terminating Commands:** Always prefer commands that terminate autonomously. Use count flags (e.g., `ping -c 3`) or the `timeout` utility to prevent background processes from hanging indefinitely.
- **Unified Logging:** Use the `netLog` function for all application-level logging to ensure output is broadcast across Serial, Telnet, and the Web UI.

### State Management
- Shared data structures are defined in `include/SystemState.h`:
  - `SystemConfig`: Configuration values (voltages, current limits) saved in NVS.
  - `DashboardData`: Real-time telemetry and state.
- **Thread Safety:** While shared directly, the BMS task updates `currentData` on Core 0 while the Main Loop reads it on Core 1. Care should be taken with complex objects, though the current usage is largely primitive values or fixed-size vectors.

### Web Dashboard
- Web pages (`index_html`, `config_html`) are stored as `PROGMEM` strings in `include/WebPages.h`.
- `WebDashboard.cpp` handles the `ESPAsyncWebServer` routes and Server-Sent Events (SSE) for real-time updates.

### Communication Protocols
- **Daly RS485:** Implementation in `src/DalyRS485.cpp`. Uses standard 9600 baud serial.
- **SMA CAN:** Implementation in `src/SMA_CAN.cpp`. Adheres to the SMA/Victron CAN protocol (250kbps, specific PGNs).

## ⚠️ Critical Safety Note
This firmware controls high-power charging/discharging. Always verify the `calculateCCL` and `calculateDCL` logic in `src/main.cpp` when making changes to the glideslope math.
