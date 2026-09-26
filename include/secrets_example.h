// include/secrets_example.h
#pragma once
#include <IPAddress.h>

// RENAME THIS FILE TO secrets.h AND FILL IN YOUR WI-FI CREDENTIALS + NETWORK CONFIG.
// Everything in here is private to your install (home network layout included) -
// this file is gitignored, so keep all of it here rather than adding new
// hardcoded network details back into main.cpp or platformio.ini.
//
// `static` (internal linkage), not a plain global: without it these are
// external-linkage definitions, and a second TU including this header
// would fail to link with a duplicate-symbol error (#109). The device
// build pins -std=gnu++11, which rules out C++17 `inline` variables - see
// DalyFrames::kAlarmBitNames() for the same reasoning elsewhere.
static const char *ssid = "YOUR_WIFI_SSID";
static const char *password = "YOUR_WIFI_PASSWORD";

// Static IP for the device itself - scripts/extract_upload_ip.py derives
// platformio.ini's OTA upload_port from local_IP automatically, so there's
// nothing else to update. Keep "IPAddress local_IP(" spelled exactly like
// this - extract_upload_ip.py regex-matches it.
static IPAddress local_IP(192, 0, 2, 56);
static IPAddress gateway(192, 0, 2, 1);
static IPAddress subnet(255, 255, 255, 0);
static IPAddress primaryDNS(8, 8, 8, 8);   // Google DNS
static IPAddress secondaryDNS(1, 1, 1, 1); // Cloudflare DNS