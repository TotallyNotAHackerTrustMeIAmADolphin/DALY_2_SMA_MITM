#pragma once

// --- T-CAN485 RS485 PINS ---
#define RS485_RX      21
#define RS485_TX      22
#define RS485_SE      19  // Transceiver Shutdown Enable
#define RS485_EN      17  // Transceiver Auto-Direction / Callback Enable
#define PIN_5V_EN     16  // CRITICAL: 5V Booster Power Enable

// --- T-CAN485 CAN PINS (For Phase 2) ---
#define CAN_TX        27
#define CAN_RX        26