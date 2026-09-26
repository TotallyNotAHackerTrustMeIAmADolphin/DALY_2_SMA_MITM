#pragma once

// Pure Daly BMS frame parsing: data structs and byte-level parsers, with no
// Arduino/FreeRTOS dependency, so this header compiles and runs under
// `pio test -e native` (test/test_dalyframes). DalyRS485 owns the UART
// send/receive/retry loop and calls the functions below on each frame.

#include <stdint.h>

namespace DalyFrames
{
    // Daly UART frame envelope: every request/response frame is kFrameLen
    // bytes: kStartByte, kHostAddr, a command byte, kPayloadLen, kPayloadLen
    // data bytes, then a trailing checksum byte.
    constexpr uint8_t kStartByte = 0xA5;
    constexpr uint8_t kHostAddr = 0x40;
    constexpr uint8_t kPayloadLen = 8;
    constexpr uint8_t kFrameLen = 13; // 4-byte header + kPayloadLen + 1 checksum byte

    // Command bytes for the four frame types this firmware sends/parses.
    enum Cmd : uint8_t
    {
        BasicInfo = 0x90,
        MosfetStatus = 0x93,
        CellVoltages = 0x95,
        AlarmStatus = 0x98,
    };

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
    // and packOvervoltLevel1/2 are named fields since the CSV log and web
    // dashboard read them directly; rawBytes carries the full 8-byte
    // payload so every bit (named via kAlarmBitNames() below) can be
    // decoded and logged.
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
    // 1.5-4.5 V is a wiring/parse fault or a pack that must not be
    // charged/discharged anyway - see cellVoltagesPlausible() below.
    constexpr uint16_t kCellMinPlausibleMv = 1500;
    constexpr uint16_t kCellMaxPlausibleMv = 4500;

    // Name table for the 0x98 alarm payload, bytes 0-6 (byte 7 is the
    // numeric fault code, not a bitfield). Indexed [byte][bit]; nullptr for
    // bits not defined in the documented protocol. A function-local static,
    // not a namespace-scope array, so only a translation unit that actually
    // calls kAlarmBitNames() instantiates it (device build pins
    // -std=gnu++11, which rules out `inline constexpr`).
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

    // Daly UART frame checksum: low byte of the sum of the frame's first
    // kFrameLen-1 bytes.
    inline uint8_t checksum(const uint8_t frame[kFrameLen])
    {
        uint8_t sum = 0;
        for (int i = 0; i < kFrameLen - 1; i++)
            sum += frame[i];
        return sum;
    }

    // checksumOk compares checksum(frame) against the frame's trailing byte.
    inline bool checksumOk(const uint8_t frame[kFrameLen])
    {
        return checksum(frame) == frame[kFrameLen - 1];
    }

    // Builds a well-formed request frame for `cmd`: no payload (all-zero
    // data bytes, since every Daly request this firmware sends has one)
    // and a correct trailing checksum. `out` must be kFrameLen bytes.
    inline void buildRequest(Cmd cmd, uint8_t out[kFrameLen])
    {
        out[0] = kStartByte;
        out[1] = kHostAddr;
        out[2] = static_cast<uint8_t>(cmd);
        out[3] = kPayloadLen;
        for (int i = 0; i < kPayloadLen; i++)
            out[4 + i] = 0;
        out[kFrameLen - 1] = checksum(out);
    }

    // Pure byte-stream framer. Feed it one incoming UART byte at a time via
    // feed(); it returns true, and fills frameOut, exactly when a
    // checksum-valid kFrameLen-byte frame completes. It only resyncs on
    // kStartByte and validates the checksum, not the command byte or
    // address - DalyRS485::receiveFrame() decides what to do with an
    // unwanted-but-valid frame. On a checksum failure it discards only the
    // leading byte and rescans the rest of the window for the next
    // kStartByte, so a real frame starting inside a bad window is still
    // found.
    class FrameAssembler
    {
    public:
        void reset()
        {
            idx_ = 0;
            checksumFailedOnLastFeed_ = false;
            rescanned_ = false;
        }

