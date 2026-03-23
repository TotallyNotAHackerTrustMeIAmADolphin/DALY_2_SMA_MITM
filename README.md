# SMA Sunny Island & Daly BMS: Active CAN Bridge (Pro-Edition)

An ESP32-S3 based "Man-in-the-Middle" CAN gateway designed to perfectly integrate a **Daly Smart BMS** (500A Discharge / 250A Charge) with an **SMA Sunny Island 3-Phase Cluster**.

## 1. The Problem
Standard Daly-to-SMA communication often results in "Measurement Range Violations" and system crashes because:
*   **Unrealistic Demands:** The Daly BMS requests up to 600A, exceeding hardware limits.
*   **Hard Cut-offs:** Sudden 0A requests at 100% SoC cause DC induction spikes and inverter errors.
*   **Latency:** The Daly WNT board is often too slow for the SMA's 1000ms timeout watchdog.
*   **False Alarms:** Generic protocol translation can trigger false "Cell Runner" alarms at low voltages.

## 2. Pro-Edition Solution
This bridge transforms the raw BMS data into a professional-grade control signal:
*   **Linear Tapering:** Smoothly glides current down to 2A at the top and 10A at the bottom.
*   **Cell Runner Protection:** Detects specific BMS alarm bits and throttles charging to **1.0A** to allow passive balancing without tripping the inverter.
*   **Moving Average Filter:** Smooths DC bus voltage noise over 10 samples for rock-solid stability.
*   **High-Speed Heartbeat:** Feeds the SMA a steady 250ms signal, eliminating all "Battery Timeout" beeps.

---

## 3. Hardware Requirements
*   **LilyGO T-2CAN** (Dual isolated CAN ports).
*   **Daly Smart BMS** (Tested with 500A/250A model + WNT Board).
*   **SMA Sunny Island Cluster** (Compatible with 4.5, 6.0, and 8.0 models).
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

## 5. Software Features

### **Intelligent Current Limits**
The bridge applies separate caps for charging and discharging:
*   **Charge:** Max 250.0A (Tapers from 54.0V to 55.2V).
*   **Discharge:** Max 500.0A (Tapers from 49.6V to 48.0V).

### **Cell Runner Bitmasking**
The code ignores Daly's "Status OK" bits (0x28) and only engages the **1.0A Balancing Mode** if:
1.  **Bit 2 (0x04)** of the Alarm/Warning byte is set.
2.  Total battery voltage is above **53.5V**.

---

## 6. Installation & Updates

### **Wireless Updates (OTA)**
Update settings (like voltage thresholds) wirelessly from your desk. In your `platformio.ini`:
```ini
upload_protocol = espota
upload_port = 192.168.178.55
```

### **Live Dashboard (Telnet)**
Monitor the system in real-time from your Linux terminal:
```bash
telnet 192.168.178.55
```

**Dashboard View:**
```text
--- SMA-DALY PRO DASHBOARD ---
BATT: 54.15V | 12.4A | 94% SOC
LIMITS: CCL: 165.0A | DCL: 500.0A
STATUS: [Charge Tapering active]
------------------------------
```

---

## 7. Python Logger
A script is provided to capture the Telnet output and save it with local timestamps for daily performance reviews.

```bash
python3 log_bms.py
```

---

## 8. Safety & Fail-Safes
*   **BMS Timeout:** If the BMS cable is unplugged or the WNT board hangs, the bridge detects the silence within 15 seconds and forces the SMA to `V_MAX_CHARGE` logic (0A) to stop all power flow.
*   **Galvanic Isolation:** The T-2CAN prevents ground loops between the high-power battery bank and the inverter control boards.

---
*Disclaimer: Operating high-voltage battery systems (840Ah) carries significant risk. Ensure your DC busbars and fuses are rated for the 500A discharge capacity enabled in this software.*