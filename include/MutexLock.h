#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Scoped FreeRTOS mutex take/give: takes the mutex with a timeout in the
// constructor and gives it back in the destructor, but only if the take
// succeeded - so no exit path can leak the lock or give one it never had.
//
//   if (MutexLock lock{dataMutex, kSomeTimeout}) { ...guarded work... }
//
// Keep the scope short: copy under the lock, log after it (other tasks
// wait on it with 10-20 ms timeouts).
class MutexLock
{
public:
    MutexLock(SemaphoreHandle_t mutex, TickType_t timeout)
        : mutex_(mutex), held_(xSemaphoreTake(mutex, timeout) == pdTRUE) {}
    ~MutexLock()
    {
        if (held_)
            xSemaphoreGive(mutex_);
    }

    MutexLock(const MutexLock &) = delete;
    MutexLock &operator=(const MutexLock &) = delete;

    explicit operator bool() const { return held_; }

private:
    SemaphoreHandle_t mutex_;
    bool held_;
};
