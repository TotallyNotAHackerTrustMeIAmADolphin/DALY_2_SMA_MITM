// include/secrets_example.h
#pragma once
#include <IPAddress.h>

// RENAME THIS FILE TO secrets.h AND FILL IN YOUR WI-FI CREDENTIALS + NETWORK CONFIG.
// Everything in here is private to your install (home network layout included) -
// this file is gitignored, so keep all of it here rather than adding new
// hardcoded network details back into main.cpp or platformio.ini.
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// Static IP for the device itself - scripts/extract_upload_ip.py derives
// platformio.ini's OTA upload_port from local_IP automatically, so there's
// nothing else to update.
IPAddress local_IP(192, 0, 2, 56);
IPAddress gateway(192, 0, 2, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);   // Google DNS
IPAddress secondaryDNS(1, 1, 1, 1); // Cloudflare DNS