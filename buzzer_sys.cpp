//************************************************************************************
// buzzer_sys.cpp
//
// Module: System Buzzer Implementation
// Project: ESP32 CC/CV Charger Controller (v8.9)
//************************************************************************************
#include "buzzer_sys.h"
#include <Arduino.h>

//************************************************************************************
// Hardware Configuration
//************************************************************************************
const int BUZZER_PIN = 13;  // Active buzzer for alerts

//************************************************************************************
// Internal State Variables
//************************************************************************************
volatile bool buzzerState = false;
volatile unsigned long lastBuzzerChange = 0;
const unsigned long MIN_BUZZER_TOGGLE_INTERVAL = 30; // Min 30ms between toggles

//************************************************************************************
// Beep Pattern Structure
//************************************************************************************
struct BeepPattern {
  int count = 0, onMs = 0, offMs = 0, completed = 0;
  unsigned long lastChange = 0;
  bool active = false, inPause = false;
  BeepPriority priority = BEEP_PRIORITY_NORMAL;
  unsigned long patternStart = 0; // Track pattern start time for timeout protection
  bool forceImmediate = false;    // Flag for immediate execution (UI buttons)
};
BeepPattern beepPattern;

//************************************************************************************
// Setup / Initialization
//************************************************************************************
void setupBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  buzzOff(true); // Ensure off at boot
}

//************************************************************************************
// Enhanced Active Buzzer Control Functions
//************************************************************************************
void buzzOff(bool force) {
  if (force || buzzerState || (millis() - lastBuzzerChange >= MIN_BUZZER_TOGGLE_INTERVAL)) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
    lastBuzzerChange = millis();
  }
}

void buzzOn(bool force) {
  if (force || !buzzerState || (millis() - lastBuzzerChange >= MIN_BUZZER_TOGGLE_INTERVAL)) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerState = true;
    lastBuzzerChange = millis();
  }
}

// Function with blocking delay - uses FORCE to guarantee execution
// Use only for blocking operations (startup, errors), NOT for button UI
void ensureBeep(int onMs, int offMs) {
  buzzOn(true);  // Force ON
  unsigned long start = millis();
  while (millis() - start < (unsigned long)onMs) {
    delay(5);  // Non-blocking yield
  }
  buzzOff(true); // Force OFF
  start = millis();
  while (millis() - start < (unsigned long)offMs) {
    delay(5);  // Non-blocking yield
  }
}

//************************************************************************************
// Beep Pattern Engine
//************************************************************************************
void startBeepPattern(int n, int onMs, int offMs, BeepPriority priority, bool forceImmediate) {
  // Validate input parameters
  if (n <= 0 || onMs <= 0) return;

  // For critical patterns or UI feedback, force immediate execution state
  if (forceImmediate) {
    if (beepPattern.active) {
      buzzOff(true);
      beepPattern.active = false;
    }
    // Set up the pattern to run immediately
    beepPattern.count = n;
    beepPattern.onMs = onMs;
    beepPattern.offMs = offMs;
    beepPattern.completed = 0;
    beepPattern.lastChange = millis();
    beepPattern.active = true;
    beepPattern.inPause = false;
    beepPattern.priority = priority;
    beepPattern.patternStart = millis();
    beepPattern.forceImmediate = true;
    
    // Trigger hardware immediately
    buzzOn(true); 
    return;
  }

  // Prevent pattern spamming - minimum 100ms between same patterns
  static unsigned long lastPatternStart = 0;
  if (millis() - lastPatternStart < 100) return;

  // Clear any existing lower/equal priority patterns
  if (beepPattern.active && priority >= beepPattern.priority) {
    buzzOff(); // Stop current beep immediately
  }

  // Configure beep pattern parameters
  beepPattern.count = n;
  beepPattern.onMs = onMs;
  beepPattern.offMs = offMs;
  beepPattern.completed = 0;
  beepPattern.lastChange = millis();
  beepPattern.active = true;
  beepPattern.inPause = false;
  beepPattern.priority = priority;
  beepPattern.patternStart = millis();
  beepPattern.forceImmediate = forceImmediate;

  lastPatternStart = millis();
  // Start first beep immediately
  buzzOn();
}

void updateBeepPattern() {
  static unsigned long lastUpdate = 0;
  // Prevent too frequent updates to avoid timing conflicts
  if (millis() - lastUpdate < 5) return;
  lastUpdate = millis();

  // Exit if no active pattern
  if (!beepPattern.active) return;

  unsigned long now = millis();
  // Safety timeout - patterns shouldn't run longer than 5 seconds
  if (now - beepPattern.patternStart > 5000) {
    buzzOff(true);
    beepPattern.active = false;
    beepPattern.priority = BEEP_PRIORITY_NORMAL;
    return;
  }

  // Handle beep on/off timing
  if (!beepPattern.inPause) {
    // Check if beep on time has elapsed
    if (now - beepPattern.lastChange >= (unsigned long)beepPattern.onMs) {
      buzzOff(beepPattern.forceImmediate);
      beepPattern.inPause = true;
      beepPattern.lastChange = now;
      beepPattern.completed++;
    }
  } else {
    // Check if pause time has elapsed
    if (now - beepPattern.lastChange >= (unsigned long)beepPattern.offMs) {
      // Continue pattern if more beeps needed
      if (beepPattern.completed < beepPattern.count) {
        beepPattern.inPause = false;
        beepPattern.lastChange = now;
        buzzOn(beepPattern.forceImmediate);  // Start next beep
      } else {
        // Pattern complete
        buzzOff(beepPattern.forceImmediate);
        beepPattern.active = false;
        beepPattern.priority = BEEP_PRIORITY_NORMAL; // Reset priority
      }
    }
  }
}

void waitForBeepPattern(unsigned long maxWaitMs) {
  unsigned long startTime = millis();
  // Wait while pattern is active, but feed watchdog if external function available
  // (Note: In this isolated module we just delay, main loop handles WDT usually.
  //  To be safe, we just loop.)
  while (beepPattern.active && (millis() - startTime < maxWaitMs)) {
    updateBeepPattern();
    delay(10);
  }
}

// [NEW v8.9] Check if beep pattern is running (for Load Logic synchronization)
bool isBeepActive() {
  return beepPattern.active;
}

//************************************************************************************
// Standard System Alerts
//************************************************************************************
void playPowerOnBeep() {
  startBeepPattern(1, 120, 60, BEEP_PRIORITY_NORMAL);
}

void playStartupBeep(bool connected) {
  connected ?
    startBeepPattern(3, 120, 80, BEEP_PRIORITY_NORMAL)
  : startBeepPattern(2, 300, 150, BEEP_PRIORITY_NORMAL);
}

void playCriticalBeep() {
  startBeepPattern(3, 110, 60, BEEP_PRIORITY_CRITICAL);
}

void startLoadPreAlertBeep(bool turnOn) {
  turnOn ?
    startBeepPattern(4, 120, 90, BEEP_PRIORITY_NORMAL)   // Load ON: 4 short beeps
  : startBeepPattern(4, 180, 120, BEEP_PRIORITY_NORMAL); // Load OFF: 4 medium beeps
}
