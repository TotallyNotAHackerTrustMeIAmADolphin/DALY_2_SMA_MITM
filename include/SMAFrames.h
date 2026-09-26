#pragma once

// Pure SMA/Victron CAN frame encode/decode + bus-off retry timing (#45):
// no Arduino/FreeRTOS dependency, so this header compiles and runs under
// `pio test -e native` (see test/test_smaframes). SMA_CAN owns the actual
// twai_transmit()/twai_receive()/twai_driver_uninstall()/begin() driver
// calls and the debug-logging callback; it calls the functions below to
// decide what bytes to send, what a received frame means, and whether a
// bus-off recovery retry is due. Extracted from src/SMA_CAN.cpp - the
// byte-level behaviour here is unchanged, only where it lives moved.

#include <stdint.h>
#include <cmath>

namespace SMAFrames
{
    // CAN identifiers (11-bit). TX: what the bridge reports as the BMS;
    // RX: what the Sunny Island broadcasts.
    namespace CanId
    {
        constexpr uint32_t kLimits = 0x351;       // CVL, CCL, DCL, DVL
        constexpr uint32_t kSoc = 0x355;          // SOC, SOH
        constexpr uint32_t kMeasurements = 0x356; // pack V, I, temp
        constexpr uint32_t kFlags = 0x359;        // maintenance flag byte
        constexpr uint32_t kBmsName = 0x35E;      // "SMA" ASCII id
        constexpr uint32_t kBmsInfo = 0x35F;      // manufacturer data
        constexpr uint32_t kInverterGrid = 0x300; // byte0 bit0: grid present
        constexpr uint32_t kInverterMode = 0x305; // byte0: charge mode
    }

    // SOC reported while maintenance is active instead of the real one, so
    // the Sunny Island treats the pack as empty and charges it.
    constexpr uint16_t kMaintSocSentinel = 2;
    constexpr uint16_t kSohPercent = 100;        // 0x355 bytes 2-3, fixed
    constexpr uint8_t kMaintFlag = 0x10;         // 0x359 byte 0
    // 0x35E/0x35F go out on every kHeartbeatEvery-th encodeStatus() call.
    constexpr uint8_t kHeartbeatEvery = 11;
    constexpr unsigned long kBusRecoveryBackoffMs = 1000;

    // Mirrors SMA_CAN.h's SMATxData field-for-field. SMA_CAN.h aliases its
    // own SMATxData to this one (`using SMATxData = SMAFrames::SMATxData;`)
    // so existing callers (main.cpp) are unchanged - same pattern as
    // DalyRS485.h re-exposing DalyFrames.h's structs under their old names.
    struct SMATxData
    {
        float packVoltage;
        float packCurrent;
        int16_t packTemp;
        float packSOC;
        uint16_t ccl;
        uint16_t dcl;
        uint16_t cvl;
        uint16_t dvl;
        bool maintenanceActive;
        bool isResetting;
    };

    // One CAN frame to transmit: identifier, valid byte count, and an
    // always-8-byte payload (bytes at/after dlc are zero-filled and must
    // not be sent by the caller).
    struct CanFrame
    {
        uint32_t id;
        uint8_t dlc;
        uint8_t data[8];
    };

    // encodeStatus() emits at most 6 frames per call: the 4 status frames
    // (0x351/0x355/0x356/0x359) every call, plus the 0x35E/0x35F heartbeat
    // pair on every kHeartbeatEvery-th call.
    struct TxFrameSet
    {
        static constexpr int kMaxFrames = 6;
        CanFrame frames[kMaxFrames];
        int count = 0;

        void add(uint32_t id, uint8_t dlc, const uint8_t *src)
        {
            // Defensive bounds check (#45 review): today's only caller,
            // encodeStatus(), never emits more than kMaxFrames, but this
            // array had no other guard against a future frame type pushing
            // it past 6 - silently drop rather than write out of bounds on
            // this safety-critical CAN path.
            if (count >= kMaxFrames)
                return;
            CanFrame &f = frames[count++];
            f.id = id;
            f.dlc = dlc;
            for (int i = 0; i < 8; i++)
                f.data[i] = (i < dlc) ? src[i] : 0;
        }
    };

