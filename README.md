# SMA Sunny Island & Daly BMS: Active CAN Bridge (Pro-Edition v4.2)

An ESP32-S3 based "Man-in-the-Middle" appliance designed to sit between a **Daly Smart BMS** and an **SMA Sunny Island Cluster** (L1 Master). It intercepts raw BMS data and calculates stabilized, high-resolution control signals to ensure professional-grade battery management.

## 1. The Core Philosophy: "Voltage is Truth"
State of Charge (SOC) reported by many BMS units is a calculation prone to drift and calibration errors. This bridge ignores SOC for logic purposes and uses high-resolution (0.01V) battery voltage to drive a **Dual-Sloped Glideslope**.

### **Charging Glideslope**
1.  **Bulk Phase:** Full speed charging (`Max Charge Amps`) until the battery hits the **Start Taper Volts**.
2.  **Taper Phase:** As voltage rises, the bridge linearly reduces the current limit.
3.  **Trickle Phase:** Once **Target Trickle Volts** is reached, current locks to **Trickle Amps** to allow passive balancers to work.
4.  **Brick Wall:** At **Max Charge Volts**, the bridge requests 0A.

### **Discharging Glideslope**
1.  **Normal Phase:** Full discharge capacity available until **Start Taper Volts (D)**.
2.  **Limp Phase:** At **Target Limp Volts**, current is restricted to **Limp Amps**. This keeps the inverter alive and synchronized without tripping the BMS under heavy load.
3.  **Hard Floor:** At **Min Discharge Volts**, the bridge requests 0A to prevent cell reversal.

---

## 2. New: Winter Maintenance Mode (Mains Charge)
LiFePO4 batteries in low-solar environments can sit at dangerously low voltages for weeks. The Bridge now includes an automated **Grid-Charging Trigger**.

*   **The Trigger:** If voltage falls below **Maint Start Volts**, the bridge enters Maintenance Mode.
*   **The Action:** 
    *   **DCL -> 0A:** Discharging is strictly forbidden.
    *   **CCL -> Maint Amps:** Requests a steady charge (e.g., 20A) from the grid.
    *   **SOC Spoofing:** The bridge sends a fake **7% SOC** to the SMA. This triggers the SI's internal emergency reserve logic, forcing it to ignore "Self-Consumption" restrictions and pull from the grid.
    *   **CAN Flag 0x60:** Sends the `Force Charge Request` bit (ID 0x351) to the inverter.
*   **The Release:** Once the battery reaches **Maint Stop Volts**, the bridge returns to Normal (Solar) mode.

---

## 3. Industrial Stability & Safety

*   **Task Watchdog Timer (WDT):** Hardware-level heartbeat monitors the main loop. Software hangs result in a hard reboot within 15 seconds.
*   **BMS Watchdog:** If communication with the Daly BMS is lost for >15s, the bridge forces the SMA to 0A Charge/Discharge to prevent "blind" operation.
*   **BMS Alarm Sniffer:** The bridge passively sniffs IDs `0x359` and `0x35A`. Any Level 1 or Level 2 Daly alarms (Overvoltage, Undervoltage, Temp) are instantly captured and pushed to the remote log.
*   **CAN Bus-Off Recovery:** EMI-heavy environments can cause the CAN controller to enter a "Bus-Off" state. The bridge detects this and hot-resets the driver automatically.
*   **Memory Guard:** Free heap is monitored continuously; fragmentation triggers a preventive reboot.

---

## 4. Hardware & Wiring (LilyGO T-2CAN)

### **Interface A (To SMA Master L1)** & **Interface B (To Daly WNT)**
The LilyGO T-2CAN provides isolation between the noisy DC bus environment and the inverter's control logic.

| RJ45 Pin | Wire Color (Typical) | Signal | LilyGO Terminal |
| :--- | :--- | :--- | :--- |
| **3** | **White/Green** | **GND** | **SGNDA / SGNDB** |
| **4** | **Blue** | **CAN High** | **CANHA / CANHB** |
| **5** | **White/Blue** | **CAN Low** | **CANLA / CANLB** |

> [!IMPORTANT]  
> You **must** connect the Signal Ground (SGND) terminals. Isolated transceivers require a common ground reference to prevent common-mode errors.

---

## 5. Management UI (`/config`)

The configuration suite is grouped into directional profiles:

### **Charging Profile**
*   **Max Charge Amps:** Global limit during Bulk phase.
*   **Start Taper Volts:** Voltage where current begins to slow down.
*   **Target Trickle Volts:** Voltage where balancing begins.
*   **Trickle Amps:** Constant current for top-end balancing.

### **Winter Maintenance**
*   **Start Voltage:** Auto-start grid charge threshold.
*   **Stop Voltage:** Voltage where the system returns to Normal/Solar mode.
*   **Maintenance Amps:** Current drawn from the grid (usually 10A - 30A).

### **Discharging Profile**
*   **Max Discharge Amps:** Peak household load limit.
*   **Target Limp Volts:** Voltage where the system enters Limp Mode.
*   **Limp Amps:** Current maintained to keep the SMA Master online.
*   **Min Discharge Volts:** The absolute floor (0A).

---

## 6. Remote Logging (Python)

The bridge streams high-resolution telemetry and system events to **Port 2323**. Use the provided `log_bms.py` for long-term health monitoring.

**Telemetry Data includes:**
*   `[STATUS]` - Voltage, Current, SOC, CCL, DCL, and SMA Charge Mode.
*   `[AUTO]` - Automatic Maintenance triggers (Start/Stop).
*   `[BMS-ALARM]` - Hex-dumps of Daly alarm frames when bits are tripped.
*   `[CONFIG]` - Audit logs of when settings are changed via Web UI.

**Example Log:**
`[2026-03-23 23:32:44] [STATUS] Mode:WINTER_MAINT V:53.14 I:19.8 SOC:7% CCL:20.0 DCL:0.0 SMA:Bulk`

---

## 7. Operation Warnings
*   **Phase Sync:** SMA Sunny Island clusters use specific pins for phase synchronization. Never ground Pins 1, 2, or 6 on the SMA RJ45 port.
*   **SMA WebUI Settings:** Ensure "Energy Saving Mode" is **OFF** in the SMA interface to prevent the inverter from ignoring the bridge's force-charge requests while sleeping.

---
*Disclaimer: This software manages high-energy battery systems. The authors are not responsible for hardware damage. Use conservative voltage setpoints during initial commissioning.*