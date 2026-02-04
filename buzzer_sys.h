//************************************************************************************
// buzzer_sys.h
//
// Module: System Buzzer & Beep Pattern Engine
// Project: ESP32 CC/CV Charger Controller (v8.9)
//
// Summary:
//   Handles active buzzer control, non-blocking beep patterns, and system alerts.
//   Moved from main .ino for modularity.
//************************************************************************************
#ifndef BUZZER_SYS_H
#define BUZZER_SYS_H

#include <Arduino.h>
#include "controller_types.h" // For BeepPriority enum

//************************************************************************************
// Initialization
//************************************************************************************
void setupBuzzer();

//************************************************************************************
// Core Control Functions
//************************************************************************************
void buzzOn(bool force = false);
void buzzOff(bool force = false);
void ensureBeep(int onMs, int offMs); // Blocking - use carefully

//************************************************************************************
// Pattern Engine (Non-blocking)
//************************************************************************************
void startBeepPattern(int n, int onMs, int offMs, BeepPriority priority = BEEP_PRIORITY_NORMAL, bool forceImmediate = false);
void updateBeepPattern();
void waitForBeepPattern(unsigned long maxWaitMs);
bool isBeepActive(); // [NEW v8.9] Returns true if a pattern is currently running

//************************************************************************************
// Standard System Alerts
//************************************************************************************
void playPowerOnBeep();
void playStartupBeep(bool connected);
void playCriticalBeep();
void startLoadPreAlertBeep(bool turnOn);

#endif // BUZZER_SYS_H