    // Encodes one sendStatus() call's worth of frames from an SMATxData
    // snapshot, byte-for-byte identical to the pre-#45 sendStatus():
    //   0x351 (8B): CVL/CCL/DCL/DVL (x10, little-endian). DVL is 0 while
    //               isResetting - see the comment at the DVL bytes.
    //   0x355 (4B): SOC (x1; maintenance sends kMaintSocSentinel instead
    //               of the real SOC) + SOH = 100.
    //   0x356 (6B): pack voltage (x100), pack current (x10), pack temp -
    //               all little-endian.
    //   0x359 (8B): all zero except kMaintFlag in byte 0 when
    //               maintenanceActive.
    // tickerIn/tickerOut thread the caller's _ticker35E counter through:
    // when it reaches kHeartbeatEvery it resets to 0 and the 0x35E/0x35F
    // frames are added; every other call just increments it.
    inline TxFrameSet encodeStatus(const SMATxData &data, uint8_t tickerIn, uint8_t &tickerOut)
    {
        TxFrameSet out;
        uint8_t frame[8];

        frame[0] = data.cvl & 0xFF;
        frame[1] = (data.cvl >> 8) & 0xFF;
        frame[2] = data.ccl & 0xFF;
        frame[3] = (data.ccl >> 8) & 0xFF;
        frame[4] = data.dcl & 0xFF;
        frame[5] = (data.dcl >> 8) & 0xFF;
        // Bytes 6-7 are the discharge voltage limit (#63). They used to
        // carry a status byte (0xC0 normal, 0x70 maintenance) with byte 7
        // zero, which an inverter reading DVL sees as 19.2 V / 11.2 V on a
        // 48 V pack. The cluster reset (isResetting) still sends 0x0000 for
        // its 5.5 s hold: that is the only thing the reset button changes
        // on the wire, so it stays byte-for-byte what it was.
        uint16_t dvl = data.isResetting ? 0 : data.dvl;
        frame[6] = dvl & 0xFF;
        frame[7] = (dvl >> 8) & 0xFF;
        out.add(CanId::kLimits, 8, frame);

        uint16_t outSOC = data.maintenanceActive ? kMaintSocSentinel : (uint16_t)std::round(data.packSOC);
        frame[0] = outSOC & 0xFF;
        frame[1] = (outSOC >> 8) & 0xFF;
        frame[2] = kSohPercent & 0xFF;
        frame[3] = (kSohPercent >> 8) & 0xFF;
        out.add(CanId::kSoc, 4, frame);

        uint16_t v_out = (uint16_t)std::round(data.packVoltage * 100.0f);
        int16_t i_out = (int16_t)std::round(data.packCurrent * 10.0f);
        frame[0] = v_out & 0xFF;
        frame[1] = (v_out >> 8) & 0xFF;
        frame[2] = i_out & 0xFF;
        frame[3] = (i_out >> 8) & 0xFF;
        frame[4] = data.packTemp & 0xFF;
        frame[5] = (data.packTemp >> 8) & 0xFF;
        out.add(CanId::kMeasurements, 6, frame);

        uint8_t frame359[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (data.maintenanceActive)
            frame359[0] |= kMaintFlag;
        out.add(CanId::kFlags, 8, frame359);

        tickerOut = (uint8_t)(tickerIn + 1);
        if (tickerOut >= kHeartbeatEvery)
        {
            tickerOut = 0;
            uint8_t smaId[8] = {'S', 0, 'M', 0, 'A', 0, 0, 0};
            out.add(CanId::kBmsName, 8, smaId);
            // Opaque manufacturer/battery-info bytes, sent verbatim; their
            // meaning isn't documented here.
            uint8_t mfg[8] = {3, 0, 0, 0, 0x48, 0x03, 0, 0};
            out.add(CanId::kBmsInfo, 8, mfg);
        }

        return out;
    }

    // Decoded update produced by a single received frame. hasChargeMode/
    // hasGridPresent tell the caller which fields (if any) this frame
    // updated - a frame is either a 0x305 mode frame or a 0x300 grid-status
    // frame, never both, but both flags are exposed independently so a
    // caller never has to guess which one fired.
    struct RxUpdate
    {
        bool hasChargeMode = false;
        const char *chargeMode = nullptr;
        bool hasGridPresent = false;
        bool gridPresent = false;
    };

    // Decodes one received CAN frame. Returns true iff out was populated
    // (id/dlc matched a known frame); false leaves out untouched by the
    // caller's contract (out is reset to defaults on every call regardless).
    //   0x305 byte0: SMA charge-mode byte -> 1=Bulk, 2=Absorption, 3=Float,
    //                4=Equalize, anything else "Unknown" (#104: it used to
    //                fall through to "Equalize", so 0 or garbage showed as
    //                an equalize charge on the dashboard).
    //   0x300 byte0 bit0: grid-present flag.
    inline const char *chargeModeName(uint8_t mode)
    {
        switch (mode)
        {
        case 1:
            return "Bulk";
        case 2:
            return "Absorption";
        case 3:
            return "Float";
        case 4:
            return "Equalize";
        default:
            return "Unknown";
        }
    }

    inline bool decodeFrame(uint32_t id, const uint8_t *data, uint8_t dlc, RxUpdate &out)
    {
        out = RxUpdate{};
        bool decoded = false;

        if (id == CanId::kInverterMode && dlc > 0)
        {
            uint8_t m = data[0];
            out.hasChargeMode = true;
            out.chargeMode = chargeModeName(m);
            decoded = true;
        }
        if (id == CanId::kInverterGrid && dlc > 0)
        {
            out.hasGridPresent = true;
            out.gridPresent = (data[0] & 0x01) != 0;
            decoded = true;
        }

        return decoded;
    }

    // Bus-off recovery retry decision (checkBusHealth()'s _wasBusOff /
    // _recoveryTimer logic): retries only once wasBusOff is true and more
    // than kBusRecoveryBackoffMs (strictly greater than) have elapsed since recoveryTimer
    // was last set. nowMillis/recoveryTimer are raw millis() values - an
    // unsigned subtraction, so a millis() wraparound (~49 days uptime)
    // behaves the same as it did before this extraction.
    inline bool shouldRetryBusRecovery(unsigned long nowMillis, bool wasBusOff, unsigned long recoveryTimer)
    {
        if (!wasBusOff)
            return false;
        return (nowMillis - recoveryTimer) > kBusRecoveryBackoffMs;
    }
}
