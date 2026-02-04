//************************************************************************************
// controller_types.h
//
// Module:
//   Controller-wide type definitions.
//
// Summary:
//   Central enum(s) shared across the project. Currently defines the load control
//   policy used by the charger and the web/LCD interfaces.
//
// Notes:
//   - Value order is persisted to Preferences (AUTO=0, MANUAL=1). Do not reorder.
//   - AUTO: load follows automatic policy (updateLoadStatus()).
//   - MANUAL: auto policy is bypassed; only user/manual toggles apply.
//************************************************************************************
#pragma once

//************************************************************************************
// Enum for load control mode (persisted as 0=AUTO, 1=MANUAL)
//************************************************************************************
enum LoadControlMode { AUTO, MANUAL };

//************************************************************************************
// Enum for Buzzer Beep Priority
//************************************************************************************
typedef enum {
  BEEP_PRIORITY_LOW = 0,
  BEEP_PRIORITY_NORMAL = 1,
  BEEP_PRIORITY_HIGH = 2,
  BEEP_PRIORITY_CRITICAL = 3
} BeepPriority;
