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
    // Daly UART frame envelope (#103): every request/response frame is
    // kFrameLen bytes: kStartByte, kHostAddr, a command byte, kPayloadLen,
    // kPayloadLen data bytes, then a trailing checksum byte.
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

    // Daly UART frame checksum: low byte of the sum of the first
    // kFrameLen-1 bytes of the frame (0xA5, address, command, length, 8
    // data bytes).
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
    // data bytes) and a correct trailing checksum. `out` must be kFrameLen
    // bytes. Every Daly request this firmware sends has an all-zero
    // payload, so buildRequest() takes no payload argument.
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

    // Pure byte-stream framer (#101). Feed it one incoming UART byte at a
    // time via feed(); it returns true, and fills frameOut, exactly when a
    // checksum-valid kFrameLen-byte frame completes. It only resyncs on
    // kStartByte and validates the checksum - it does not look at the
    // command byte or address, since a legitimate frame for a different
    // command is complete and correct, just not the one the caller wanted;
    // DalyRS485::receiveFrame() is the one that decides what to do with an
    // unwanted-but-valid frame (see its comment).
    //
    // On a checksum failure, the old byte-sync loops this replaces
    // (DalyRS485::receiveSingleFrame(), and the inner loop of
    // readCellVoltages()) threw away the whole kFrameLen-byte window and
    // restarted synchronization from the next byte off the wire. A stray
    // kStartByte anywhere in that window started a false frame and cost
    // the real frame behind it. feed() instead discards only the leading
    // byte and rescans the rest of the window for the next kStartByte, so
    // a real frame that started inside a bad window is still found. This
    // is a deliberate behaviour change (#101) - the byte lost to a false
    // sync no longer takes a real frame down with it.
    class FrameAssembler
    {
    public:
        void reset()
        {
            idx_ = 0;
            checksumFailedOnLastFeed_ = false;
        }

        bool feed(uint8_t byte, uint8_t frameOut[kFrameLen])
        {
            checksumFailedOnLastFeed_ = false;

            if (idx_ == 0 && byte != kStartByte)
                return false;

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

            checksumFailedOnLastFeed_ = true;
            rescan();
            return false;
        }

        // True for exactly one feed() call: the one whose completed
        // kFrameLen-byte window failed its checksum. Lets a caller log the
        // same way the pre-#101 readCellVoltages() loop did, without the
        // assembler itself doing any logging.
        bool checksumFailedOnLastFeed() const { return checksumFailedOnLastFeed_; }

    private:
        // A checksum just failed on buf_[0..kFrameLen-1]. Drop buf_[0] (the
        // byte that started this bad window) and look for the next
        // kStartByte among buf_[1..kFrameLen-1], sliding it (and whatever
        // follows it) down to index 0. If none is found the window really
        // was noise; start clean from the next byte off the wire.
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
                    return;
                }
            }
            idx_ = 0;
        }

        uint8_t buf_[kFrameLen] = {};
        int idx_ = 0;
        bool checksumFailedOnLastFeed_ = false;
    };

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
    // Frame-number range/dedup checking and cellIdx mapping against
    // expectedCells now live in CellFrameCollector below, since they
    // depend on the pack's configured cell count, not the frame itself.
    inline bool parseCellFrame(const uint8_t data[8], uint8_t &frameNo, uint16_t mv[3])
    {
        frameNo = data[0];
        mv[0] = (data[1] << 8) | data[2];
        mv[1] = (data[3] << 8) | data[4];
        mv[2] = (data[5] << 8) | data[6];
        return true;
    }

    // Upper bound on the cell count CellFrameCollector will collect
    // (#102). Generously above CellSmoother::MAX_CELLS (16, the actual
    // pack size this firmware supports) rather than importing that
    // constant, so DalyFrames.h stays free of a dependency on
    // CellSmoother.h; reset() clamps to this rather than trusting an
    // out-of-range expectedCells. A uint32_t frame mask (below) safely
    // covers every frame number this bound can produce.
    constexpr int kMaxCollectorCells = 32;

    // Collects the 0x95 "Cell Voltages" stream into per-cell millivolt
    // values (#102): reset(cells) for a fresh read, accept() once per
    // received payload, complete() once every frame has arrived. Replaces
    // the frame-number range check, dedup (framesMask), and frame->cell
    // mapping ((frameNo-1)*3+i) that used to live inline in
    // DalyRS485::readCellVoltages() - same logic, now pure and native-
    // testable. framesMask_ is uint32_t, not the uint8_t the inline
    // version used, which silently truncated (1 << frameNum wrapped) for
    // more than ~21 cells; not reachable with today's 16-cell pack, but a
    // real latent bug the old shape couldn't express.
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

        // Feeds one 8-byte cell-voltage payload. Out-of-range frame
        // numbers (0, or beyond expectedFrames_) are rejected; a frame
        // number already seen is a duplicate and is ignored (its mv
        // values were already stored). The last frame is partial when
        // cells_ isn't a multiple of 3 (e.g. 16 cells -> frame 6 carries
        // only 1 cell); its unused mv slots are never written, matching
        // the `if (cellIdx < cells_)` bound the old inline code had.
        // Returns true iff this payload's frame number was newly
        // accepted (false for out-of-range or duplicate).
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

        // Per-cell millivolt values, indexed [0, cells_). Only valid past
        // an index once its frame has been accept()-ed; unset slots stay
        // at the 0 reset() left them.
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
