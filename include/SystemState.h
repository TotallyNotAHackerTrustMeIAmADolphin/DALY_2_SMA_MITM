#pragma once
#include <Arduino.h>

// Guards cross-core access to both SystemConfig (cfg) and DashboardData
// (currentData) - defined in main.cpp, created in setup() before any task
// that touches either struct is started.
extern SemaphoreHandle_t dataMutex;

// DashboardData itself (and the SystemConfig it pulls in) lives in its own
// header, free of Arduino/FreeRTOS dependencies (#32), so TelemetrySchema.h
// and its native unit tests can include and format it without the ESP32
// Arduino core. dataMutex above still needs Arduino.h for
// SemaphoreHandle_t, so it stays declared in this (Arduino-dependent)
// header rather than moving too.
#include "DashboardData.h"
