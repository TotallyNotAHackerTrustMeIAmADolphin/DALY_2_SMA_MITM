#pragma once
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>

// The Daly BMS's own cell overvoltage protection on this pack (#8, confirmed
// by the operator) - cvMaxCharge must stay below this with real margin, since
// a Daly-side trip resets the reported SOC to 100% and can produce a bus
// transient. Not a substitute for this firmware's own cutoff.
constexpr float kDalyOvervoltageV = 3.65f;
// Minimum margin cvMaxCharge must leave below kDalyOvervoltageV (#8: the live
// deployed 3.55V/100mV margin was already found to be one contributing
// factor to the #8 SMA cluster fault and must not be narrowed further).
constexpr float kMinChargeMarginV = 0.10f;

// Lowest cell-voltage threshold accepted anywhere: LiFePO4 must not be
// discharged below 2.5 V/cell (a "0.3" typo for 3.0 passes every ordering
// check, so it needs a range).
constexpr float kMinCellThresholdV = 2.50f;
// Upper bound for every current setpoint: catches "2500" for "250"; well
// above anything this pack/inverter uses.
constexpr float kMaxCurrentA = 1000.0f;
// Moving-average window; must match CellSmoother::MAX_SAMPLES
// (static_assert in main.cpp).
constexpr int kMaxVSamples = 20;

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

    struct ValidationResult;
    // Pure sanity check over one SystemConfig (defined below the field
    // table): every field against its own min/max in kConfigFields, plus the
    // rules that relate two fields. No Arduino/FreeRTOS dependency, tested in
    // test/test_systemconfig/. Callers decide what to do with a failing
    // result (WebDashboard's saveConfig() refuses it; loadConfig() logs it).
    static ValidationResult validate(const SystemConfig &cfg);
};

// One row per setting: THE place where a setting's NVS key / form field
// name, default, allowed range, display precision, step and label live.
// WebDashboard uses it for loadConfig() (defaults, and replacing a stored
// value outside the range), saveConfig() (parseConfigField() below) and the
// /config page (label, and the <input> with the same min/max/step, are
// generated from the row); validate() below checks every field against it.
// Add a setting = add a row, plus `<strong>!!LABEL_<key>!!</strong>` and
// `!!IN_<key>!!` in config_html (test_systemconfig fails otherwise). The ranges are sanity limits against typos and
// rolled-over values (-60 mV stored as 65476 once switched spread derating
// off, #61), not tuning advice.
struct ConfigField
{
    enum Kind
    {
        KIND_FLOAT,
        KIND_INT,
        KIND_UINT16
    };
    const char *key; // NVS key and /save form field name - never rename (NVS)
    Kind kind;
    size_t offset; // offsetof(SystemConfig, <member>)
    float def;
    float min; // inclusive
    float max; // inclusive
    uint8_t decimals; // /config display precision (floats)
    const char *step; // HTML step attribute
    const char *label; // the /config page shows this (<strong>!!LABEL_<key>!!</strong>)
    const char *unit;
};

