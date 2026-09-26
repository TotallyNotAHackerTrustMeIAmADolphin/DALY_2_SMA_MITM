#pragma once

// Pure Daly BMS frame parsing (#30): data structs and byte-level parsers,
// with no Arduino/FreeRTOS dependency, so this header compiles and runs
// under `pio test -e native` (see test/test_dalyframes). DalyRS485 owns the
// UART send/receive/retry loop and the debug-logging callback; it calls the
// functions below on each frame's raw payload. Extracted from
// src/DalyRS485.cpp - the byte-level behaviour here is unchanged, only
// where it lives moved.

#include <stdint.h>

namespace DalyFrames
{
    struct DalyBasicInfo
    {
        float packVoltage;
        float packCurrent;
        float packSOC;
    };

    struct DalyMosfetStatus
    {
        bool chargeMosOn;
        bool dischargeMosOn;
    };

    // Daly "Alarm Info" (cmd 0x98) protection bitfield. cellOvervoltLevel1/2
    // and packOvervoltLevel1/2 are kept as named fields since the CSV log
    // and web dashboard read them directly; rawBytes carries the full
    // 8-byte payload so every bit (named via kAlarmBitNames() below) can be
    // decoded and logged - see parseAlarmStatus() for the byte
    // layout/provenance.
    struct DalyAlarmStatus
    {
        bool cellOvervoltLevel1;
        bool cellOvervoltLevel2;
        bool packOvervoltLevel1;
        bool packOvervoltLevel2;
        bool anyProtectionActive; // true if any bit in bytes 0-6 of the alarm frame is set
        uint8_t rawBytes[8];      // full 0x98 payload; rawBytes[7] is the numeric fault code
    };

    // Plausible cell-voltage envelope for a LiFePO4 cell. Anything outside
    // 1.5-4.5 V is either a wiring/parse fault or a pack that must not be
    // charged/discharged anyway - see cellVoltagesPlausible() below and
    // DalyRS485::readCellVoltages(), which rejects the whole read (rather
    // than feed the value into bmsTask's moving-average filter) when this
    // gate fails.
    constexpr uint16_t kCellMinPlausibleMv = 1500;
    constexpr uint16_t kCellMaxPlausibleMv = 4500;

    // Name table for the 0x98 alarm payload, bytes 0-6 (byte 7 is the
    // numeric fault code, not a bitfield). Indexed [byte][bit]; nullptr for
    // bits not defined in the documented protocol. See parseAlarmStatus()
    // for the byte layout this mirrors.
    //
    // Wrapped in a function returning a reference to a function-local
    // static, rather than a plain namespace-scope array, so the table has
    // exactly one definition with internal linkage per translation unit
    // that actually calls it - a translation unit that merely includes this
    // header without calling kAlarmBitNames() never instantiates it, so it
    // can't trigger an unused-variable warning there. (The device build
    // pins -std=gnu++11 via the Arduino-ESP32 core, which rules out a
    // simpler C++17 `inline constexpr` array.)
    inline const char *const (&kAlarmBitNames())[7][8]
    {
        static const char *const table[7][8] = {
            // byte 0: cell/pack over/undervoltage
            {"Cell overvoltage Level 1", "Cell overvoltage Level 2",
             "Cell undervoltage Level 1", "Cell undervoltage Level 2",
             "Pack overvoltage Level 1", "Pack overvoltage Level 2",
             "Pack undervoltage Level 1", "Pack undervoltage Level 2"},
            // byte 1: charge/discharge over/undertemperature
            {"Charge overtemperature Level 1", "Charge overtemperature Level 2",
             "Charge undertemperature Level 1", "Charge undertemperature Level 2",
             "Discharge overtemperature Level 1", "Discharge overtemperature Level 2",
             "Discharge undertemperature Level 1", "Discharge undertemperature Level 2"},
            // byte 2: charge/discharge overcurrent, SOC high/low
            {"Charge overcurrent Level 1", "Charge overcurrent Level 2",
             "Discharge overcurrent Level 1", "Discharge overcurrent Level 2",
             "SOC high Level 1", "SOC high Level 2",
             "SOC low Level 1", "SOC low Level 2"},
            // byte 3: cell voltage difference, temperature difference
            {"Cell voltage difference Level 1", "Cell voltage difference Level 2",
             "Temperature difference Level 1", "Temperature difference Level 2",
             nullptr, nullptr, nullptr, nullptr},
            // byte 4: MOSFET temperature/adhesion/open circuit
            {"Charge MOSFET overtemperature", "Discharge MOSFET overtemperature",
             "Charge MOSFET temperature sensor fault", "Discharge MOSFET temperature sensor fault",
             "Charge MOSFET adhesion (stuck on)", "Discharge MOSFET adhesion (stuck on)",
             "Charge MOSFET open circuit", "Discharge MOSFET open circuit"},
            // byte 5: AFE/sampling/EEPROM/RTC/precharge/communication faults
            {"AFE chip fault", "Voltage sampling dropped",
             "Cell temperature sensor fault", "EEPROM fault",
             "RTC fault", "Precharge failure",
             "Communication failure", "Internal communication failure"},
            // byte 6: current module/pack voltage/short circuit/low-voltage charging
            {"Current module fault", "Pack voltage detection fault",
             "Short circuit protection", "Low-voltage charging forbidden",
             nullptr, nullptr, nullptr, nullptr},
        };
        return table;
    }

