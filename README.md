

***

# 🔋 SMA Sunny Island & Daly BMS: Active CAN Bridge (Pro-Edition)

An industrial-grade, dual-core ESP32 appliance designed to bridge a **Daly Smart BMS (RS485)** and an **SMA Sunny Island Cluster (CAN)**. 

Unlike standard bridges that rely on the BMS's often inaccurate Pack SOC calculation, this bridge fetches **high-resolution individual cell voltages** and uses them to drive a real-time, mathematically smoothed **Charge/Discharge Glideslope**. 

Built for the **LilyGO T-CAN485** board, this firmware utilizes true FreeRTOS multithreading to ensure a lightning-fast Web UI, rock-solid 250ms CAN heartbeats, and uninterrupted background RS485 polling.

---

## ✨ Key Features

*   **⚡ Cell-Driven Glideslope:** Calculates Charge Current Limits (CCL) and Discharge Current Limits (DCL) based strictly on the **Highest** and **Lowest** individual cells, instantly throttling the inverter if a single cell runs away.
*   **🧠 Dual-Core RTOS Architecture:** Core 0 handles the slow RS485 BMS serial polling. Core 1 strictly handles the high-speed SMA CAN protocol and Async Web Server. Zero lag, zero dropped CAN frames.
*   **🌐 Modern Web Dashboard:** View live pack stats, SMA operating states, and a dynamic 16-cell voltage grid (with the highest and lowest cells auto-highlighted) from any browser.
*   **❄️ Winter Maintenance Mode:** Automatically detects dangerously low cell voltages during dark winter weeks and spoofs a 2% SOC to force the SMA Sunny Island to pull from the grid.
*   **🛡️ "Nuclear" Bus-Off Recovery:** Includes deep ESP-IDF workarounds. If the CAN cable is unplugged, the system gracefully suspends the driver instead of crashing, and auto-recovers the second the cable is reattached.
*   **📡 Remote Telnet Logging:** Stream live diagnostic data and state changes directly to your terminal.

---

## 🛠️ Hardware Requirements

1.  **Microcontroller:** [LilyGO T-CAN485](https://github.com/Xinyuan-LilyGO/T-CAN485) (ESP32 WROVER/WROOM based).
2.  **Inverter:** SMA Sunny Island (Tested on 8.0H-13 Clusters).
3.  **Battery/BMS:** Daly Smart BMS with RS485 / UART output.

### 🔌 Wiring Guide

**1. BMS RS485 (LilyGO Green Terminal -> Daly)**
*   `A` -> Daly RS485 `A`
*   `B` -> Daly RS485 `B`
*   `GND` -> Daly `GND`

**2. SMA CAN (LilyGO Green Terminal -> SMA RJ45)**
*   `H` -> SMA Pin 4 (CAN High)
*   `L` -> SMA Pin 5 (CAN Low)
*   `GND` -> SMA Pin 3 (CAN GND)
> ⚠️ **CRITICAL:** You *must* connect the CAN Ground. The LilyGO uses an isolated CAN transceiver. Without a shared ground, the signals will float and the SMA will reject the data. Also, ensure the **120-ohm DIP switch** on the LilyGO board is turned **ON**.

---

## 🚀 Installation & Setup

This project is built using [PlatformIO](https://platformio.org/).

### 1. Configure Wi-Fi Credentials
For security, Wi-Fi passwords are intentionally excluded from the codebase.
1. Navigate to the `include/` directory.
2. Rename `secrets_example.h` to `secrets.h`.
3. Open `secrets.h` and enter your Wi-Fi SSID and Password.

### 2. Set your Static IP (Optional but recommended)
Open `src/main.cpp` and adjust the IP address block at the top of the file to match your local network setup.

### 3. Flash the Board
Plug the LilyGO T-CAN485 into your computer via USB. Open PlatformIO and hit **Upload**. 
*(Note: Subsequent updates can be flashed Over-The-Air (OTA) without plugging in the device).*

---

## 🎛️ The Glideslope Philosophy

Many standard BMS units report SOC in rigid 1% steps, which causes the inverter to suddenly jerk its current limits by 20A-50A at a time. This bridge completely ignores the BMS's SOC for control logic.

Instead, you define **Cell Voltage Setpoints** in the Web UI:
1.  **Bulk Phase:** Full speed charging (`Max Charge Amps`) until the highest cell hits the **Start Taper Vpc**.
2.  **Taper Phase:** As the highest cell voltage continues to rise, the bridge linearly reduces the current limit, drawing a smooth downward slope.
3.  **Trickle Phase:** Once **Target Trickle Vpc** is reached, current locks to **Trickle Amps** (e.g., 2.0A) to hold the pack steady while passive balancers burn off the top cells.
4.  **Brick Wall:** At **Max Charge Vpc**, the bridge requests 0A.

The exact same math applies in reverse for discharging, using the **Lowest Cell** to smoothly taper household loads to 0A before the BMS is forced to trip its hardware emergency disconnect.

---

## 🖥️ Using the Web Dashboard

Once booted, navigate to the device's IP address in your web browser (e.g., `http://192.168.178.56`).

*   **Dashboard Tab:** View live telemetry, the 16-cell grid, and the live SMA Inverter State.
    *   *SMA States:* `INIT`, `STARTUP`, `STANDBY`, `RUNNING`, `EMERGENCY` (Normal during glideslope limiting), `FAULT`.
*   **Configuration Tab:** Adjust your glideslope voltage targets and current limits. Hitting "Save" instantly updates the running math and writes the values to the ESP32's non-volatile storage (NVS).

---

## 👨‍💻 Diagnostics & Telnet

If you need to debug the system, open a terminal and connect via Telnet:
```bash
telnet 192.168.178.56
```
You will see a live feed of configuration changes, inverter state shifts, and any RS485/CAN hardware fault recoveries.

---

## ⚠️ Disclaimer
*This software interacts with high-energy battery systems and expensive inverter equipment. It is provided "as-is" without warranty of any kind. The authors are not responsible for any hardware damage, battery degradation, or fire hazards. Always test with conservative voltage setpoints during initial commissioning!*