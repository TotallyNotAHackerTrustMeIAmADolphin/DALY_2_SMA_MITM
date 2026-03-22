# SMA Sunny Island & Daly BMS: Active CAN Bridge

An ESP32-S3 based "Man-in-the-Middle" CAN gateway for integrating **Daly Smart BMS** (via WNT/CAN interface) with **SMA Sunny Island** (3-Phase Cluster) inverters.

## 1. The Problem
Daly BMS units often request unrealistic charge currents (e.g., 600A) and trigger hard safety disconnects at 0% or 100% SoC. This causes:
*   **DC Voltage Spikes:** Abruptly stopping 150A+ causes induction spikes that crash SMA inverters.
*   **Measurement Range Violations:** Sudden voltage changes lock the Sunny Island into error states.
*   **Timeouts:** The Daly CAN interface is often too slow for the SMA 1000ms heartbeat requirement.

## 2. The Solution
This project uses a **LilyGO T-2CAN** to sit between the BMS and the Inverters.
*   **Active Tapering:** Smoothly reduces Charge (CCL) and Discharge (DCL) currents as the battery reaches empty or full.
*   **Asynchronous Heartbeat:** The bridge feeds the SMA a high-speed (200ms) heartbeat, preventing communication timeouts even if the BMS is busy.
*   **Protocol Emulation:** Emulates the Pylontech/SMA handshake (ID `0x35E`) and protection flags (`0x359`) for maximum stability.

---

## 3. Hardware Requirements
*   **LilyGO T-2CAN** (ESP32-S3 with dual CAN: Internal TWAI + MCP2515).
*   **Daly Smart BMS** with WNT CAN/RS485 interface board.
*   **3x SMA Sunny Island** (Cluster configuration).
*   **2x RJ45 Ethernet Cables** (Stripped at one end).

---

## 4. Wiring Diagram

The LilyGO T-2CAN uses isolated CAN transceivers. **Ensure you use the SGND (Signal Ground) terminals.**

### **A. Daly WNT to LilyGO (Port B - Internal)**
| RJ45 Pin | Wire Color (T568B) | Signal | LilyGO Terminal |
| :--- | :--- | :--- | :--- |
| **2** | Orange | **GND** | **SGNDB** |
| **4** | Blue | **CAN High** | **CANHB** |
| **5** | White/Blue | **CAN Low** | **CANLB** |

### **B. LilyGO to SMA Master L1 (Port A - MCP2515)**
| RJ45 Pin | Wire Color (T568B) | Signal | LilyGO Terminal |
| :--- | :--- | :--- | :--- |
| **2** | Orange | **GND** | **SGNDA** |
| **4** | Blue | **CAN High** | **CANHA** |
| **5** | White/Blue | **CAN Low** | **CANLA** |

*Note: Pins 1, 3, 6, 7, 8 on the SMA side carry SYNC signals. **Do not connect them to the LilyGO.** Only pins 2, 4, 5 are required.*

---

## 5. Software Configuration

### **Prerequisites**
*   **PlatformIO** (Linux/Windows/Mac).
*   Library: `autowp/mcp2515` (or the LilyGO internal driver).

### **User Constants**
Edit these values in `main.cpp` to match your specific battery bank:

```cpp
const float MAX_CHARGE_LIMIT_A    = 250.0; // Charge Current Cap
const float MAX_DISCHARGE_LIMIT_A = 500.0; // Discharge Current Cap
const float V_MAX_CHARGE     = 55.20;      // Hard stop (3.45V/cell)
const float V_START_C_TAPER  = 54.00;      // Start slowing down (3.37V/cell)
const float V_START_D_TAPER  = 49.60;      // Start slowing discharge
const float V_MIN_DISCHARGE  = 48.00;      // Cutoff (3.0V/cell)
```

### **Fixed IP & Network**
Update your WiFi credentials and Static IP:
```cpp
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
IPAddress local_IP(192, 168, 178, 55); 
```

---

## 6. Installation & Updates

### **Initial Upload (USB)**
1.  Connect LilyGO via USB.
2.  `pio run -t upload`

### **Wireless Updates (OTA)**
Once the device is in the basement, update wirelessly via PlatformIO:
```bash
# in platformio.ini
upload_protocol = espota
upload_port = 192.168.178.55

# Then run:
pio run -t upload
```

---

## 7. Monitoring & Logging

### **Live Dashboard (Telnet)**
Connect to the device brain over WiFi to see real-time "Daly vs Bridge" logic:
```bash
telnet 192.168.178.55
```
**Example Output:**
```text
--- BMS BRIDGE DASHBOARD ---
BATTERY: 54.20V | 12.5A | 92% SOC
CHARGE : Daly Req: 600A | Bridge Allowed: 185.0A [Tapering!]
DISCHRG: Daly Req: 250A | Bridge Allowed: 500.0A 
----------------------------
```

### **Logging to File (Linux)**
Run the included Python script to save logs with timestamps:
```bash
python3 log_bms.py
```

---

## 8. Safety Features
1.  **BMS Timeout Watchdog:** If the Daly BMS stops sending data for >10 seconds, the LilyGO automatically tells the SMA to stop all current (0A) to protect the battery.
2.  **Linear Tapering:** Unlike the BMS "On/Off" logic, the Bridge reduces current linearly, preventing DC bus oscillations.
3.  **Galvanic Isolation:** Uses the T-2CAN's isolated transceivers to prevent ground loops between the battery bank and the inverter cluster.

---

## 9. Disclaimer
*This project involves high-current DC power and complex inverter clusters. Always verify your wiring and fuse ratings. High-current discharge (500A) requires appropriately sized busbars and BMS FETs.*