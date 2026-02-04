//************************************************************************************
// WiFiSignalHelpers.cpp
//
// Implementation of WiFi signal quality and visualization helpers.
//
// Purpose:
//   - Convert WiFi RSSI to quality percentage (0-100%)
//   - Generate visual bar representation of signal strength
//   - Provide consistent WiFi signal display across UI
//
// Dependencies:
//   - WiFi.h (ESP32 WiFi library)
//   - WiFiSignalHelpers.h (header declarations)
//
// Usage:
//   Call getWiFiSignalQuality() for numeric quality percentage
//   Call getWiFiSignalBars() for visual bar representation
//************************************************************************************

#include "WiFiSignalHelpers.h"

//************************************************************************************
// Calculate WiFi signal quality as a percentage (0–100%) from RSSI (dBm).
//************************************************************************************
int getWiFiSignalQuality() {
  // Check if WiFi is connected first
  if (WiFi.status() != WL_CONNECTED) {
    return 0; // 0% when disconnected
  }
  
  int rssi = WiFi.RSSI();
  
  // Clamp RSSI to expected range
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  
  // Linear mapping: -100 to -50 dBm -> 0 to 100%
  return 2 * (rssi + 100);
}

//************************************************************************************
// Generate a 5-character string representing WiFi signal strength using visual bars.
//************************************************************************************
String getWiFiSignalBars() {
  int quality = getWiFiSignalQuality();
  int bars = (quality * 5) / 100; // Convert to 0-5 bars
  
  // Create bar string
  String barStr = "";
  for (int i = 0; i < 5; i++) {
    if (i < bars) {
      barStr += "■"; // Solid block for filled bars
    } else {
      barStr += "□"; // Light block for empty bars
    }
  }
  
  return barStr;
}