        bool feed(uint8_t byte, uint8_t frameOut[kFrameLen])
        {
            checksumFailedOnLastFeed_ = false;

            if (idx_ == 0)
            {
                if (byte != kStartByte)
                    return false;
                rescanned_ = false; // a window starting fresh off the wire
            }

            buf_[idx_++] = byte;
            if (idx_ < kFrameLen)
                return false;

            if (checksumOk(buf_))
            {
                for (int i = 0; i < kFrameLen; i++)
                    frameOut[i] = buf_[i];
                idx_ = 0;
                return true;
            }

            // A real payload can itself contain 0xA5 (e.g. 3493 mV =
            // 0x0DA5), chaining rescan() through several sub-windows; only
            // the first (not rescanned_) is reported, so one corrupted
            // window logs once.
            checksumFailedOnLastFeed_ = !rescanned_;
            rescan();
            return false;
        }

        // True for exactly one feed() call per corrupted window: the one
        // whose completed kFrameLen-byte window first failed its checksum.
        bool checksumFailedOnLastFeed() const { return checksumFailedOnLastFeed_; }

    private:
        // A checksum just failed on buf_[0..kFrameLen-1]. Drop buf_[0] and
        // look for the next kStartByte among buf_[1..kFrameLen-1], sliding
        // it down to index 0; if none is found, start clean from the wire.
        void rescan()
        {
            for (int i = 1; i < kFrameLen; i++)
            {
                if (buf_[i] == kStartByte)
                {
                    int remaining = kFrameLen - i;
                    for (int j = 0; j < remaining; j++)
                        buf_[j] = buf_[i + j];
                    idx_ = remaining;
                    rescanned_ = true;
                    return;
                }
            }
            idx_ = 0;
        }

        uint8_t buf_[kFrameLen] = {};
        int idx_ = 0;
        bool checksumFailedOnLastFeed_ = false;
        bool rescanned_ = false;
    };

    // Daly UART "Basic Info" (cmd 0x90) 8-byte payload: [0..1] pack voltage
    // (0.1V), [4..5] pack current (0.1A, offset by 30000 = 0A), [6..7] SOC
    // (0.1%). [2..3] unused by this firmware, not decoded here.
    inline bool parseBasicInfo(const uint8_t data[8], DalyBasicInfo &out)
    {
        out.packVoltage = ((data[0] << 8) | data[1]) / 10.0f;
        uint16_t currentOffset = (data[4] << 8) | data[5];
        out.packCurrent = (currentOffset - 30000) / 10.0f;
        out.packSOC = ((data[6] << 8) | data[7]) / 10.0f;
        return true;
    }

    // Daly UART "Cell Voltages" (cmd 0x95) 8-byte payload, one frame per up
    // to 3 cells: [0] 1-based frame number, [1..2]/[3..4]/[5..6] cell
    // voltage N/N+1/N+2 (mV, big-endian), [7] unused. Frame-number
    // range/dedup checking and cellIdx mapping against expectedCells live
    // in CellFrameCollector below, since they depend on the pack's
    // configured cell count, not the frame itself.
    inline bool parseCellFrame(const uint8_t data[8], uint8_t &frameNo, uint16_t mv[3])
    {
        frameNo = data[0];
        mv[0] = (data[1] << 8) | data[2];
        mv[1] = (data[3] << 8) | data[4];
        mv[2] = (data[5] << 8) | data[6];
        return true;
    }

    // Upper bound on the cell count CellFrameCollector will collect.
    // Generously above the pack's actual cell count (kPackCells, 16) so
    // reset() can clamp an out-of-range expectedCells instead of trusting
    // it.
    constexpr int kMaxCollectorCells = 32;

