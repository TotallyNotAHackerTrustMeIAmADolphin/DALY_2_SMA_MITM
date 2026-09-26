#pragma once

// Shared SystemConfig fixture for test_glideslope and test_statusframe (#73):
// both files' setUp() built the same charge/discharge taper shape by hand,
// with setUnchecked(), so nothing checked it was a config the firmware
// would accept - and it wasn't (test_statusframe's cvMaintStart=3.0V
// equalled cvMinDischarge and failed SystemConfig::validate(); see the
// nudge to 3.05V below). One definition, validated, here.
//
// Uses set() (not setUnchecked()) throughout: every value here is a normal,
// in-range config, not one of the out-of-range fail-safe inputs individual
// tests still build by hand with setUnchecked().

#include <unity.h>
#include "SystemConfig.h"

inline SystemConfig glideslopeTestConfig()
{
    SystemConfig cfg;
    TEST_ASSERT_TRUE(cfg.maxChargeA.set(100.0));
    TEST_ASSERT_TRUE(cfg.trickleA.set(5.0));
    TEST_ASSERT_TRUE(cfg.cvStartTaper.set(3.3));
    TEST_ASSERT_TRUE(cfg.cvHighAlarmGate.set(3.4));
    TEST_ASSERT_TRUE(cfg.cvMaxCharge.set(3.5));
    TEST_ASSERT_TRUE(cfg.maintAmps.set(20.0));

    TEST_ASSERT_TRUE(cfg.maxDischargeA.set(200.0));
    TEST_ASSERT_TRUE(cfg.limpDischargeA.set(15.0));
    TEST_ASSERT_TRUE(cfg.cvStartDTaper.set(3.2));
    TEST_ASSERT_TRUE(cfg.cvLowAlarmGate.set(3.1));
    TEST_ASSERT_TRUE(cfg.cvMinDischarge.set(3.0));

    TEST_ASSERT_TRUE(cfg.bmsTimeout.set(60));

    // Deliberately far from cvStartTaper/cvHighAlarmGate/cvMaxCharge so the
    // maintenance-hysteresis tests don't interact with the taper thresholds.
    // Per-cell thresholds (#12: compared directly against minCellSmoothedV,
    // no cell count involved): start 3.05V, stop 3.2V. 3.0V (cvMinDischarge)
    // itself fails SystemConfig::validate()'s
    // maintStartBelowMinDischarge rule, so this is nudged up 50mV rather
    // than reused as-is.
    TEST_ASSERT_TRUE(cfg.cvMaintStart.set(3.05));
    TEST_ASSERT_TRUE(cfg.cvMaintStop.set(3.2));

    // spreadStartMv=60, spreadMaxMv=150 - left at the SystemConfig defaults.

    TEST_ASSERT_TRUE(SystemConfig::validate(cfg).ok());
    return cfg;
}
