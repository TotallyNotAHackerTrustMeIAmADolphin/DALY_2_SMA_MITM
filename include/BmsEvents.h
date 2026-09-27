#pragma once

// The bmsTask edge-triggered event logic (#44): pure, so test/test_bmsevents
// runs it natively. bmsTask reads from the Daly driver, calls decide() (no
// lock needed), takes dataMutex to write currentData, then logs whatever
// events came back via netLog.
//
// Same shape as StatusFrame::decide(): previous state + a new reading in,
// one-shot events to log + updated state out. Unlike StatusFrame (one
// Snapshot per 250ms tick), bmsTask reads DalyBasicInfo/DalyMosfetStatus/
// DalyAlarmStatus at three different points in its loop, so decide() takes
// each reading as an optional pointer and bmsTask calls it once per
// successful read, passing nullptr for the other two.

#include <stdint.h>
#include <cmath>
#include <cstring>
#include "DalyFrames.h"

namespace BmsEvents
{
    using DalyFrames::DalyAlarmStatus;
    using DalyFrames::DalyBasicInfo;
    using DalyFrames::DalyMosfetStatus;

    // Persistent between calls; owned by bmsTask as a local, with baseline
    // flags so the first read after boot doesn't log a spurious "changed"
    // event.
    struct State
    {
        bool haveSoc = false;
        float lastSoc = 0.0f;

        bool haveMosfetBaseline = false;
        bool lastChargeMosOn = false;
        bool lastDischargeMosOn = false;

        bool haveAlarmBaseline = false;
        uint8_t lastAlarmBytes[7] = {0};
        uint8_t lastFaultCode = 0;
    };

    // One-shot log events for this call - bmsTask emits the existing log
    // lines for whichever of these fired. alarmChanged[b] is an XOR mask of
    // bytes 0-6 of the 0x98 payload against the previous reading - bmsTask
    // loops its set bits, naming each via DalyFrames::kAlarmBitNames() and
    // reading SET/CLEARED off the new alarm reading itself.
    struct Events
    {
        bool socJumped = false;
        float socFrom = 0.0f;
        float socTo = 0.0f;

        bool chargeMosChanged = false;
        bool chargeMosOn = false;
        bool dischargeMosChanged = false;
        bool dischargeMosOn = false;

        uint8_t alarmChanged[7] = {0};

        bool faultCodeChanged = false;
        uint8_t faultCodeFrom = 0;
        uint8_t faultCodeTo = 0;
    };

    // basicInfo/mosfetStatus/alarmStatus are nullptr when that particular
    // read didn't happen (or failed) this call - bmsTask calls this once
    // per successful bms.readX(), passing only the one it just read.
    inline Events decide(State &st, const DalyBasicInfo *basicInfo,
                          const DalyMosfetStatus *mosfetStatus,
                          const DalyAlarmStatus *alarmStatus)
    {
        Events ev;

        // A Daly BMS recalibrates SOC to 100% on a "charge full" condition
        // (or occasionally jumps for other reasons); flag that as an event
        // so an abrupt CCL drop at the SMA can be lined up against it.
        if (basicInfo)
        {
            float soc = basicInfo->packSOC;
            if (st.haveSoc &&
                (fabsf(soc - st.lastSoc) > 10.0f || (soc >= 99.9f && st.lastSoc < 95.0f)))
            {
                ev.socJumped = true;
                ev.socFrom = st.lastSoc;
                ev.socTo = soc;
            }
            st.lastSoc = soc;
            st.haveSoc = true;
        }

        // BMS's own hardware protection state. Edge-triggered only (not
        // every poll) so a stuck-on condition doesn't spam the log queue;
        // baseline flag so the first read after boot doesn't log a
        // spurious "changed" event from an unset previous state.
        if (mosfetStatus)
        {
            if (st.haveMosfetBaseline)
            {
                if (mosfetStatus->chargeMosOn != st.lastChargeMosOn)
                {
                    ev.chargeMosChanged = true;
                    ev.chargeMosOn = mosfetStatus->chargeMosOn;
                }
                if (mosfetStatus->dischargeMosOn != st.lastDischargeMosOn)
                {
                    ev.dischargeMosChanged = true;
                    ev.dischargeMosOn = mosfetStatus->dischargeMosOn;
                }
            }
            st.lastChargeMosOn = mosfetStatus->chargeMosOn;
            st.lastDischargeMosOn = mosfetStatus->dischargeMosOn;
            st.haveMosfetBaseline = true;
        }

        // Edge-triggered, one bit mask per changed byte (undefined bits
        // still reported, by byte.bit position, so an unexpected fault
        // isn't silently swallowed) - baseline flag so the first read after
        // boot doesn't report every bit as "SET"/"CLEARED" from an all-zero
        // starting point.
        if (alarmStatus)
        {
            if (st.haveAlarmBaseline)
            {
                for (int b = 0; b < 7; b++)
                    ev.alarmChanged[b] = alarmStatus->rawBytes[b] ^ st.lastAlarmBytes[b];

                if (alarmStatus->rawBytes[7] != st.lastFaultCode)
                {
                    ev.faultCodeChanged = true;
                    ev.faultCodeFrom = st.lastFaultCode;
                    ev.faultCodeTo = alarmStatus->rawBytes[7];
                }
            }
            memcpy(st.lastAlarmBytes, alarmStatus->rawBytes, sizeof(st.lastAlarmBytes));
            st.lastFaultCode = alarmStatus->rawBytes[7];
            st.haveAlarmBaseline = true;
        }

        return ev;
    }
}