    // Collects the 0x95 "Cell Voltages" stream into per-cell millivolt
    // values: reset(cells) for a fresh read, accept() once per received
    // payload, complete() once every frame has arrived. framesMask_ is
    // uint32_t so 1u << frameNum can't wrap.
    class CellFrameCollector
    {
    public:
        void reset(int cells)
        {
            if (cells < 0)
                cells = 0;
            if (cells > kMaxCollectorCells)
                cells = kMaxCollectorCells;
            cells_ = cells;
            expectedFrames_ = (cells_ + 2) / 3;
            framesMask_ = 0;
            framesReceived_ = 0;
            for (int i = 0; i < kMaxCollectorCells; i++)
                mv_[i] = 0;
        }

        // Feeds one 8-byte cell-voltage payload. Out-of-range frame numbers
        // and already-seen duplicates are rejected. The last frame is
        // partial when cells_ isn't a multiple of 3 (e.g. 16 cells -> frame
        // 6 carries only 1 cell); its unused mv slots are never written.
        // Returns true iff this payload's frame number was newly accepted.
        bool accept(const uint8_t payload[kPayloadLen])
        {
            uint8_t frameNo;
            uint16_t mv[3];
            parseCellFrame(payload, frameNo, mv);

            if (frameNo == 0 || frameNo > expectedFrames_)
                return false;

            uint32_t bit = 1u << frameNo;
            if (framesMask_ & bit)
                return false; // duplicate - already counted and stored

            framesMask_ |= bit;
            framesReceived_++;

            for (int i = 0; i < 3; i++)
            {
                int cellIdx = (frameNo - 1) * 3 + i;
                if (cellIdx < cells_)
                    mv_[cellIdx] = mv[i];
            }
            return true;
        }

        bool complete() const { return framesReceived_ == expectedFrames_; }

        // Per-cell millivolt values, indexed [0, cells_); valid past an
        // index once its frame has been accept()-ed.
        const uint16_t *mv() const { return mv_; }

        int framesReceived() const { return framesReceived_; }
        int expectedFrames() const { return expectedFrames_; }

    private:
        int cells_ = 0;
        int expectedFrames_ = 0;
        uint32_t framesMask_ = 0;
        int framesReceived_ = 0;
        uint16_t mv_[kMaxCollectorCells] = {};
    };

    // Daly UART "Status Info 2" (cmd 0x93), same documented protocol family
    // as the 0x90/0x95 frames above (e.g. syssi/esphome-daly-bms): [0]
    // charge/discharge status, [1]/[2] charge/discharge MOSFET state (0=off,
    // 1=on), [3] BMS life cycle count, [4..7] remaining capacity (Ah*1000).
    // NOT yet verified byte-for-byte against this specific pack's firmware -
    // sanity-check against a serial monitor after flashing. Defensive:
    // MOSFET bytes must be 0 or 1, else report failure instead of guessing.
    inline bool parseMosfetStatus(const uint8_t data[8], DalyMosfetStatus &out)
    {
        if (data[1] > 1 || data[2] > 1)
            return false;

        out.chargeMosOn = (data[1] == 1);
        out.dischargeMosOn = (data[2] == 1);
        return true;
    }

    // Daly UART "Alarm Info" (cmd 0x98) payload: bytes 0-6 are the bitfield
    // kAlarmBitNames() above names bit-for-bit; byte 7 is a numeric fault
    // code, not a bitfield. NOT yet verified byte-for-byte against this
    // specific pack's firmware - sanity-check against a serial monitor
    // after flashing, e.g. by temporarily lowering cvMaxCharge below the
    // pack's real voltage and confirming bit0 of byte 0 sets.
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
    // the envelope and reports its index via badIndex (-1 when every cell
    // passes). Compares directly in millivolts - the Daly's own unit and
    // CellFrameCollector::mv()'s - so there's no float round-trip to disagree
    // with the mV value actually stored (#70).
    inline bool cellVoltagesPlausible(const uint16_t *mv, int n, int &badIndex)
    {
        for (int i = 0; i < n; i++)
        {
            if (mv[i] < kCellMinPlausibleMv || mv[i] > kCellMaxPlausibleMv)
            {
                badIndex = i;
                return false;
            }
        }
        badIndex = -1;
        return true;
    }
}
