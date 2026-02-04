// ===== wifi_sys.h =====
//************************************************************************************
// wifi_sys.h
//
// Module: WiFi System & Network Management
// Project : ESP32 Universal CC/CV Charger Controller
//
// Summary:
//   Separated WiFi, mDNS, and Credentials management logic to declutter main code.

//************************************************************************************
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>

// Forward declarations - avoid including entire headers
class LiquidCrystal_I2C;
class Bounce;

//************************************************************************************
// Global State Flags (Defined in wifi_sys.cpp)
//************************************************************************************
extern bool mdnsStarted;
extern bool webStarted;

//************************************************************************************
// Function Declarations
//************************************************************************************

// Initialization and BACKUP
void setupNetwork();                    // Called from setup()
bool setupWiFiAndServices();            // Complete WiFi setup with mDNS and web server
void wifiMaintain();                    // Called from loop()
void mDNSMaintain();                    // Called from loop() for ESP8266 compatibility
void handleWiFiResetButton();           // Called from loop() to check button

// Connection Helpers
bool checkWiFiConnectivity();
void waitForWiFiConnection();           // Blocking (with timeout) wait used in setup
void stopNetworkingForPortal();
void resumeNetworkingAfterPortal();

// Credentials Management
void saveWiFiCredentials(const String& ssid, const String& password);
bool loadWiFiCredentials(String& ssid, String& password);
bool attemptFallbackCredentials();
void triggerWiFiSetup();                // Starts the captive portal (non-blocking v8.9)

// Display and Status
void displayWiFiStatus(bool connected); // Display WiFi status on LCD

// Helpers
void waitForDuration(unsigned long duration);
void showStatusWithDuration(unsigned long duration, bool success);