    // Daly UART frame checksum: low byte of the sum of the first 12 bytes
    // of the 13-byte frame (0xA5, address, command, length, 8 data bytes),
    // compared against byte 12.
    inline bool checksumOk(const uint8_t frame[13])
    {
        uint8_t checksum = 0;
        for (int i = 0; i < 12; i++)
            checksum += frame[i];
        return checksum == frame[12];
    }

    // Daly UART "Basic Info" (cmd 0x90) 8-byte payload:
    //   [0..1] pack voltage, 0.1V units
    //   [4..5] pack current, 0.1A units, offset by 30000 (30000 = 0A)
    //   [6..7] SOC, 0.1% units
    // (bytes [2..3] are unused by this firmware - not decoded here, same as
    // the pre-refactor code.)
    inline bool parseBasicInfo(const uint8_t data[8], DalyBasicInfo &out)
    {
        out.packVoltage = ((data[0] << 8) | data[1]) / 10.0f;
        uint16_t currentOffset = (data[4] << 8) | data[5];
        out.packCurrent = (currentOffset - 30000) / 10.0f;
        out.packSOC = ((data[6] << 8) | data[7]) / 10.0f;
        return true;
    }

    // Daly UART "Cell Voltages" (cmd 0x95) 8-byte payload, one frame per up
    // to 3 cells:
    //   [0]    1-based frame number
    //   [1..2] cell voltage N, mV, big-endian
    //   [3..4] cell voltage N+1, mV, big-endian
    //   [5..6] cell voltage N+2, mV, big-endian
    //   [7]    unused by this firmware (not decoded)
    // Frame-number range/dedup checking and cellIdx bounds against
    // expectedCells stay in DalyRS485::readCellVoltages(), since they
    // depend on the pack's configured cell count, not the frame itself.
    inline bool parseCellFrame(const uint8_t data[8], uint8_t &frameNo, uint16_t mv[3])
    {
        frameNo = data[0];
        mv[0] = (data[1] << 8) | data[2];
        mv[1] = (data[3] << 8) | data[4];
        mv[2] = (data[5] << 8) | data[6];
        return true;
    }

    // Daly UART "Status Info 2" (cmd 0x93) payload layout, from the same
    // community-documented protocol family as the 0x90/0x95 frames above
    // (e.g. syssi/esphome-daly-bms, patman15 Daly UART docs):
    //   [0] charge/discharge status (0=stall, 1=charge, 2=discharge)
    //   [1] charge MOSFET state (0=off, 1=on)
    //   [2] discharge MOSFET state (0=off, 1=on)
    //   [3] BMS life cycle count
    //   [4..7] remaining capacity, Ah*1000
    // NOT yet verified byte-for-byte against this specific pack's firmware -
    // sanity-check against a serial monitor / known MOSFET state after
    // flashing. Defensive: MOSFET bytes must be 0 or 1; anything else means
    // this frame isn't what we think it is, so report failure instead of
    // guessing.
    inline bool parseMosfetStatus(const uint8_t data[8], DalyMosfetStatus &out)
    {
        if (data[1] > 1 || data[2] > 1)
            return false;

        out.chargeMosOn = (data[1] == 1);
        out.dischargeMosOn = (data[2] == 1);
        return true;
    }

