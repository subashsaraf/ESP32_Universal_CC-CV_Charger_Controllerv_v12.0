// ===== wifi_sys.cpp =====
//************************************************************************************
// wifi_sys.cpp
//
// Module: WiFi System Implementation
// Project: ESP32 Universal CC/CV Charger Controller (v10.0)
//
// Summary:
//   Implementation of WiFi, mDNS, and Credential management.

//************************************************************************************
#include "wifi_sys.h"
#include "charger_web.h"      
#include "buzzer_sys.h"     // For buzzer functions
#include "controller_types.h" // For BeepPriority

// Include LCD and Bounce headers
#include <LiquidCrystal_I2C.h>
#include <Bounce2.h>

//====================================================================================
// EXTERNALS: Access global objects/functions defined in the main .ino file
//====================================================================================
extern LiquidCrystal_I2C lcd;
extern WebServer server;
extern Preferences preferences;
extern Bounce buttonWiFiReset;

// State variables defined in main
extern int pwm_value;

// Battery status for display
extern float bat_voltage;
extern float input_voltage;  
extern float Current_A;

// Helper functions defined in main
extern void writePWMClamped(int duty);
extern void feedWatchdog();
extern void updateBeepPattern();
// Note: BeepPriority is defined in controller_types.h

// Buzzer functions from buzzer_sys
extern void playStartupBeep(bool wifiConnected);
extern void playPowerOnBeep();

// Debug Macros (Matching main file)
#define ENABLE_SERIAL_DEBUG 1 
#if ENABLE_SERIAL_DEBUG
  #define DEBUG_PRINT(...) Serial.print(__VA_ARGS__)
  #define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
  #define DEBUG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
  #define DEBUG_PRINT(...)
  #define DEBUG_PRINTLN(...)
  #define DEBUG_PRINTF(fmt, ...)
#endif

//====================================================================================
// GLOBAL VARIABLES (Specific to this module)
//====================================================================================
bool mdnsStarted = false;
bool webStarted = false;

// Constants for Reset Button Logic
const unsigned long LONG_PRESS_DURATION = 8000;
const unsigned long PRE_TRIGGER_WARNING = 5000;
const unsigned long PORTAL_TIMEOUT_SECONDS = 180;
const unsigned long MESSAGE_DISPLAY_TIME = 5000;
const unsigned long STATUS_DISPLAY_TIME = 10000;
const unsigned long PRE_TRIGGER_BEEP_INTERVAL = 1000;

// Non-blocking delay structure (same as in main.ino)
struct NonBlockingDelay {
  unsigned long previousMillis;
  bool isComplete(unsigned long interval) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      return true;
    }
    return false;
  }
  void reset() {
    previousMillis = millis();
  }
};

//************************************************************************************
// Setup / Initialization
//************************************************************************************
void setupNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("charger");
  WiFi.begin();
}

//************************************************************************************
// Blocking Waits (Used in Setup/Portal)
//************************************************************************************
void waitForWiFiConnection() {
  unsigned long startTime = millis();
  const unsigned long timeoutMs = 15000;

  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

  while (millis() - startTime < timeoutMs) {
    int remainingSeconds = (timeoutMs - (millis() - startTime)) / 1000;
    lcd.setCursor(0, 1);
    lcd.print("Timeout in "); lcd.print(remainingSeconds); lcd.print("s    ");
    
    if (WiFi.status() == WL_CONNECTED) break;

    // Keep system alive
    buttonWiFiReset.update();
    updateBeepPattern();
    feedWatchdog();
    delay(100);
  }
}

void waitForDuration(unsigned long duration) {
  unsigned long startTime = millis();
  while (millis() - startTime < duration) {
    buttonWiFiReset.update();
    updateBeepPattern();
    feedWatchdog();
    delay(10);
  }
}

