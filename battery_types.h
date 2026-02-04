//************************************************************************************
// battery_types.h
//
// Module: Battery chemistry profiles for CC/CV charging (12V nominal)
//
// Purpose: Centralized configuration for safe, chemistry-specific charge control.
//          Supports Lead-Acid, LiFePO₄, and 3S Li-ion batteries.
//
// Key Features:
//   - Enum order preserved: critical for Preferences storage — never reorder!
//   - Per-chemistry voltage/current targets (CC Bulk → CV Absorption → Float).
//   - Hard overvoltage cutoffs to prevent catastrophic failure.
//   - Per-chemistry hysteresis and safety limits for stable mode transitions.
//   - Temperature compensation disabled; temperature used only for fan control.
//   - Human-readable names for UI/display.
//   - Battery-specific timing parameters for proper charging cycles.
//   - Battery-specific idle timing for optimal energy management.
//   - Battery-specific float exit timing for stable float mode transitions.
//
// Notes:
//   - DO NOT modify the order of BatteryType after deployment.
//   - All voltages are measured at the battery terminals (via ADS1115 ADC).
//   - Voltage divider ratio: (R1+R2)/R2 ≈ 6.238 → ADC × 6.238 = actual voltage.
//   - Values are conservative for long-term cell health and safety.
//   - Always verify against manufacturer datasheets before changing.
//
// ⚠️ WARNING: Incorrect settings can damage batteries or cause fire.
//            Never modify without understanding the chemistry risks.
//************************************************************************************

#ifndef BATTERY_TYPES_H
#define BATTERY_TYPES_H

#include <cstddef>

//************************************************************************************
// Enum: Supported battery types
//   Order is CRITICAL — stored in Preferences. NEVER change after deployment.
//************************************************************************************
enum BatteryType {
  LEAD_ACID_12V_7AH,   // 12V SLA/VRLA, ~7Ah class
  LIFEPO4_12V,         // 4S LiFePO₄ (12.8V nominal)
  LIION_3S             // 3S Li-ion/NMC (11.1V nominal)
};

//************************************************************************************
// Structure: Charging parameters per battery chemistry
//   All voltages: measured at battery terminals (not input side).
//   Currents: maximum allowed during each phase (in amps).
//   No temperature compensation applied — temperature used only for fan control.
//************************************************************************************
struct BatteryParams {
  const char* name;                // Display name (e.g., "LiFePO4 12V")
  
  // Core charging voltages
  float bulk_voltage_max;          // Max voltage in Bulk (CC) phase
  float bulk_voltage_min;          // Min voltage to enter Bulk phase
  float absorption_voltage;        // CV target in Absorption phase
  float float_voltage_max;         // Max float voltage (maintain)
  float float_voltage_min;         // Min float voltage (exit float if below)
  float battery_voltage_min;       // Minimum safe voltage (load cut-off)
  float input_voltage_min;         // Min Input voltage to enable charging
  
  // Safety limits (battery-specific)
  float overvoltage_cutoff;        // Hard stop: PWM disabled if exceeded
  float bulk_safety_limit;         // Maximum bulk voltage limit (hard clamp)
  float float_safety_limit;        // Maximum float voltage limit (hard clamp)
  
  // Hysteresis parameters (battery-specific)
  float voltage_hysteresis;        // Voltage hysteresis for mode transitions
  float current_hysteresis;        // Current hysteresis for mode transitions
  
  // Current limits
  float bulk_current;              // Max current in Bulk phase (A) - CC mode
  float absorption_current;        // Max current in Absorption phase (A) - CV mode
  float float_current;             // Max current in Float phase (A)
  
  // Timing parameters
  unsigned long absorption_time_ms;    // Time to stay in absorption mode (ms)
  unsigned long min_mode_time_ms;      // Minimum time to stay in each charge mode (ms)
  unsigned long idle_entry_time_ms;    // Time to enter IDLE mode (ms)
  unsigned long idle_exit_time_ms;     // Time to exit IDLE mode (ms)
  unsigned long float_exit_time_ms;    // Time to exit float mode (ms) - NEW
};

