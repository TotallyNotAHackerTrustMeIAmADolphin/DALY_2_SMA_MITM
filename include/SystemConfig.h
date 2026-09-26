#pragma once
#include <array>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum ParseResult
{
    PARSE_OK,
    PARSE_NOT_A_NUMBER,
    PARSE_OUT_OF_RANGE
};

// What NVS, /save and the /config page need from a setting, whatever its
// value type: its name, and its value and limits as numbers. Setting<T>
// below is the real thing; this base only lets SystemConfig::all() hand
// out every setting in one list.
class SettingBase
{
public:
    // How the value is stored in NVS: Preferences enforces the stored
    // type, so a key written with putUInt must be read with getUInt.
    enum Kind
    {
        KIND_FLOAT,
        KIND_INT,
        KIND_UINT16
    };

    const char *key() const { return key_; } // NVS key and /save field name - never rename (NVS)
    const char *label() const { return label_; }
    const char *unit() const { return unit_; }
    const char *step() const { return step_; } // HTML step attribute
    uint8_t decimals() const { return decimals_; } // display precision
    Kind kind() const { return kind_; }

    virtual double value() const = 0;
    virtual double min() const = 0;
    virtual double max() const = 0;
    virtual double def() const = 0;

    // Stores v if it is a number within [min, max] (and whole, for an
    // integer setting); otherwise leaves the value alone and returns false.
    // The one gate every value from NVS or the /config form goes through,
    // so a -60 can't wrap to 65476 (#61) and a NaN can't get in.
    virtual bool set(double v) = 0;
    virtual void reset() = 0; // back to the default
    // Copies only the value from the same setting of another SystemConfig
    // (see SystemConfig::operator=). o must be the same Setting<T> member.
    virtual void copyValueFrom(const SettingBase &o) = 0;

    // Parses a /save form value and set()s it. The whole string (spaces
    // around it aside) must be a number - an integer for an integer
    // setting - else PARSE_NOT_A_NUMBER; a number set() refuses is
    // PARSE_OUT_OF_RANGE. The value is only changed on PARSE_OK.
    ParseResult parse(const char *text)
    {
        while (*text == ' ')
            text++;
        if (*text == '\0')
            return PARSE_NOT_A_NUMBER;
        char *end = nullptr;
        errno = 0;
        double v = kind_ == KIND_FLOAT ? strtod(text, &end) : (double)strtol(text, &end, 10);
        bool overflow = errno == ERANGE;
        if (end == text)
            return PARSE_NOT_A_NUMBER;
        while (*end == ' ')
            end++;
        if (*end != '\0')
            return PARSE_NOT_A_NUMBER;
        if (overflow || !set(v))
            return PARSE_OUT_OF_RANGE;
        return PARSE_OK;
    }

protected:
    SettingBase(const char *key, const char *label, const char *unit, const char *step, uint8_t decimals, Kind kind)
        : key_(key), label_(label), unit_(unit), step_(step), decimals_(decimals), kind_(kind) {}
    ~SettingBase() = default;

private:
    const char *key_;
    const char *label_;
    const char *unit_;
    const char *step_;
    uint8_t decimals_;
    Kind kind_;
};

