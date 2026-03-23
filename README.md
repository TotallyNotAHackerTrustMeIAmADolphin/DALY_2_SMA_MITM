# SMA Sunny Island & Daly BMS: Active CAN Bridge (Pro-Edition v3.0)

An ESP32-S3 based "Man-in-the-Middle" CAN gateway designed to integrate a **Daly Smart BMS** (500A Discharge / 250A Charge) with an **SMA Sunny Island 3-Phase Cluster**. 

This appliance transforms raw, aggressive BMS data into a stabilized, professional-grade control signal, enabling smooth operation of high-capacity DIY LiFePO4 banks (e.g., 840Ah / 45kWh+).

## 1. The Problem: "The Hunting & Crashing Cycle"
Standard Daly-to-SMA communication suffers from several critical flaws:
*   **Voltage Oscillation:** Using only voltage for tapering causes "hunting" (Current drops -> Voltage drops -> Bridge raises current again), leading to an unstable charging curve.
*   **Premature Cutoff:** Daly BMS units often report 100% SOC while cells are still below target voltage, stopping the charge before balancing can occur.
*   **Inductive Spikes:** Sudden 0A requests from the BMS at high current trigger DC bus violations and "Measurement Range" errors on the SMA.
*   **Discharge Risks:** High house loads at low SOC can "brown out" cells before the inverter switches to grid power.

## 2. Pro-Edition Features

### **Hybrid "Stable-Slope" Tapering**
The bridge calculates Charge (CCL) and Discharge (DCL) limits by comparing **Battery Voltage** and **SOC** simultaneously. It automatically selects the most restrictive value, creating a smooth, non-oscillating "glideslope" for both ends of the battery cycle.

### **Management Appliance UI**
A full browser-based configuration suite (`/config`) allowing live adjustment of 12+ parameters. Includes:
*   **Simple Spinners:** Native browser number arrows for easy adjustment on any device.
*   **Detailed Tooltips:** Descriptions for every safety setting built directly into the UI.
*   **Audit Trail:** Every change made via the Web UI is logged with a "Previous -> New" timestamped entry in the system log.

### **Advanced Protection Suite**
*   **Trickle Charge Mode:** Overrides the BMS "100% SOC" signal to maintain a low current (e.g., 2A) until the pack hits the absolute target voltage (`vMaxCharge`).
*   **Cell-Runner Balancing:** Detects specific high-cell alarm bits and drops current to **1.0A** to allow passive balancers to work without tripping the inverter.
*   **Low-Cell Limp Mode:** Forces a low discharge limit (e.g., 5A) if a cell is struggling at the bottom end, keeping the SMA synchronized but preventing a BMS hard-trip.
*   **Moving Average Filter:** Smooths DC bus noise over a configurable window (e.g., 10 samples).

### **SMA Telemetry Integration**
The bridge "sniffs" the internal SMA cluster communication to display:
*   **SMA Charge Mode:** (Bulk, Absorption, Float, or Equalize).
*   **Grid Status:** Real-time indication of whether the system is Grid-Tied or in Island/Off-Grid mode.

---

## 3. Hardware Requirements
*   **LilyGO T-2CAN** (Dual isolated CAN ports).
*   **Daly Smart BMS** (500A Discharge / 250A Charge model + WNT Board).
*   **SMA Sunny Island Cluster** (Connected to Master L1).

---

## 4. Wiring Diagram

The LilyGO T-2CAN uses isolated transceivers. **You must connect the SGND (Signal Ground) terminals.**

### **Interface A (To SMA Master L1)** & **Interface B (To Daly WNT)**
| RJ45 Pin | Wire Color (Typical) | Signal | LilyGO Terminal |
| :--- | :--- | :--- | :--- |
| **3** | **White/Green** | **GND** | **SGNDA / SGNDB** |
| **4** | **Blue** | **CAN High** | **CANHA / CANHB** |
| **5** | **White/Blue** | **CAN Low** | **CANLA / CANLB** |

> [!CAUTION]  
> **Phase Sync Warning:** SMA Sunny Island clusters use Pins 1, 3, and 6 for phase synchronization between inverters. If your cable is non-standard, ensure the ground is on Pin 2 or 3 as per your specific testing, but **never** ground a pin that carries a sync signal.

---

## 5. Monitoring & Control

### **Web Dashboard (Port 80)**
`http://192.168.178.55`
*   **Real-time Telemetry:** Voltage, Current, Power (kW), and SOC.
*   **Status Logs:** Live view of the internal logic (e.g., `Mode:TAPER`, `Mode:BALANCING`).

### **Configuration Suite (`/config`)**
*   **SOC Taper Start:** Set the percentage to begin slowing down (e.g., 95%).
*   **Trickle Charge:** Set the current for the final 1% of capacity.
*   **Voltage Samples:** Tune the "nervousness" of the bridge.

### **Remote Logging (Port 2323)**
The system streams high-resolution data to a raw TCP port. Use the provided Python script for long-term data collection:
```bash
python3 log_bms.py
```
**Example Log:**
`[STATUS] Mode:TAPER_C V:54.60 I:45.2 SOC:96% CCL:125.0 DCL:500.0 SMA:Absorption`

---

## 6. Safety & Watchdogs
*   **BMS Watchdog:** If the bridge loses BMS data for more than the configured timeout (default 15s), it forces the SMA to 0A charging.
*   **Persistent Storage:** All settings are saved to the ESP32 NVS Flash and survive power outages.
*   **Isolated Design:** Prevents ground loops between the high-power battery bank and inverter control logic.

---
*Disclaimer: Operating high-voltage battery systems (840Ah) carries significant risk. Ensure all 13mm DC terminals on the SMA units are torqued correctly and your fuses are rated for the loads described.*