    // Daly UART "Alarm Info" (cmd 0x98) payload layout, same documented
    // protocol family as above:
    //   [0] bit0/1 = cell overvolt level1/2, bit2/3 = cell undervolt level1/2,
    //       bit4/5 = pack overvolt level1/2, bit6/7 = pack undervolt level1/2
    //   [1] bit0/1 = charge overtemp L1/L2, bit2/3 = charge undertemp L1/L2,
    //       bit4/5 = discharge overtemp L1/L2, bit6/7 = discharge undertemp L1/L2
    //   [2] bit0/1 = charge overcurrent L1/L2, bit2/3 = discharge overcurrent L1/L2,
    //       bit4/5 = SOC high L1/L2, bit6/7 = SOC low L1/L2
    //   [3] bit0/1 = cell voltage difference L1/L2, bit2/3 = temperature difference L1/L2
    //   [4] bit0 = charge MOS overtemp, bit1 = discharge MOS overtemp,
    //       bit2/3 = charge/discharge MOS temp sensor fault,
    //       bit4/5 = charge/discharge MOS adhesion (stuck on),
    //       bit6/7 = charge/discharge MOS open circuit
    //   [5] bit0 = AFE chip fault, bit1 = voltage sampling dropped,
    //       bit2 = cell temp sensor fault, bit3 = EEPROM fault, bit4 = RTC fault,
    //       bit5 = precharge failure, bit6 = communication failure,
    //       bit7 = internal communication failure
    //   [6] bit0 = current module fault, bit1 = pack voltage detection fault,
    //       bit2 = short circuit protection, bit3 = low-voltage charging forbidden
    //   [7] numeric fault code
    // See kAlarmBitNames() above for the byte/bit -> name table this
    // mirrors. NOT yet verified byte-for-byte against this specific pack's
    // firmware - sanity-check against a serial monitor after flashing, e.g.
    // by temporarily lowering cvMaxCharge below the pack's real voltage and
    // confirming bit0 of byte 0 sets.
    inline bool parseAlarmStatus(const uint8_t data[8], DalyAlarmStatus &out)
    {
        for (int i = 0; i < 8; i++)
            out.rawBytes[i] = data[i];

        out.cellOvervoltLevel1 = data[0] & 0x01;
        out.cellOvervoltLevel2 = data[0] & 0x02;
        out.packOvervoltLevel1 = data[0] & 0x10;
        out.packOvervoltLevel2 = data[0] & 0x20;

        out.anyProtectionActive = false;
        for (int i = 0; i < 7; i++)
        {
            if (data[i] != 0)
            {
                out.anyProtectionActive = true;
                break;
            }
        }
        return true;
    }

    // Plausibility gate for a set of cell voltages (see kCellMinPlausibleMv/
    // kCellMaxPlausibleMv above). Returns false on the first cell outside
    // the envelope and reports its index via badIndex; badIndex is set to
    // -1 when every cell passes. Compares directly in volts rather than
    // rounding to a uint16_t mv count: the old (uint16_t)(v[i]*1000+0.5)
    // cast was undefined behaviour for NaN, negative or huge v[i] (the
    // float-to-integer conversion is only defined when the truncated value
    // fits the target type). A NaN input now fails every comparison and is
    // correctly rejected rather than converting to an unspecified/UB mv
    // value. This also drops the old rounding, so a value a hair below the
    // limit (e.g. 1.4996) that used to round up and pass is now rejected.
    inline bool cellVoltagesPlausible(const float *v, int n, int &badIndex)
    {
        for (int i = 0; i < n; i++)
        {
            if (!(v[i] >= kCellMinPlausibleMv / 1000.0f && v[i] <= kCellMaxPlausibleMv / 1000.0f))
            {
                badIndex = i;
                return false;
            }
        }
        badIndex = -1;
        return true;
    }
}