// Formats v (one of s's own value/min/max/def) at full precision for the
// post-save "[CFG] <label>: <old> -> <new> <unit>" change log - deliberately
// NOT the setting's display decimals (formatNumber() in WebDashboard.cpp
// rounds to those, e.g. 0 for an amps field, so 250 -> 252.5 would print as
// "253" and a tiny real change could print as unchanged). An integer kind
// prints as a plain integer.
//
// A float kind prints the *shortest* %g precision that round-trips v's
// binary32 value exactly: 7 significant digits is NOT enough (that was
// this function's first, wrong version - (float)500.0 != (float)500.00003,
// yet both print "500" with %.7g, so a real change could still log as
// "X -> X"). FLT_DECIMAL_DIG is 9: every binary32 value round-trips through
// decimal at 9 significant digits, so the search below always finds a
// precision <= 9 that works.
//
// A plain "first precision that round-trips" search isn't quite enough,
// though: %g itself switches to exponential form whenever its decimal
// exponent X is >= the precision P (not just for genuinely tiny/huge
// values) - so at low precision, a whole number like 250 or 1000
// round-trips exactly as "2.5e+02"/"1e+03" before precision ever reaches
// the point where %g would print it in fixed form. Settings only run up
// to a few thousand, so once precision is high enough to cover the
// integer part, %g stops using exponential and stays that way (adding
// more digits only appends past the decimal point) - the search below
// keeps going past the first round-trip while the candidate is still
// exponential, and prefers the first later precision whose round-trip
// no longer is, falling back to the very first round-trip only if no
// fixed-form one exists at precision <= 9 (a value small enough that %g's
// X < -4 rule makes exponential form unavoidable, e.g. an amps setting's
// 0.00005 -> "5e-05" - a correct, if unusual-looking, round-trip, left
// as-is). Ordinary values still print short: 250 -> "250",
// 252.0625 -> "252.0625", 3.5499999 (stored 3.55f) -> "3.55". %g itself
// strips trailing zeros (and a trailing '.'). Pure, snprintf-based, always
// NUL-terminated; out must be at least ~24 bytes.
inline void formatSettingValue(const SettingBase &s, double v, char *out, size_t outSize)
{
    if (s.kind() != SettingBase::KIND_FLOAT)
    {
        snprintf(out, outSize, "%ld", (long)v);
        return;
    }
    float target = (float)v;
    char firstRoundTrip[24] = "";
    for (int precision = 1; precision <= 9; precision++)
    {
        char candidate[24];
        snprintf(candidate, sizeof(candidate), "%.*g", precision, v);
        if ((float)strtod(candidate, nullptr) != target)
            continue;
        if (firstRoundTrip[0] == '\0')
            snprintf(firstRoundTrip, sizeof(firstRoundTrip), "%s", candidate);
        if (strchr(candidate, 'e') == nullptr && strchr(candidate, 'E') == nullptr)
        {
            snprintf(out, outSize, "%s", candidate);
            return;
        }
    }
    snprintf(out, outSize, "%s", firstRoundTrip);
}

template <typename T>
struct SettingKind;
template <>
struct SettingKind<float>
{
    static constexpr SettingBase::Kind value = SettingBase::KIND_FLOAT;
};
template <>
struct SettingKind<int>
{
    static constexpr SettingBase::Kind value = SettingBase::KIND_INT;
};
template <>
struct SettingKind<uint16_t>
{
    static constexpr SettingBase::Kind value = SettingBase::KIND_UINT16;
};

// One setting: its current value plus everything that describes it.
// Reads like a plain T (`cfg.maxChargeA * 0.5f`), so the glideslope math
// uses settings as numbers without knowing about any of this.
template <typename T>
class Setting : public SettingBase
{
public:
    Setting(const char *key, const char *label, const char *unit, T min, T max, T def,
            uint8_t decimals, const char *step)
        : SettingBase(key, label, unit, step, decimals, SettingKind<T>::value),
          value_(def), min_(min), max_(max), def_(def) {}

    operator T() const { return value_; }
    T get() const { return value_; }

    double value() const override { return value_; }
    double min() const override { return min_; }
    double max() const override { return max_; }
    double def() const override { return def_; }

    bool set(double v) override
    {
        if (!(v >= (double)min_ && v <= (double)max_)) // false for NaN too
            return false;
        if (kind() != KIND_FLOAT && v != floor(v))
            return false;
        // For a float, min/max are floats themselves, so rounding v to
        // float can't carry it across either limit.
        value_ = (T)v;
        return true;
    }
    void reset() override { value_ = def_; }
    void copyValueFrom(const SettingBase &o) override
    {
        value_ = static_cast<const Setting<T> &>(o).value_;
    }

    // Copy-constructing is fine (a SystemConfig copy copies each member
    // from the same member). Assigning is not: the implicit operator=
    // would also copy key/label/range, so `cfg.cvStartTaper =
    // cfg.cvMaxCharge` would turn one setting into another (NVS key "cmv")
    // and skip set(). Use set(), or assign whole SystemConfigs.
    Setting(const Setting &) = default;
    Setting &operator=(const Setting &) = delete;

#ifdef PIO_UNIT_TESTING
    // Bypasses the range check. Only compiled into native tests, which feed
    // the math values set() would refuse (NaN, negative amps, reversed
    // thresholds) to prove its own fail-safes. Firmware can't call it.
    void setUnchecked(T v) { value_ = v; }
#endif

private:
    T value_;
    T min_;
    T max_;
    T def_;
};