//************************************************************************************
// Display WiFi Status on LCD
//************************************************************************************
void displayWiFiStatus(bool connected) {
  lcd.clear();
  
  if (connected) {
    String ipStr = WiFi.localIP().toString();
    String ssidStr = WiFi.SSID();

    DEBUG_PRINTLN("WiFi: Connected");
    DEBUG_PRINTF("SSID: %s\n", ssidStr.c_str());
    DEBUG_PRINTF("IP: %s\n", ipStr.c_str());
    DEBUG_PRINTF("WiFi RSSI: %d dBm\n", WiFi.RSSI());

    lcd.setCursor(0, 3); lcd.print("http://charger.local");
    lcd.setCursor(0, 0); lcd.print("WiFi: Connected");
    lcd.setCursor(0, 1); lcd.print("SSID:"); lcd.print(ssidStr);
    lcd.setCursor(0, 2); lcd.print("IP:");   lcd.print(ipStr);

    // Non-blocking 10 second delay
    NonBlockingDelay connectedDelay;
    unsigned long connectStart = millis();
    while (millis() - connectStart < 10000) {
      if (connectedDelay.isComplete(100)) {
        updateBeepPattern();
        feedWatchdog();
      }
      delay(10);
    }
  } else {
    DEBUG_PRINTLN("WiFi: Failed!");
    lcd.setCursor(0, 0); lcd.print("WiFi: Failed!");
    lcd.setCursor(0, 1); lcd.print("No network found");
    lcd.setCursor(0, 3); lcd.print("WiFi:ERROR");

    // Non-blocking 5 second delay
    NonBlockingDelay failedDelay;
    unsigned long failStart = millis();
    while (millis() - failStart < 5000) {
      if (failedDelay.isComplete(100)) {
        updateBeepPattern();
        feedWatchdog();
      }
      delay(10);
    }

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Offline Mode");

    // Non-blocking 1 second delay
    NonBlockingDelay offlineDelay;
    unsigned long offlineStart = millis();
    while (millis() - offlineStart < 1000) {
      if (offlineDelay.isComplete(100)) {
        updateBeepPattern();
        feedWatchdog();
      }
      delay(10);
    }
  }
}

//************************************************************************************
// Complete WiFi and Services Setup
//************************************************************************************
bool setupWiFiAndServices() {
  bool wifiConnected = false;
  
  // Wait for WiFi connection
  waitForWiFiConnection();
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  
  // Display connection status
  displayWiFiStatus(wifiConnected);
  
  // Setup mDNS if connected
  if (wifiConnected) {
    if (MDNS.begin("charger")) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
  }
  
  // Play startup beep
  playStartupBeep(wifiConnected);
  
  // Wait for beep to complete
  delay(4000);
  
  // Setup web server if connected
  if (wifiConnected) {
    setupWebServer();
    webStarted = true;
  }
  
  return wifiConnected;
}

//************************************************************************************
// WiFi BACKUP (Call in Loop)
//************************************************************************************
void wifiMaintain() {
  static bool wasConnected = false;
  static unsigned long lastTry = 0;
  static unsigned long backoff = 5000;       // Start 5s
  static unsigned long maxBackoff = 120000;  // Max 2m
  static uint8_t softAttempts = 0;
  static unsigned long lastConnectedTime = 0;
  static bool forceReconnect = false;

  const bool connected = (WiFi.status() == WL_CONNECTED);
  const unsigned long now = millis();

  // 1. Connection Lost
  if (wasConnected && !connected) {
    DEBUG_PRINTLN("WiFi disconnected. Starting reconnection process...");
    lastTry = now;
    softAttempts = 0;
    forceReconnect = true;
  }

  // 2. Connected
  if (connected) {
    if (!wasConnected) {
      DEBUG_PRINTF("WiFi reconnected. IP: %s\n", WiFi.localIP().toString().c_str());
      lastConnectedTime = now;

      if (!webStarted) {
        setupWebServer();
        webStarted = true;
      }
      if (!mdnsStarted) {
        if (MDNS.begin("charger")) {
          MDNS.addService("http", "tcp", 80);
          mdnsStarted = true;
        }
      }
      // Reset logic
      backoff = 5000;
      softAttempts = 0;
      forceReconnect = false;
    }
    if (mdnsStarted) mDNSMaintain();
    wasConnected = true;
    return;
  }

  wasConnected = false;

  // 3. Reconnection Logic
  if (forceReconnect || (now - lastTry >= backoff)) {
    lastTry = now;
    
    // Hard Reset if stuck
    if (softAttempts >= 3 || backoff >= 30000) {
      DEBUG_PRINTLN("Performing hard WiFi reset...");
      WiFi.disconnect(false); 
      delay(100);
      WiFi.mode(WIFI_STA);
      
      String ssid, pass;
      if (loadWiFiCredentials(ssid, pass)) {
         WiFi.begin(ssid.c_str(), pass.c_str());
      } else {
         WiFi.begin();
      }

      softAttempts = 0;
      forceReconnect = false;
    } else {
      // Soft Reconnect
      DEBUG_PRINTLN("Attempting WiFi reconnection...");
      WiFi.reconnect();
      softAttempts++;
    }

    // Exponential Backoff
    if (backoff < maxBackoff) {
      backoff = min(backoff * 2, maxBackoff);
      backoff += random(1000, 3000); // Jitter
    }
    DEBUG_PRINTF("Next WiFi reconnect attempt in %lu ms\n", backoff);
  }
}

