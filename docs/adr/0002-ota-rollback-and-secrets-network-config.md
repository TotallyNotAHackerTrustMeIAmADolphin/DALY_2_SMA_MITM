# 0002 - OTA rollback confirmation and network config in secrets.h

## Status
Accepted.

## Context
Two operational decisions that shape the boot/flash workflow but don't fit
neatly under a single module:

1. Early firmware would OTA-flash and immediately mark the new image valid,
   so a crash loop in the new image had no way back except a USB reflash on
   site.
2. Network configuration (WiFi credentials, static IP, gateway, DNS) used to
   be partly hardcoded in `src/main.cpp`/`platformio.ini`, which meant
   redeploying to a different network meant editing tracked source files.

## Decisions

**OTA rollback.** `verifyRollbackLater()` (`src/Diagnostics.cpp`) overrides a
weak Arduino-core symbol to return `true`, so the bootloader does not mark a
freshly OTA-flashed image valid at boot. `loop()` calls
`Diagnostics::confirmImageIfReady(wifiUp, bmsUp)`, which confirms the image
(`esp_ota_mark_app_valid_cancel_rollback()`) the first time, at or after 2
minutes of uptime, that a check finds both WiFi and the BMS link up
(`include/RollbackConfirm.h`'s `decide()`) - a point-in-time check, not a
continuity requirement: WiFi/BMS don't need to have been up for the whole
2 minutes, only at the moment `loop()` happens to check past the deadline.
Any reset before confirmation - panic, WDT,
brownout, power cycle - makes the bootloader boot the previous image instead.
This only works on a device whose bootloader was USB-flashed from a core
with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (core 2.0.17 has it); a
USB-flashed image itself has OTA state "undefined" and is never rolled back.
While an image is pending confirmation, a second OTA is refused by the OTA
library itself (`esp_ota_begin` -> `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`).

**Network config in secrets.h.** All WiFi credentials and static network
config (`local_IP`, `gateway`, `subnet`, `primaryDNS`, `secondaryDNS`) live
in the gitignored `include/secrets.h`, copied from
`include/secrets_example.h`. `scripts/extract_upload_ip.py`, a PlatformIO
`pre:` build script, reads `local_IP` out of it to set `upload_port`
automatically for OTA. Nothing in `src/main.cpp` or `platformio.ini` may
hardcode a network value again - a redeploy to a different network is then
just an edit to `secrets.h`, never a tracked-source change.
