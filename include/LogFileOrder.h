#pragma once

#include <cstring>

// Pure comparator for the ascending (oldest-first) order
// SDLogger::listLogFiles() returns its files in (#97). Extracted out of an
// untested lambda inside listLogFiles() so it has no Arduino dependency and
// test/test_logfileorder can include and run *this* code natively (`pio
// test -e native`), same pattern as TailTrim.h/CsvDecimation.h. Takes plain
// C strings, not Arduino String, for the same reason.
namespace LogFileOrder
{
    // boot_* fallback files are written before NTP has synced the clock
    // (see SDLogger::currentLogPath()), so their name carries a boot ID,
    // not a date.
    inline bool isBootName(const char *name)
    {
        return std::strncmp(name, "boot_", 5) == 0;
    }

    // Strict weak ordering: true if `a` sorts strictly before `b`. boot_*
    // fallback files always sort before dated YYYY-MM-DD files regardless
    // of their boot ID, since a plain byte compare would otherwise put
    // "boot_..." AFTER any digit-starting name ('b' > '0'-'9') - which
    // would wrongly rank a stale boot_ file as more recent than a properly
    // dated one and break "select most recent = last" (the dashboard's own
    // assumption, see index_html's `files[files.length - 1]`). Within the
    // same group (boot_* vs boot_*, or dated vs dated - which also covers
    // two files sharing a date but differing in extension, e.g. .csv vs
    // .log), falls back to a plain byte compare of the full name.
    inline bool isOlder(const char *a, const char *b)
    {
        bool aBoot = isBootName(a);
        bool bBoot = isBootName(b);
        if (aBoot != bBoot)
            return aBoot; // boot_* always sorts first
        return std::strcmp(a, b) < 0;
    }
}