// NVS-persisted settings (loaded/saved by WebDashboard). Each member is a
// Setting: its NVS key, label, unit, min, max, default, display decimals
// and HTML step are declared right here, and set() enforces the range.
// Kept free of Arduino/FreeRTOS includes so the glideslope math and the
// native tests can use it. The ranges are sanity limits against typos and
// rolled-over values, not tuning advice.
struct SystemConfig
{
    //                     key      label                  unit   min                 max                                    default  dec  step
    Setting<float> maxChargeA{"ca", "Max Charge Amps", "A", 0.0f, kMaxCurrentA, 250.0f, 0, "any"};
    Setting<float> cvStartTaper{"cvt", "Start Taper Vpc", "V", kMinCellThresholdV, kDalyOvervoltageV, 3.375f, 3, "0.001"};
    Setting<float> cvHighAlarmGate{"cag", "Target Trickle Vpc", "V", kMinCellThresholdV, kDalyOvervoltageV, 3.425f, 3, "0.001"};
    Setting<float> trickleA{"ta", "Trickle Amps", "A", 0.0f, kMaxCurrentA, 2.0f, 1, "any"};
    // Max is the #8 headroom: Daly OVP minus the minimum margin. In binary32
    // 3.65f - 0.10f = 3.5500002 > 3.55f, so 3.550 passes and anything above
    // (e.g. 3.5505) is refused - no tolerance needed or wanted.
    Setting<float> cvMaxCharge{"cmv", "Max Charge Vpc", "V", kMinCellThresholdV, kDalyOvervoltageV - kMinChargeMarginV, 3.450f, 3, "0.001"};
    Setting<float> cvMaintStart{"cmsv", "Maint. Start Vpc", "V", kMinCellThresholdV, kDalyOvervoltageV, 3.030f, 3, "0.001"};
    Setting<float> cvMaintStop{"cmpp", "Maint. Stop Vpc", "V", kMinCellThresholdV, kDalyOvervoltageV, 3.220f, 3, "0.001"};
    Setting<float> maintAmps{"mam", "Maintenance Amps", "A", 0.0f, kMaxCurrentA, 20.0f, 0, "any"};
    Setting<float> maxDischargeA{"da", "Max Discharge Amps", "A", 0.0f, kMaxCurrentA, 500.0f, 0, "any"};
    Setting<float> cvStartDTaper{"cdvt", "Start Taper Vpc (D)", "V", kMinCellThresholdV, kDalyOvervoltageV, 3.100f, 3, "0.001"};
    Setting<float> cvLowAlarmGate{"clag", "Target Limp Vpc", "V", kMinCellThresholdV, kDalyOvervoltageV, 3.065f, 3, "0.001"};
    Setting<float> limpDischargeA{"ld_v2", "Limp Amps", "A", 0.0f, kMaxCurrentA, 15.0f, 0, "any"};
    Setting<float> cvMinDischarge{"cmdv", "Min Discharge Vpc", "V", kMinCellThresholdV, kDalyOvervoltageV, 3.000f, 3, "0.001"};
    Setting<int> vSamples{"vs", "Voltage Window", "samples", 1, kMaxVSamples, 12, 0, "1"};
    // A negative timeout used to become a ~49-day window in isFresh(),
    // disabling the stale-BMS 0 A fail-safe.
    Setting<int> bmsTimeout{"to", "BMS timeout", "s", 5, 600, 60, 0, "1"};
    // Raw (max-min) cell spread at which current-limit derating starts
    // (#24), and at which it bottoms out at trickle/limp current. Above
    // ~1000 mV a threshold can never be reached, which would switch the
    // derating off in effect. start >= full is allowed: spreadFactor()
    // treats it as a step.
    Setting<uint16_t> spreadStartMv{"sps", "Cell spread: start derating", "mV", 0, 1000, 60, 0, "1"};
    Setting<uint16_t> spreadMaxMv{"spm", "Cell spread: full derating", "mV", 0, 1000, 150, 0, "1"};

    static constexpr size_t kNumSettings = 17;

