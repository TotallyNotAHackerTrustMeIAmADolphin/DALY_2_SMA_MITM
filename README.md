# SMA Sunny Island & Daly BMS: Active CAN Bridge (Pro-Edition)

An ESP32-S3 based "Man-in-the-Middle" CAN gateway designed to integrate a **Daly Smart BMS** (500A Discharge / 250A Charge) with an **SMA Sunny Island 3-Phase Cluster**.

This bridge solves the stability issues common in DIY LiFePO4 storage systems by intercepting raw BMS data and translating it into a smooth, professional-grade control signal that the SMA cluster understands.

## 1. The Problem
Standard Daly-to-SMA communication often fails in high-capacity (840Ah+) systems because:
*   **Unrealistic Demands:** The Daly BMS requests up to 600A, exceeding safe hardware limits.
*   **Hard Cut-offs:** Sudden 0A requests at 100% SoC cause DC induction spikes, triggering "Measurement Range Violations" on the SMA.
*   **Latency:** The Daly CAN interface is often too slow for the SMA's 1000ms timeout watchdog.
*   **Cell Imbalance:** Sudden disconnects prevent passive balancers from finishing their work.

## 2. Pro-Edition Features
*   **Web Dashboard:** Live browser-based telemetry with real-time graphs for Voltage and Current.
*   **Asynchronous Heartbeat:** Feeds the SMA a steady 250ms signal, eliminating "Battery Timeout" errors.
*   **Intelligent Tapering:** Smoothly glides current down to 2A (Charge) or 10A (Discharge) at the voltage limits.
*   **Cell Runner Protection:** Automatically detects a single high cell (via bitmask `0x04`) and drops charging to **1.0A** for balancing.
*   **Voltage Smoothing:** Uses a Moving Average Filter (10 samples) to eliminate DC bus noise.
*   **High-Power Mapping:** Supports separate caps for Charge (**250A**) and Discharge (**500A**).

---

## 3. Hardware Requirements
*   **LilyGO T-2CAN** (ESP32-S3 with dual isolated CAN ports).
*   **Daly Smart BMS** (Tested with 500A/250A model + WNT Board).
*   **SMA Sunny Island Cluster** (3-Phase, connected to Master L1).
*   **2x RJ45 Ethernet Cables** (Stripped at one end).

---

## 4. Wiring Diagram

The LilyGO T-2CAN uses isolated transceivers. **You must connect the SGND (Signal Ground) terminals.**

### **A. Daly WNT to LilyGO (Port B - Internal)**
| RJ45 Pin | Wire Color (Typical) | Signal | LilyGO Terminal |
| :--- | :--- | :--- | :--- |
| **3** | White/Green | **GND** | **SGNDB** |
| **4** | Blue | **CAN High** | **CANHB** |
| **5** | White/Blue | **CAN Low** | **CANLB** |

### **B. LilyGO to SMA Master L1 (Port A - MCP2515)**
| RJ45 Pin | Wire Color (Typical) | Signal | LilyGO Terminal |
| :--- | :--- | :--- | :--- |
| **3** | White/Green | **GND** | **SGNDA** |
| **4** | Blue | **CAN High** | **CANHA** |
| **5** | White/Blue | **CAN Low** | **CANLA** |

> [!WARNING]  
> **Phase Sync Cables:** SMA Sunny Island clusters use Pins 1, 3, and 6 for phase synchronization between inverters. If your cable is non-standard, ensure the ground is on Pin 2 or 3 as per your specific testing, but **never** ground a pin that carries a sync signal.

---

## 5. Software Configuration

### **PlatformIO Constants**
Edit `main.cpp` to set your specific bank limits:
```cpp
const float MAX_CHARGE_LIMIT_A    = 250.0; // 250A Charge Cap
const float MAX_DISCHARGE_LIMIT_A = 500.0; // 500A Discharge Cap
const float V_MAX_CHARGE          = 55.20; // 3.45V/cell Hard Stop
const float V_START_C_TAPER       = 54.00; // Charge slowing start
const float V_MIN_DISCHARGE       = 48.00; // 3.0V/cell Cutoff
```

---

## 6. Monitoring & Interfaces

### **Web Dashboard (Port 80)**
Access live telemetry from any device on your WiFi at `http://192.168.178.55`.
*   **Live Charts:** Real-time Voltage/Current tracking via Chart.js.
*   **Web Console:** View system logic and state changes directly in the browser.

### **Remote Logging (Port 2323)**
The system streams raw status data to a TCP port. Use the provided Python script to capture these logs with timestamps:
```bash
python3 log_bms.py
```
**Log Format:**
`[STATUS] Mode:TAPER_CHG V:54.60 I:45.2 SOC:96 CCL:125.0 DCL:500.0`

### **Wireless Updates (OTA)**
The bridge remains in the basement. Flash new code wirelessly via PlatformIO:
```ini
upload_protocol = espota
upload_port = 192.168.178.55
```

---

## 7. Safety & Watchdogs
*   **BMS Watchdog:** If the bridge loses BMS data for >15s, it tells the SMA to stop all current.
*   **Handshake Emulation:** Sends the `"SMA     "` manufacturer string (ID `0x35E`) to ensure the Inverter trusts the data.
*   **Alarm Bitmasking:** Specifically looks for the "Cell Over Voltage" bit (Bit 2) while ignoring normal status bits (`0x28`).

---
*Disclaimer: This project involves high-current electricity (500A). Ensure all 13mm DC terminals on the SMA units are torqued correctly and your fuses are rated for the loads described.*