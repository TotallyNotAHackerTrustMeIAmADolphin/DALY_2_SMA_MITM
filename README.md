# SMA Sunny Island & Daly BMS: Active CAN Bridge (Pro-Edition v3.5)

An ESP32-S3 based "Man-in-the-Middle" CAN gateway designed to integrate a **Daly Smart BMS** (500A Discharge / 250A Charge) with an **SMA Sunny Island 3-Phase Cluster**. 

This appliance transforms raw, aggressive BMS data into a stabilized, professional-grade control signal, enabling smooth operation of high-capacity DIY LiFePO4 banks (e.g., 840Ah / 45kWh+).

---

## 1. The Core Logic: Cross-Computed Linear Tapering

Unlike standard gateways that simply pass data through, this bridge uses **Asymmetric Cross-Computation**. It calculates two independent current limits—one based on **Battery Voltage** and one based on **State of Charge (SOC)**—and selects the most restrictive value (the minimum).

### The Mathematical Intersection
The bridge calculates a "Slope" for both metrics:
1.  **Voltage Slope:** Scales `Max Amps` $\rightarrow$ `Trickle Amps` between `vStartTaper` and `vHighAlarmGate`.
2.  **SOC Slope:** Scales `Max Amps` $\rightarrow$ `Trickle Amps` between `socStartTaper` and `100%`.

**The Result:** If a single cell "runs" early (Voltage spikes while SOC is only 90%), the Voltage-based taper will take control and slow the charger down automatically. If the cells are perfectly balanced, the SOC-based taper will ensure a smooth arrival at 100%.

---

## 2. Detailed Charging Cycle (Step-by-Step)

| Phase | State | Behavior | Parameter Roles |
| :--- | :--- | :--- | :--- |
| **1. Bulk** | < `vStartTaper` & < `socStartTaper` | Full speed charging. | Uses `maxChargeA`. |
| **2. Taper** | > `vStartTaper` OR > `socStartTaper` | The bridge begins lowering the current limit (CCL) linearly. Every 0.01V increase results in a proportional decrease in Amps. | Slopes toward `trickleA`. |
| **3. Trickle** | $\ge$ `vHighAlarmGate` OR 100% SOC | The math stops. The bridge forces the SMA to maintain an exact, constant current to allow passive balancers to work. | Locks to `trickleA`. |
| **4. Stop** | $\ge$ `vMaxCharge` | Absolute safety cutoff. The bridge tells the SMA to stop all charging immediately. | Hard stop at `vMaxCharge`. |

---

## 3. Detailed Discharge Cycle (Step-by-Step)

| Phase | State | Behavior | Parameter Roles |
| :--- | :--- | :--- | :--- |
| **1. Normal** | > `vStartDTaper` & > `socStartDTaper` | Full discharge capacity available for house loads. | Uses `maxDischargeA`. |
| **2. Taper** | < `vStartDTaper` OR < `socStartDTaper` | The bridge restricts discharge current to prevent high-load "brownouts" as cells weaken. | Slopes toward `limpDischargeA`. |
| **3. Limp** | $\le$ `vLowAlarmGate` OR BMS Low Cell Alarm | The bridge forces a low constant discharge limit. This keeps the SMA cluster alive and synchronized but stops high-power loads from tripping the BMS. | Locks to `limpDischargeA`. |
| **4. Cutoff** | $\le$ `vMinDischarge` | Absolute floor. Bridge requests 0A discharge to prevent cell reversal. | Hard stop at `mdv`. |

---

## 4. Parameter Reference (The Management UI)

Adjust these live via `http://192.168.178.55/config`:

*   **vStartTaper (C/D):** The "Entry Point." The voltage where the bridge stops being a "transparent pipe" and starts actively restricting current.
*   **High/Low Cell Gate:** The "Target Finish." In charging, this is where the current should reach the Trickle level. In discharging, this is where the current should reach Limp level.
*   **Trickle Charge (A):** Ideally **2.0A to 5.0A**. Enough to satisfy the SMA control loop and allow Daly passive balancers (30mA - 200mA) to bleed off high cells.
*   **Limp Discharge (A):** Ideally **5.0A to 10.0A**. Just enough to keep the SMA Master L1 from shutting down entirely, allowing it to stay on until grid/solar power returns.
*   **Voltage Samples:** Sets the Moving Average window. Higher values (e.g., 15) stop the "hunting" caused by 3-phase inverter ripple but add slight latency.

---

## 5. Hardware Requirements & Wiring

*   **LilyGO T-2CAN** (Isolated dual CAN).
*   **Interface A:** SMA Sunny Island (Pin 4: CANH, Pin 5: CANL, Pin 3: GND).
*   **Interface B:** Daly WNT/CAN Board (Standard Pinout).

> [!IMPORTANT]  
> The LilyGO T-2CAN uses isolated transceivers. You **must** connect the **SGNDA** and **SGNDB** terminals to the respective CAN grounds or the communication will be unstable.

---

## 6. Safety Features
*   **BMS Watchdog:** If the bridge loses BMS data for >15s, it forces the SMA to 0A for safety.
*   **Factory Reset:** A one-click "Restore Defaults" button in the UI wipes NVS flash and reloads safe engineering fallbacks.
*   **Task Watchdog (WDT):** Hardware-level MCU monitor that reboots the ESP32 if the main loop hangs for more than 15 seconds.
*   **Isolated Design:** Prevents ground loops between the 48V DC bus logic and the Inverter control logic.

---
*Disclaimer: Operating high-capacity battery systems (840Ah) carries significant risk. Ensure all 13mm DC terminals on the SMA units are torqued correctly and your fuses are rated for the loads described.*