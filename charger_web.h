//************************************************************************************
// charger_web.h
//
// Web UI + JSON API interface for ESP32 Universal CC/CV Charger Controller.
//
// - Declares web server setup, handlers, and helpers
// - Exposes HTML & JSON builders used by the UI
// - Provides extern access to system state and helpers
// - Contains NO implementation code
//
// Usage:
//   WebServer server(80);
//   setupWebServer();          // setup()
//   setupWebServerRoutes();    // setup()
//   handleWebServer();         // loop()
//
// Implementation:
//   charger_web.cpp
//   charger_web_routes.cpp
//

//************************************************************************************

#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "controller_types.h"
#include "battery_types.h"

//*****************************************************************************
// Compile-time configuration knobs
//*****************************************************************************
#ifndef LOAD_ACTIVE_HIGH
#define LOAD_ACTIVE_HIGH 1
#endif

#ifndef CSRF_ALLOW_NULL_ORIGIN
#define CSRF_ALLOW_NULL_ORIGIN 1
#endif

#ifndef MDNS_HOST
#define MDNS_HOST "charger"
#endif

#ifndef BULK_MODE_LABEL
#define BULK_MODE_LABEL "(BULK)"
#endif

#ifndef INVERT_LOAD_LABELS
#define INVERT_LOAD_LABELS 0
#endif

// HTML buffer sizing - adjust based on ESP32 variant memory
#ifndef HTML_RESERVE_SIZE
#define HTML_RESERVE_SIZE 10400
#endif

#ifndef JSON_RESERVE_SIZE
#define JSON_RESERVE_SIZE 960
#endif

//*****************************************************************************
// External state variables (provided by main sketch)
//*****************************************************************************
extern float bat_voltage, input_voltage, Current_A, temperatureC; 
extern int pwm_value;
extern String mode_str, load_status, loadControlModeStr;
extern int getBatteryPercentage(float voltage);
extern bool load_is_on;
extern int load_enable;
extern WebServer server;
extern LoadControlMode loadControlMode;
extern BatteryParams currentBattery;

// External helper functions (implemented in sketch)
extern int getWiFiSignalQuality(); // returns 0..100 (%)
extern void persistLoadMode();     // persist loadControlMode/label to NVS/Preferences

//*****************************************************************************
// Global system state tracking variables (declared in charger_web.cpp)
//*****************************************************************************
extern uint32_t bootTimeMs;
extern String wifiSSID;
extern String localIP;
extern uint32_t lastNetworkUpdate;

//*****************************************************************************
// Function Declarations
//*****************************************************************************

// System Time Management
void initUptime();
int getUptimeSeconds();
String formatUptimeString();

// Utility Functions
String getWiFiBars(int q);
String formatCurrentStr();
int dutyPercentCapAware();
String htmlEscape(const String& s);
String jsonEscape(const String& s);
String encToStr(uint8_t auth);
void applyLoad(bool on);
String formatLoadText(bool isOn, bool isManual);

// Battery Status Functions
String getBatteryStatusText(int soc);
const char* getBatteryStatusCode(int soc);

// Security Functions
void addSecurityHeaders();
String hostOnlyFromURL(const String& url);
String stripPortLower(const String& hostHeader);
bool csrfOK();

// Network Information Management
void updateNetworkInfo();

// Page Builders
String makeWiFiHTML();
String makeHTML();
String makeJSON();

// Web Server Management
void setupWebServer();          // legacy / optional wrapper
void setupWebServerRoutes();   // ROUTES ONLY (charger_web_routes.cpp)
void handleWebServer();
