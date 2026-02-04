//************************************************************************************
// WiFiSignalHelpers.h
//
// Module: WiFi Signal Quality Helpers
//
// Purpose:
//   - Convert WiFi RSSI to quality percentage
//   - Generate visual bar representation
//   - Provide consistent WiFi signal display
//
// Summary:
//   - getWiFiSignalQuality(): Returns 0–100% signal quality based on RSSI (dBm)
//   - getWiFiSignalBars(): Returns a 5-character string of signal bars
//
// Behavior:
//   - Quality mapping: ≤ -100 dBm → 0%; ≥ -50 dBm → 100%; linear in between
//   - When disconnected, returns 0% quality
//   - Bar representation uses Unicode block characters
//
// Notes:
//   - These functions automatically handle disconnected state
//   - Suitable for both LCD and web UI display
//************************************************************************************

#ifndef WIFI_SIGNAL_HELPERS_H
#define WIFI_SIGNAL_HELPERS_H

#include <Arduino.h>
#include <WiFi.h>

//************************************************************************************
// Function Declarations
//************************************************************************************

// Calculate WiFi signal quality as a percentage (0–100%) from RSSI (dBm)
int getWiFiSignalQuality();

// Generate a 5-character string representing WiFi signal strength using visual bars
String getWiFiSignalBars();

#endif // WIFI_SIGNAL_HELPERS_H