constexpr ConfigField kConfigFields[] = {
    // key     kind                     member                                     default  min                 max                                    dec step     label                          unit
    {"ca", ConfigField::KIND_FLOAT, offsetof(SystemConfig, maxChargeA), 250.0f, 0.0f, kMaxCurrentA, 0, "any", "Max Charge Amps", "A"},
    {"cvt", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvStartTaper), 3.375f, kMinCellThresholdV, kDalyOvervoltageV, 3, "0.001", "Start Taper Vpc", "V"},
    {"cag", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvHighAlarmGate), 3.425f, kMinCellThresholdV, kDalyOvervoltageV, 3, "0.001", "Target Trickle Vpc", "V"},
    {"ta", ConfigField::KIND_FLOAT, offsetof(SystemConfig, trickleA), 2.0f, 0.0f, kMaxCurrentA, 1, "any", "Trickle Amps", "A"},
    // Max is the #8 headroom rule: Daly OVP minus the minimum margin. In
    // binary32 3.65f - 0.10f = 3.5500002 > 3.55f, so 3.550 passes and
    // anything above (e.g. 3.5505) fails - no tolerance needed or wanted.
    {"cmv", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMaxCharge), 3.450f, kMinCellThresholdV, kDalyOvervoltageV - kMinChargeMarginV, 3, "0.001", "Max Charge Vpc", "V"},
    {"cmsv", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMaintStart), 3.030f, kMinCellThresholdV, kDalyOvervoltageV, 3, "0.001", "Maint. Start Vpc", "V"},
    {"cmpp", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMaintStop), 3.220f, kMinCellThresholdV, kDalyOvervoltageV, 3, "0.001", "Maint. Stop Vpc", "V"},
    {"mam", ConfigField::KIND_FLOAT, offsetof(SystemConfig, maintAmps), 20.0f, 0.0f, kMaxCurrentA, 0, "any", "Maintenance Amps", "A"},
    {"da", ConfigField::KIND_FLOAT, offsetof(SystemConfig, maxDischargeA), 500.0f, 0.0f, kMaxCurrentA, 0, "any", "Max Discharge Amps", "A"},
    {"cdvt", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvStartDTaper), 3.100f, kMinCellThresholdV, kDalyOvervoltageV, 3, "0.001", "Start Taper Vpc (D)", "V"},
    {"clag", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvLowAlarmGate), 3.065f, kMinCellThresholdV, kDalyOvervoltageV, 3, "0.001", "Target Limp Vpc", "V"},
    {"ld_v2", ConfigField::KIND_FLOAT, offsetof(SystemConfig, limpDischargeA), 15.0f, 0.0f, kMaxCurrentA, 0, "any", "Limp Amps", "A"},
    {"cmdv", ConfigField::KIND_FLOAT, offsetof(SystemConfig, cvMinDischarge), 3.000f, kMinCellThresholdV, kDalyOvervoltageV, 3, "0.001", "Min Discharge Vpc", "V"},
    {"vs", ConfigField::KIND_INT, offsetof(SystemConfig, vSamples), 12, 1, kMaxVSamples, 0, "1", "Voltage Window", "samples"},
    // A negative timeout used to become a ~49-day window in isFresh(),
    // disabling the stale-BMS 0 A fail-safe.
    {"to", ConfigField::KIND_INT, offsetof(SystemConfig, bmsTimeout), 60, 5, 600, 0, "1", "BMS timeout", "s"},
    // Above ~1000 mV a spread threshold can never be reached, which would
    // switch spread derating (#24) off in effect.
    {"sps", ConfigField::KIND_UINT16, offsetof(SystemConfig, spreadStartMv), 60, 0, 1000, 0, "1", "Cell spread: start derating", "mV"},
    {"spm", ConfigField::KIND_UINT16, offsetof(SystemConfig, spreadMaxMv), 150, 0, 1000, 0, "1", "Cell spread: full derating", "mV"},
};
constexpr size_t kNumConfigFields = sizeof(kConfigFields) / sizeof(kConfigFields[0]);
static_assert(kNumConfigFields <= 32, "ValidationResult::outOfRange is a 32-bit mask");

// A field's value as float (every int/uint16 setting fits exactly).
inline float configFieldValue(const SystemConfig &cfg, const ConfigField &f)
{
    const uint8_t *m = reinterpret_cast<const uint8_t *>(&cfg) + f.offset;
    switch (f.kind)
    {
    case ConfigField::KIND_INT:
        return (float)*reinterpret_cast<const int *>(m);
    case ConfigField::KIND_UINT16:
        return (float)*reinterpret_cast<const uint16_t *>(m);
    default:
        return *reinterpret_cast<const float *>(m);
    }
}

struct SystemConfig::ValidationResult
{
    // Bit i set: kConfigFields[i] is outside its [min, max] - or NaN/inf,
    // which fails the range comparison too.
    uint32_t outOfRange = 0;
    // Any field is NaN/inf. The rules below are skipped then: NaN compares
    // false, so they would only add spurious violations.
    bool hasNaN = false;