//************************************************************************************
// Battery parameter lookup table
//   Order must match BatteryType enum exactly.
//   Used for indexing via enum value (e.g., batteryTable[type]).
//************************************************************************************
constexpr BatteryParams batteryTable[] = {
  {
    // LEAD ACID 12V (SLA/VRLA)
    // Critical: Lead-acid requires long absorption time for sulfation prevention
    // Conservative timing and moderate hysteresis for stable operation
    .name                     = "Lead Acid 12V",
    .bulk_voltage_max         = 14.2f,
    .bulk_voltage_min         = 13.6f,
    .absorption_voltage       = 14.1f,
    .float_voltage_max        = 13.5f,
    .float_voltage_min        = 13.2f,
    .battery_voltage_min      = 9.88f,
    .input_voltage_min        = 5.0f,
    
    // Safety limits
    .overvoltage_cutoff       = 14.8f,
    .bulk_safety_limit        = 14.4f,    // Conservative for lead-acid
    .float_safety_limit       = 13.6f,    // Conservative for float
    
    // Hysteresis
    .voltage_hysteresis       = 0.15f,    // Lead-acid can handle moderate hysteresis
    .current_hysteresis       = 0.20f,    // Moderate current hysteresis
    
    // Current limits
    .bulk_current             = 0.750f,
    .absorption_current       = 0.350f,
    .float_current            = 0.150f,
    
    // Timing parameters
    .absorption_time_ms       = 5400000UL,   // 90 minutes - CRITICAL for lead-acid
    .min_mode_time_ms         = 300000UL,    // 5 minutes - Prevents mode oscillation
    .idle_entry_time_ms       = 600000UL,    // 10 minutes - Conservative for stable float
    .idle_exit_time_ms        = 600000UL,    // 10 minutes - Match entry time
    .float_exit_time_ms       = 300000UL     // 5 minutes - Conservative float exit timing
  },

  {
    // LiFePO₄ 12V (4S pack)
    // Note: LiFePO4 has very flat voltage curve, needs tighter hysteresis
    // Moderate timing for energy efficiency
    .name                     = "LiFePO₄ 12V",
    .bulk_voltage_max         = 14.6f,
    .bulk_voltage_min         = 13.6f,
    .absorption_voltage       = 14.4f,
    .float_voltage_max        = 13.8f,
    .float_voltage_min        = 13.2f,
    .battery_voltage_min      = 9.88f,
    .input_voltage_min        = 5.0f,
    
    // Safety limits
    .overvoltage_cutoff       = 15.0f,
    .bulk_safety_limit        = 14.8f,    // Tighter for LiFePO4 safety
    .float_safety_limit       = 13.8f,    // Match float voltage max
    
    // Hysteresis
    .voltage_hysteresis       = 0.10f,    // Tighter hysteresis for flat voltage curve
    .current_hysteresis       = 0.15f,    // Tighter current hysteresis
    
    // Current limits
    .bulk_current             = 1.200f,
    .absorption_current       = 0.500f,
    .float_current            = 0.100f,
    
    // Timing parameters
    .absorption_time_ms       = 1200000UL,   // 20 minutes - For cell balancing
    .min_mode_time_ms         = 120000UL,    // 2 minutes - Stable transitions
    .idle_entry_time_ms       = 300000UL,    // 5 minutes - Moderate for energy saving
    .idle_exit_time_ms        = 300000UL,    // 5 minutes - Match entry time
    .float_exit_time_ms       = 120000UL     // 2 minutes - Moderate float exit timing
  },

  {
    // Li-ion 3S (NMC/LCO, ~11.1V nominal)
    // Fast charging, tight control needed for safety
    .name                     = "Li-ion 3S",
    .bulk_voltage_max         = 12.6f,
    .bulk_voltage_min         = 12.0f,
    .absorption_voltage       = 12.6f,
    .float_voltage_max        = 12.6f,
    .float_voltage_min        = 12.0f,
    .battery_voltage_min      = 9.88f,
    .input_voltage_min        = 5.0f,
    
    // Safety limits (tight for Li-ion safety)
    .overvoltage_cutoff       = 13.0f,
    .bulk_safety_limit        = 12.8f,    // Very tight for Li-ion
    .float_safety_limit       = 12.7f,    // Very tight for Li-ion
    
    // Hysteresis
    .voltage_hysteresis       = 0.08f,    // Very tight hysteresis for Li-ion
    .current_hysteresis       = 0.10f,    // Tight current hysteresis
    
    // Current limits
    .bulk_current             = 1.500f,
    .absorption_current       = 0.500f,
    .float_current            = 0.100f,
    
    // Timing parameters
    .absorption_time_ms       = 600000UL,    // 10 minutes - Fast absorption
    .min_mode_time_ms         = 60000UL,     // 1 minute - Quick response
    .idle_entry_time_ms       = 180000UL,    // 3 minutes - Fast for responsiveness
    .idle_exit_time_ms        = 180000UL,    // 3 minutes - Match entry time
    .float_exit_time_ms       = 60000UL      // 1 minute - Fast float exit timing
  }
};

//************************************************************************************
// Number of supported battery types
//************************************************************************************
constexpr size_t BATTERY_TYPE_COUNT = sizeof(batteryTable) / sizeof(batteryTable[0]);

//************************************************************************************
// Helper: Validate battery type index
//   Returns true if type is within valid range [0, BATTERY_TYPE_COUNT).
//************************************************************************************
inline bool isChemistrySupported(BatteryType type) {
  return static_cast<size_t>(type) < BATTERY_TYPE_COUNT;
}

//************************************************************************************
// Helper: Get battery parameters by type
//   Returns reference to struct; undefined behavior if invalid type.
//   Use isChemistrySupported() to check before calling.
//************************************************************************************
inline const BatteryParams& getBatteryParams(BatteryType type) {
  return batteryTable[static_cast<size_t>(type)];
}

#endif // BATTERY_TYPES_H