    // Every setting, in /config page order - for NVS load/save, /save
    // parsing and the page. A new member must be added here too:
    // test_systemconfig checks that all() covers every byte of
    // SystemConfig, so a member missing from the list fails the tests.
    std::array<SettingBase *, kNumSettings> all() { return list<SettingBase>(*this); }
    std::array<const SettingBase *, kNumSettings> all() const { return list<const SettingBase>(*this); }

    SystemConfig() = default;
    SystemConfig(const SystemConfig &) = default;
    // Settings can't be assigned one by one (see Setting), so assigning a
    // whole config copies each setting's value from the same setting.
    SystemConfig &operator=(const SystemConfig &o)
    {
        std::array<SettingBase *, kNumSettings> dst = all();
        std::array<const SettingBase *, kNumSettings> src = o.all();
        for (size_t i = 0; i < kNumSettings; i++)
            dst[i]->copyValueFrom(*src[i]);
        return *this;
    }

    // Each setting's own range is enforced by set(); these are the rules
    // that relate two settings, which no single range can express.
    struct ValidationResult
    {
        // #12: the Winter Force Charge trigger (min cell < cvMaintStart)
        // must sit above the discharge floor, or the grid top-up can only
        // start once discharge is already cut.
        bool maintStartBelowMinDischarge = false;
        // Charge side strictly increasing: full current below cvStartTaper,
        // tapering to trickle at cvHighAlarmGate, hard cutoff at cvMaxCharge.
        bool chargeTaperOrderBad = false;
        // Discharge side strictly decreasing: cvStartDTaper > cvLowAlarmGate
        // > cvMinDischarge.
        bool dischargeTaperOrderBad = false;
        // Maintenance hysteresis (#12): stop above start, or it never
        // releases.
        bool maintHysteresisBad = false;

        bool ok() const
        {
            return !maintStartBelowMinDischarge && !chargeTaperOrderBad && !dischargeTaperOrderBad &&
                   !maintHysteresisBad;
        }
    };

    // Pure, tested in test/test_systemconfig/. saveConfig() refuses a
    // failing result; loadConfig() logs it.
    static ValidationResult validate(const SystemConfig &cfg)
    {
        ValidationResult r;
        r.maintStartBelowMinDischarge = cfg.cvMaintStart <= cfg.cvMinDischarge;
        r.chargeTaperOrderBad =
            !(cfg.cvStartTaper < cfg.cvHighAlarmGate && cfg.cvHighAlarmGate < cfg.cvMaxCharge);
        r.dischargeTaperOrderBad =
            !(cfg.cvStartDTaper > cfg.cvLowAlarmGate && cfg.cvLowAlarmGate > cfg.cvMinDischarge);
        r.maintHysteresisBad = cfg.cvMaintStart >= cfg.cvMaintStop;
        return r;
    }

    // Bit i set iff before.all()[i]->value() != after.all()[i]->value() -
    // which settings actually changed across a /save, for WebDashboard's
    // post-save "[CFG] <label>: <old> -> <new> <unit>" log lines. Pure and
    // tested in test/test_systemconfig/.
    static uint32_t changedMask(const SystemConfig &before, const SystemConfig &after)
    {
        static_assert(kNumSettings <= 32, "changedMask() needs a wider return type");
        std::array<const SettingBase *, kNumSettings> b = before.all();
        std::array<const SettingBase *, kNumSettings> a = after.all();
        uint32_t mask = 0;
        for (size_t i = 0; i < kNumSettings; i++)
            if (b[i]->value() != a[i]->value())
                mask |= (uint32_t)1 << i;
        return mask;
    }

private:
    template <typename B, typename Self>
    static std::array<B *, kNumSettings> list(Self &c)
    {
        return {{&c.maxChargeA, &c.cvStartTaper, &c.cvHighAlarmGate, &c.trickleA, &c.cvMaxCharge,
                 &c.cvMaintStart, &c.cvMaintStop, &c.maintAmps,
                 &c.maxDischargeA, &c.cvStartDTaper, &c.cvLowAlarmGate, &c.limpDischargeA, &c.cvMinDischarge,
                 &c.vSamples, &c.bmsTimeout, &c.spreadStartMv, &c.spreadMaxMv}};
    }
};
