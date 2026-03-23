# SMA Sunny Island & Daly BMS: Active CAN Bridge (Pro-Edition v4.0)

An ESP32-S3 based "Man-in-the-Middle" appliance designed to sit between a **Daly Smart BMS** and an **SMA Sunny Island Cluster** (L1 Master). It intercepts raw BMS data and calculates stabilized, high-resolution control signals to ensure professional-grade battery management.

## 1. The Core Philosophy: "Voltage is Truth"
State of Charge (SOC) reported by many BMS units is a calculation prone to drift, "integer jumping," and calibration errors. This bridge ignores SOC for logic purposes and uses high-resolution (0.01V) battery voltage to drive a **Dual-Sloped Glideslope**.

### **Charging Glideslope**
1.  **Bulk Phase:** Full speed charging (`Max Charge Amps`) until the battery hits the **Start Taper Volts**.
2.  **Taper Phase:** As voltage rises, the bridge linearly reduces the current limit.
3.  **Trickle Phase:** Once **Target Trickle Volts** is reached, the bridge locks the current to exactly **Trickle Amps**. This provides a steady floor for passive balancers to work effectively.
4.  **Brick Wall:** At **Max Charge Volts**, the bridge requests 0A to protect against overvoltage.

### **Discharging Glideslope**
1.  **Normal Phase:** Full discharge capacity available until **Start Taper Volts (D)**.
2.  **Taper Phase:** Limits current as the battery enters the "lower knee" to prevent cell brownouts under heavy loads.
3.  **Limp Phase:** At **Target Limp Volts**, the bridge restricts the SMA to **Limp Amps**. This keeps the inverter alive and synchronized without tripping the BMS.
4.  **Hard Floor:** At **Min Discharge Volts**, the bridge requests 0A to prevent cell reversal.

---

## 2. Management UI (`/config`)

The configuration suite is logically grouped into directional profiles:

### **Charging Profile**
*   **Max Charge Amps:** The global limit during Bulk charging.
*   **Start Taper Volts:** The "Entry Point." When voltage hits this, current starts slowing down.
*   **Target Trickle Volts:** The "Finish Line." Current reaches the Trickle floor here.
*   **Trickle Amps:** The exact current maintained for top-end balancing.
*   **Max Charge Volts:** The absolute safety cutoff (0A).

### **Discharging Profile**
*   **Max Discharge Amps:** Peak household load limit.
*   **Start Taper Volts (D):** Entry point for discharge restriction.
*   **Target Limp Volts:** Voltage where the system settles into Limp Mode.
*   **Limp Amps:** The exact current maintained to keep the SMA Master online.
*   **Min Discharge Volts:** The absolute floor (0A).

### **System Tuning**
*   **Voltage Samples:** Moving average window (Recommended: 10-15). Smooths out inverter ripple.
*   **Restore Defaults:** A orange "Factory Reset" button that wipes NVS and reloads safe engineering fallbacks.

---

## 3. Industrial Stability & Safety

*   **Task Watchdog Timer (WDT):** A hardware-level heartbeat monitors the main loop. If the software hangs for more than 15 seconds, the MCU forces a hard reboot.
*   **CAN Bus-Off Recovery:** High-power environments generate EMI. If the internal CAN controller hits a "Bus-Off" state, the bridge detects it and hot-resets the driver automatically.
*   **BMS Watchdog:** If communication with the Daly BMS is lost for >15s, the bridge forces the SMA to 0A Charge/Discharge to prevent "blind" operation.
*   **Memory Guard:** Continuous monitoring of the Free Heap. If memory fragmentation occurs, the bridge reboots to maintain 100% uptime.
*   **Isolated Design:** The LilyGO T-2CAN provides isolation between the noisy DC bus environment and the inverter's control logic.

---

## 4. Hardware & Wiring

### **Interface A (To SMA Master L1)** & **Interface B (To Daly WNT)**
| RJ45 Pin | Wire Color (Typical) | Signal | LilyGO Terminal |
| :--- | :--- | :--- | :--- |
| **3** | **White/Green** | **GND** | **SGNDA / SGNDB** |
| **4** | **Blue** | **CAN High** | **CANHA / CANHB** |
| **5** | **White/Blue** | **CAN Low** | **CANLA / CANLB** |

> [!IMPORTANT]  
> You **must** connect the Signal Ground (SGND) terminals. Without a common ground reference, the isolated transceivers will experience common-mode errors.

---

## 5. Remote Logging (Python)

The bridge streams high-resolution telemetry to Port 2323. Use the provided `log_bms.py` for long-term health monitoring.

**Logger Features:**
*   **Connection Events:** Logs exact timestamps of established or lost connections.
*   **Black-Box Recording:** Saves real-time Current (CCL/DCL), Voltage, and SMA Mode.
*   **Automatic Reconnect:** Retries every 5 seconds if the network drops.

**Example Log:**
`[2024-05-20 14:00:01] [STATUS] Mode:RUN V:54.45 I:2.0 SOC:99% CCL:2.0 DCL:500.0 SMA:Float`

---

## 6. Operation Warnings
*   **Phase Sync:** SMA Sunny Island clusters use specific pins for phase synchronization. Never ground Pins 1, 2, or 6 on the SMA RJ45 port.
*   **Torque:** Ensure all 13mm DC battery terminals are torqued to 12Nm. Loose connections cause voltage ripples that can confuse any BMS gateway.

---
*Disclaimer: This software manages high-energy battery systems (840Ah+). The authors are not responsible for hardware damage. Use conservative voltage setpoints for initial commissioning.*