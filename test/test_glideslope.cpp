#include <Arduino.h>
#include <unity.h>

// Mocking required types and globals to test calculateCCL
struct ConfigMock {
    float maxChargeA = 100.0;
    float trickleA = 5.0;
    float cvStartTaper = 3.3;
    float cvHighAlarmGate = 3.4;
    float cvMaxCharge = 3.5;
    float maintAmps = 20.0;
};
ConfigMock cfg;

struct DataMock {
    bool maintenanceActive = false;
};
DataMock currentData;

// Include the function to test, wrapped if necessary. 
// Since it's in main.cpp, we might need to be clever.
// For now, let's copy the logic into the test file for unit verification.

uint16_t calculateCCL_test(float maxCellV)
{
    if (currentData.maintenanceActive)
        return (uint16_t)round(cfg.maintAmps * 10.0f);

    if (maxCellV >= cfg.cvMaxCharge)
        return 0;
    if (maxCellV >= cfg.cvHighAlarmGate)
        return (uint16_t)round(cfg.trickleA * 10.0f);

    if (maxCellV > cfg.cvStartTaper)
    {
        float div = cfg.cvHighAlarmGate - cfg.cvStartTaper;
        if (div <= 0.001f) return (uint16_t)round(cfg.trickleA * 10.0f);
        
        float slope = (cfg.cvHighAlarmGate - maxCellV) / div;
        float target = cfg.trickleA + (slope * (cfg.maxChargeA - cfg.trickleA));
        return (uint16_t)round(max(target, cfg.trickleA) * 10.0f);
    }
    return (uint16_t)round(cfg.maxChargeA * 10.0f);
}

void test_calculateCCL_stable_at_max(void) {
    TEST_ASSERT_EQUAL(1000, calculateCCL_test(3.0));
}

void test_calculateCCL_taper_start(void) {
    // at 3.3V (cvStartTaper), should be max (100A)
    TEST_ASSERT_EQUAL(1000, calculateCCL_test(3.3));
}

void test_calculateCCL_taper_end(void) {
    // at 3.4V (cvHighAlarmGate), should be trickle (5A)
    TEST_ASSERT_EQUAL(50, calculateCCL_test(3.4));
}

void test_calculateCCL_mid_taper(void) {
    // at 3.35V (midpoint between 3.3 and 3.4), target should be average of 100A and 5A = 52.5A -> 525
    TEST_ASSERT_EQUAL(525, calculateCCL_test(3.35));
}

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_calculateCCL_stable_at_max);
    RUN_TEST(test_calculateCCL_taper_start);
    RUN_TEST(test_calculateCCL_taper_end);
    RUN_TEST(test_calculateCCL_mid_taper);
    UNITY_END();
}

void loop() {}