void mDNSMaintain() {
#if defined(ARDUINO_ARCH_ESP8266)
  MDNS.update();
#endif
}

//************************************************************************************
// Credentials Management
//************************************************************************************
void saveWiFiCredentials(const String& ssid, const String& password) {
  preferences.begin("wifi_config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.end();
  DEBUG_PRINTLN("WiFi credentials saved to flash storage");
}

bool loadWiFiCredentials(String& ssid, String& password) {
  preferences.begin("wifi_config", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  preferences.end();

  bool valid = (ssid.length() > 0 && ssid.length() <= 32) && (password.length() <= 64);
  if (valid) {
    DEBUG_PRINT("Loaded WiFi credentials - SSID: "); DEBUG_PRINTLN(ssid);
  } else {
    DEBUG_PRINTLN("No valid WiFi credentials found in storage");
  }
  return valid;
}

bool attemptFallbackCredentials() {
  String ssid, password;
  if (loadWiFiCredentials(ssid, password)) {
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long connectStart = millis();
    const unsigned long CONNECT_TIMEOUT = 10000;
    while (millis() - connectStart < CONNECT_TIMEOUT) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(500);
      feedWatchdog();
    }
  }
  return false;
}

bool checkWiFiConnectivity() {
  if (WiFi.status() != WL_CONNECTED) return false;
  IPAddress ip = WiFi.localIP();
  return ip.toString() != "0.0.0.0";
}

//************************************************************************************
// Network Helpers (Portal Control)
//************************************************************************************
void stopNetworkingForPortal() {
  if (webStarted) {
    server.stop();
    webStarted = false;
  }
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
  delay(50);
}

void resumeNetworkingAfterPortal() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  unsigned long t0 = millis();
  const unsigned long timeout = 15000UL;

  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout) {
    delay(100);
    if ((millis() - t0) % 1000 == 0) DEBUG_PRINT(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINTF("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
    if (!mdnsStarted) {
      if (MDNS.begin("charger")) {
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;
        DEBUG_PRINTLN("mDNS started successfully");
      }
    }
    if (!webStarted) {
      setupWebServer();
      webStarted = true;
      DEBUG_PRINTLN("Web server started successfully");
    }
  } else {
    DEBUG_PRINTLN("\nWiFi connection failed after portal configuration");
  }
}

//************************************************************************************
// Show Status with Duration
//************************************************************************************
void showStatusWithDuration(unsigned long duration, bool success) {
  unsigned long statusStart = millis();
  while (millis() - statusStart < duration) {
    unsigned long elapsed = millis() - statusStart;
    unsigned long remaining = (duration - elapsed) / 1000;

    lcd.setCursor(0, 3);
    lcd.print("Continue in "); 
    if (remaining < 10) lcd.print(" ");
    lcd.print(remaining); lcd.print("s ");

    buttonWiFiReset.update();
    updateBeepPattern();
    feedWatchdog();
    delay(100);
  }
}

//************************************************************************************
// Reset Button & Captive Portal Logic
// v8.9 Fix: Non-blocking WiFi portal to prevent watchdog deadlock
//************************************************************************************
void triggerWiFiSetup() {
  // 1. Initial UI
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("* WiFi Setup Mode *");
  lcd.setCursor(0, 1); lcd.print("SSID: ChargerController"); 
  lcd.setCursor(0, 2); lcd.print("http://192.168.4.1");
  lcd.setCursor(0, 3); lcd.print("Portal opening...");
  
  waitForDuration(MESSAGE_DISPLAY_TIME);

  // 2. Safety: Disable Charger
  pwm_value = 0; 
  writePWMClamped(0);
  
  // 3. Stop existing Nets
  stopNetworkingForPortal();

  // 4. Start Portal (NON-BLOCKING - v8.9 FIX)
  static WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SECONDS);
  wm.setBreakAfterConfig(true);
  wm.setConfigPortalBlocking(false); // [FIX v8.9] Non-blocking mode

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Portal Active!");
  lcd.setCursor(0, 1); lcd.print("Connect to SSID:");
  lcd.setCursor(0, 2); lcd.print("ChargerController"); 
  lcd.setCursor(0, 3); lcd.print("Timeout: "); lcd.print(PORTAL_TIMEOUT_SECONDS); lcd.print("s");

  wm.startConfigPortal("ChargerController"); 
  
  // [FIX v8.9] Non-blocking portal loop with watchdog feeding
  unsigned long portalStart = millis();
  bool configResult = false;
  
  while (millis() - portalStart < (PORTAL_TIMEOUT_SECONDS * 1000UL)) {
    wm.process(); // Process portal without blocking
    
    // Keep system alive
    feedWatchdog();
    updateBeepPattern();
    buttonWiFiReset.update();
    
    // Check if portal completed
    if (wm.getConfigPortalActive() == false) {
      configResult = true;
      break;
    }
    
    // Update timeout display
    int remaining = PORTAL_TIMEOUT_SECONDS - ((millis() - portalStart) / 1000);
    if (remaining >= 0) {
      lcd.setCursor(10, 3);
      if (remaining < 10) lcd.print(" ");
      lcd.print(remaining); lcd.print("s ");
    }
    
    delay(10);
  }
  
  // Force stop if still active
  if (wm.getConfigPortalActive()) {
    wm.stopConfigPortal();
  }

  // 5. Handle Result
  if (configResult) {
    saveWiFiCredentials(WiFi.SSID(), WiFi.psk());
  }

  lcd.clear();
  if (configResult) {
    lcd.setCursor(0, 0); lcd.print("WiFi Config Saved!");
    lcd.setCursor(0, 1); lcd.print("SSID:"); lcd.print(WiFi.SSID());
    lcd.setCursor(0, 2); lcd.print("Success!");
  } else {
    lcd.setCursor(0, 0); lcd.print("WiFi Setup Timeout!");
    lcd.setCursor(0, 1); lcd.print("Using prev settings");
    lcd.setCursor(0, 2); lcd.print("Continuing...");
  }

  showStatusWithDuration(STATUS_DISPLAY_TIME, configResult);
  resumeNetworkingAfterPortal();
  
  // Check and Restart
  if (configResult && checkWiFiConnectivity()) {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("WiFi: Connected");
      lcd.setCursor(0, 1); lcd.print("SSID:"); lcd.print(WiFi.SSID());
      lcd.setCursor(0, 2); lcd.print("IP:");   lcd.print(WiFi.localIP().toString());
      waitForDuration(MESSAGE_DISPLAY_TIME);
      ESP.restart();
  }
  
  if (!configResult) ESP.restart(); // Restart on timeout to ensure clean state
}

