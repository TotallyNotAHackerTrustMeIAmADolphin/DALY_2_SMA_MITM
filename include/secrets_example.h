// include/secrets_example.h
#pragma once
#include <IPAddress.h>

// RENAME THIS FILE TO secrets.h AND FILL IN YOUR WI-FI CREDENTIALS + NETWORK CONFIG.
// Everything in here is private to your install (home network layout included) -
// this file is gitignored, so keep all of it here rather than adding new
// hardcoded network details back into main.cpp or platformio.ini.
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// Static IP for the device itself - also update platformio.ini's upload_port
// to match if you rely on OTA uploads.
IPAddress local_IP(192, 168, 178, 56);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);   // Google DNS
IPAddress secondaryDNS(1, 1, 1, 1); // Cloudflare DNS