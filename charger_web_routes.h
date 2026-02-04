//************************************************************************************
// charger_web_routes.h
//
// Web Server Route Declarations for ESP32 Universal CC/CV Charger Controller.
//
// Purpose:
//   - Declares route setup function used by charger_web.cpp
//   - Separates route definitions from web page builders
//   - Provides clean interface for web server initialization
//
// Dependencies:
//   - charger_web.h (for HTML/JSON builders and helper functions)
//   - WebServer (ESP32 core)
//
// Usage:
//   Include this header in charger_web.cpp and call setupWebServerRoutes()
//   from setupWebServer() function.
//
// Implementation:
//   charger_web_routes.cpp contains the actual route handlers.
//
//   - Updated file descriptions for CC/CV charger
//************************************************************************************

#ifndef CHARGER_WEB_ROUTES_H
#define CHARGER_WEB_ROUTES_H

#include <Arduino.h>

//************************************************************************************
// Function Declarations
//************************************************************************************

// Setup all web server routes
void setupWebServerRoutes();

#endif // CHARGER_WEB_ROUTES_H