//************************************************************************************
// Handle WiFi Reset Button
//************************************************************************************
void handleWiFiResetButton() {
  buttonWiFiReset.update();
  static enum { IDLE, PRESSED_WAITING, PRE_TRIGGER_WARNING_STATE, TRIGGERED } buttonState = IDLE;
  static unsigned long pressStart = 0;
  static unsigned long lastBeepTime = 0;

  switch (buttonState) {
    case IDLE:
      if (buttonWiFiReset.fell()) {
        buttonState = PRESSED_WAITING;
        pressStart = millis();
        lastBeepTime = pressStart;
      }
      break;
      
    case PRESSED_WAITING:
      if (buttonWiFiReset.rose()) {
        buttonState = IDLE;
      } else if (millis() - pressStart > LONG_PRESS_DURATION) {
        buttonState = TRIGGERED;
        triggerWiFiSetup();
      } else if (millis() - pressStart > PRE_TRIGGER_WARNING) {
        if (millis() - lastBeepTime > PRE_TRIGGER_BEEP_INTERVAL) {
          lastBeepTime = millis();
        }
        buttonState = PRE_TRIGGER_WARNING_STATE;
      }
      break;
      
    case PRE_TRIGGER_WARNING_STATE:
      if (buttonWiFiReset.rose()) {
        buttonState = IDLE;
      } else if (millis() - pressStart > LONG_PRESS_DURATION) {
        buttonState = TRIGGERED;
        triggerWiFiSetup();
      } else {
        // Speed up
        unsigned long timeHeld = millis() - pressStart;
        if (timeHeld > 6000 && timeHeld < 7000) {
          if (millis() - lastBeepTime > 500) {
            lastBeepTime = millis();
          }
        } else if (timeHeld > 7000) {
           if (millis() - lastBeepTime > 200) {
            lastBeepTime = millis();
          }
        }
      }
      break;
      
    case TRIGGERED:
      buttonState = IDLE;
      break;
  }
}
