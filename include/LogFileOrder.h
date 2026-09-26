#pragma once

#include <cstring>

// Pure, natively-tested ordering for SDLogger::listLogFiles() (plain C
// strings, no Arduino dependency).
namespace LogFileOrder
{
    // boot_* files (written before NTP syncs) carry a boot ID, not a date.
    inline bool isBootName(const char *name)
    {
        return std::strncmp(name, "boot_", 5) == 0;
    }

    // Strict weak ordering, ascending: boot_* always sorts first (a plain
    // byte compare would otherwise put it after any digit-starting name),
    // then a byte compare within either group.
    inline bool isOlder(const char *a, const char *b)
    {
        bool aBoot = isBootName(a);
        bool bBoot = isBootName(b);
        if (aBoot != bBoot)
            return aBoot;
        return std::strcmp(a, b) < 0;
    }
}
