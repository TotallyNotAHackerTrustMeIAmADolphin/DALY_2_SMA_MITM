#pragma once
#include <stdint.h>
#include <math.h>

// The Daly BMS's own cell overvoltage protection on this pack (#8, confirmed
// by the operator) - cvMaxCharge must stay below this with real margin, since
// a Daly-side trip resets the reported SOC to 100% and can produce a bus
// transient. Not a substitute for this firmware's own cutoff.
constexpr float kDalyOvervoltageV = 3.65f;
// Minimum margin cvMaxCharge must leave below kDalyOvervoltageV (#8: the live
// deployed 3.55V/100mV margin was already found to be one contributing
// factor to the #8 SMA cluster fault and must not be narrowed further).
constexpr float kMinChargeMarginV = 0.10f;

// NVS-persisted settings (loaded/saved by WebDashboard). Kept free of
// Arduino/FreeRTOS includes so the pure glideslope math in Glideslope.h,
// and the native unit tests that include it, can use it too.
struct SystemConfig {
    float maxChargeA;
    float maxDischargeA;
    float cvStartTaper;
    float cvMaxCharge;
    float cvStartDTaper;
    float cvMinDischarge;
    float cvHighAlarmGate;
    float cvLowAlarmGate;
    float trickleA;
    float limpDischargeA;
    int vSamples;
    int bmsTimeout;
    float cvMaintStart;
    float cvMaintStop;
    float maintAmps;

    // Raw (max-min) cell spread, in mV, at which current-limit derating
    // starts (#24) - below this the taper/full-current result is untouched.
    uint16_t spreadStartMv = 60;
    // Raw cell spread, in mV, at which derating bottoms out: the taper/
    // full-current result is forced down to trickle/limp current. Linear
    // in between spreadStartMv and spreadMaxMv.
    uint16_t spreadMaxMv = 150;

    // Sanity-check flags for validate() below (#53). Each bool is set
    // independently except hasNaN, which short-circuits every other check
    // (a NaN field makes ordering/margin comparisons meaningless, so there's
    // no point reporting spurious ordering violations alongside it).
    struct ValidationResult {
        bool hasNaN = false;
        // #12: the Winter Force Charge trigger (min cell < cvMaintStart)
        // must sit above the discharge floor, or the grid top-up can only
        // start once discharge is already cut.
        bool maintStartBelowMinDischarge = false;
        // Charge taper thresholds must be in strictly increasing voltage
        // order: full current below cvStartTaper, tapering down to trickle
        // at cvHighAlarmGate, hard cutoff at cvMaxCharge.
        bool chargeTaperOrderBad = false;
        // Discharge taper thresholds must be in strictly decreasing voltage
        // order: full current above cvStartDTaper, tapering down to limp at
        // cvLowAlarmGate, hard cutoff at cvMinDischarge.
        bool dischargeTaperOrderBad = false;
        // Maintenance hysteresis (#12): stop threshold must be above start,
        // or the mode would never release once triggered.
        bool maintHysteresisBad = false;
        // #8: cvMaxCharge must leave at least kMinChargeMarginV below the
        // Daly's own cell overvoltage protection.
        bool chargeHeadroomBad = false;
        // A negative current setpoint (trickle/limp/maintenance/full charge
        // or discharge amps) has no valid meaning - 0 A is allowed.
        bool hasNegativeCurrent = false;

        bool ok() const
        {
            return !hasNaN && !maintStartBelowMinDischarge && !chargeTaperOrderBad &&
                   !dischargeTaperOrderBad && !maintHysteresisBad && !chargeHeadroomBad &&
                   !hasNegativeCurrent;
        }
    };

    // Pure sanity check over one SystemConfig - no Arduino/FreeRTOS
    // dependency, so it runs under the native unit tests
    // (test/test_systemconfig/) same as Glideslope.h. Doesn't mutate cfg;
    // callers decide what to do with a failing result (WebDashboard's
    // saveConfig() refuses to persist it; loadConfig() only logs).
    static ValidationResult validate(const SystemConfig &cfg)
    {
        ValidationResult r;

        const float *floats[] = {
            &cfg.maxChargeA, &cfg.maxDischargeA, &cfg.cvStartTaper, &cfg.cvMaxCharge,
            &cfg.cvStartDTaper, &cfg.cvMinDischarge, &cfg.cvHighAlarmGate, &cfg.cvLowAlarmGate,
            &cfg.trickleA, &cfg.limpDischargeA, &cfg.cvMaintStart, &cfg.cvMaintStop,
            &cfg.maintAmps,
        };
        for (const float *f : floats) {
            if (isnan(*f)) {
                r.hasNaN = true;
                return r; // short-circuit: every other check is meaningless
            }
        }

        r.maintStartBelowMinDischarge = cfg.cvMaintStart <= cfg.cvMinDischarge;
        r.chargeTaperOrderBad =
            !(cfg.cvStartTaper < cfg.cvHighAlarmGate && cfg.cvHighAlarmGate < cfg.cvMaxCharge);
        r.dischargeTaperOrderBad =
            !(cfg.cvStartDTaper > cfg.cvLowAlarmGate && cfg.cvLowAlarmGate > cfg.cvMinDischarge);
        r.maintHysteresisBad = cfg.cvMaintStart >= cfg.cvMaintStop;

        // No tolerance on purpose: in IEEE binary32, 3.65f - 0.10f is
        // 3.5500002 and 3.55f is 3.5499999, so the live 3.550 V ceiling
        // passes a plain comparison and anything above it (e.g. 3.5505)
        // fails. Adding slack here would only raise the safety ceiling.
        r.chargeHeadroomBad = cfg.cvMaxCharge > (kDalyOvervoltageV - kMinChargeMarginV);

        r.hasNegativeCurrent = cfg.trickleA < 0 || cfg.limpDischargeA < 0 ||
                                cfg.maintAmps < 0 || cfg.maxChargeA < 0 || cfg.maxDischargeA < 0;

        return r;
    }
};
