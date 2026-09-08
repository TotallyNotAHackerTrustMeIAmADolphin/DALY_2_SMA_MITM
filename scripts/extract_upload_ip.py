"""
PlatformIO pre-build script: derives OTA upload_port from the device's
static IP in include/secrets.h (local_IP), so that IP only has to live
in the one gitignored file instead of also being hardcoded (and
committed) here in the build config.

Falls back to whatever upload_port platformio.ini/CLI provide (or
PlatformIO's own error if none) when secrets.h is missing or doesn't
match - never hard-fails the build.
"""
Import("env")

import os
import re

secrets_path = os.path.join(env.subst("$PROJECT_DIR"), "include", "secrets.h")

if not os.path.isfile(secrets_path):
    print("[extract_upload_ip] include/secrets.h not found - copy include/secrets_example.h "
          "to include/secrets.h and fill it in. OTA upload_port not derived; pass "
          "--upload-port explicitly if uploading now.")
else:
    with open(secrets_path, "r") as f:
        content = f.read()

    match = re.search(
        r"IPAddress\s+local_IP\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)",
        content,
    )
    if match:
        ip = ".".join(match.groups())
        env.Replace(UPLOAD_PORT=ip)
        print("[extract_upload_ip] OTA upload_port set from secrets.h local_IP: %s" % ip)
    else:
        print("[extract_upload_ip] WARNING: could not find 'IPAddress local_IP(...)' in "
              "include/secrets.h - OTA upload_port not derived; pass --upload-port "
              "explicitly if uploading now.")