    // Rules that relate two fields (a per-field range can't express them):
    // #12: the Winter Force Charge trigger (min cell < cvMaintStart) must
    // sit above the discharge floor, or the grid top-up can only start
    // once discharge is already cut.
    bool maintStartBelowMinDischarge = false;
    // Charge side strictly increasing: full current below cvStartTaper,
    // tapering to trickle at cvHighAlarmGate, hard cutoff at cvMaxCharge.
    bool chargeTaperOrderBad = false;
    // Discharge side strictly decreasing: cvStartDTaper > cvLowAlarmGate >
    // cvMinDischarge.
    bool dischargeTaperOrderBad = false;
    // Maintenance hysteresis (#12): stop above start, or it never releases.
    bool maintHysteresisBad = false;

    bool ok() const
    {
        return outOfRange == 0 && !hasNaN && !maintStartBelowMinDischarge && !chargeTaperOrderBad &&
               !dischargeTaperOrderBad && !maintHysteresisBad;
    }
};

inline SystemConfig::ValidationResult SystemConfig::validate(const SystemConfig &cfg)
{
    ValidationResult r;

    for (size_t i = 0; i < kNumConfigFields; i++)
    {
        float v = configFieldValue(cfg, kConfigFields[i]);
        if (!isfinite(v))
            r.hasNaN = true;
        if (!(v >= kConfigFields[i].min && v <= kConfigFields[i].max))
            r.outOfRange |= (1u << i);
    }
    if (r.hasNaN)
        return r;

    r.maintStartBelowMinDischarge = cfg.cvMaintStart <= cfg.cvMinDischarge;
    r.chargeTaperOrderBad =
        !(cfg.cvStartTaper < cfg.cvHighAlarmGate && cfg.cvHighAlarmGate < cfg.cvMaxCharge);
    r.dischargeTaperOrderBad =
        !(cfg.cvStartDTaper > cfg.cvLowAlarmGate && cfg.cvLowAlarmGate > cfg.cvMinDischarge);
    r.maintHysteresisBad = cfg.cvMaintStart >= cfg.cvMaintStop;

    return r;
}

// Stores v into cfg's field if it is finite and within the row's
// [min, max]; otherwise leaves the field untouched and returns false.
// Integers are range-checked here, before they are narrowed, so nothing
// can wrap (-60 into a uint16_t was 65476, #61).
inline bool storeConfigField(SystemConfig &cfg, const ConfigField &f, double v)
{
    if (!(v >= (double)f.min && v <= (double)f.max)) // false for NaN too
        return false;
    uint8_t *m = reinterpret_cast<uint8_t *>(&cfg) + f.offset;
    switch (f.kind)
    {
    case ConfigField::KIND_INT:
        *reinterpret_cast<int *>(m) = (int)v;
        break;
    case ConfigField::KIND_UINT16:
        *reinterpret_cast<uint16_t *>(m) = (uint16_t)v;
        break;
    default:
        // min/max are floats themselves, so rounding v to float can't
        // carry it across either limit.
        *reinterpret_cast<float *>(m) = (float)v;
        break;
    }
    return true;
}

enum ParseResult
{
    PARSE_OK,
    PARSE_NOT_A_NUMBER,
    PARSE_OUT_OF_RANGE
};

// Parses one /save form value into cfg's field (#61). The old
// String::toInt()/toFloat() returned 0 for an empty or non-numeric field
// and toInt() into a uint16_t wrapped. Here the whole string (surrounding
// spaces aside) must be a number - an integer for int fields - and it must
// be within the row's range; otherwise the field is left untouched.
inline ParseResult parseConfigField(SystemConfig &cfg, const ConfigField &f, const char *str)
{
    while (*str == ' ')
        str++;
    if (*str == '\0')
        return PARSE_NOT_A_NUMBER;
    char *end = nullptr;
    errno = 0;
    double v;
    if (f.kind == ConfigField::KIND_FLOAT)
        v = strtod(str, &end);
    else
        v = (double)strtol(str, &end, 10);
    bool overflow = errno == ERANGE;
    if (end == str)
        return PARSE_NOT_A_NUMBER;
    while (*end == ' ')
        end++;
    if (*end != '\0')
        return PARSE_NOT_A_NUMBER;
    if (overflow || !storeConfigField(cfg, f, v))
        return PARSE_OUT_OF_RANGE;
    return PARSE_OK;
}
