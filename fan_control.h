//************************************************************************************
// fan_control.h
//
// Fan Control Module for DOIT ESP32 DEVKIT V1 (38 PIN)
// Controls 2-pin 12V DC fans using PWM via low-side 2N2222 transistor switching
//
// Summary:
//   - LEDC peripheral generates PWM signal on configurable GPIO pin
//   - Linear temperature-to-PWM mapping with hysteresis and startup kick
//   - Smooth transitions using slew-rate limiting
//   - Power-on sequence for controlled fan startup
//
// Hardware Configuration (2N2222 Transistor Version):
//   - Fan Positive Lead (+12V) → External 12V power supply
//   - Fan Negative Lead → 2N2222 Collector
//   - 2N2222 Emitter → Ground (GND)
//   - 2N2222 Base ← ESP32 GPIO through 1kΩ current limiting resistor
//   - Flyback diode (Schottky SS14 recommended) across fan terminals
//   - Local decoupling capacitors near fan power connections
//
// 2N2222 Transistor Specifications:
//   - Maximum Continuous Collector Current: 500mA (verify fan current requirements)
//   - Power Dissipation: P = Ic × Vce(sat) ≈ Ic × 0.3V at saturation
//   - Required Base Current: Ic/10 to Ic/20 for proper saturation
//
// DOIT ESP32 DEVKIT V1 (38 PIN) Features:
//   - 2x 8-bit LEDC peripherals (16 channels total)
//   - PWM frequency range: ~1Hz to 40MHz
//   - Resolution up to 20 bits (frequency dependent)
//   - Built-in millis() timer for precise timing control
//
//************************************************************************************
#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include <Arduino.h>

//************************************************************************************
// PWM HARDWARE CONFIGURATION
// Defines LEDC channel, pin, frequency, and resolution for fan control
//************************************************************************************
#define FAN_PWM_PIN           18      // GPIO pin connected to 2N2222 base (fan switched ground)
#define FAN_PWM_CH            2       // LEDC channel number (0-15, avoid conflicts)
#define FAN_PWM_FREQ          25000   // PWM frequency in Hz (25kHz = inaudible operation)
#define FAN_PWM_RES           10      // PWM resolution in bits (10-bit = 0-1023 range)

//************************************************************************************
// TEMPERATURE CONTROL PARAMETERS
// Defines temperature thresholds and corresponding fan behavior
//************************************************************************************
#define FAN_TEMP_ON           42.0f   // Temperature to turn fan ON (°C)
#define FAN_TEMP_OFF          38.0f   // Temperature to turn fan OFF (°C)  
#define FAN_TEMP_MAX          55.0f   // Temperature for maximum fan speed (°C)
#define FAN_MIN_DUTY          400     // Minimum duty cycle for reliable fan operation (0-1023)
#define FAN_MAX_DUTY          1023    // Maximum duty cycle (100% speed)
#define FAN_STARTUP_KICK_MS   200     // Duration of startup kick in milliseconds
#define FAN_STARTUP_DUTY      1023    // Duty cycle during startup kick (maximum power)

//************************************************************************************
// FAN CONTROL API
// Public functions for initializing and controlling fan operation
//************************************************************************************
void setupFanPWM();                    // Initialize PWM hardware and fan control state
void updateFanPWM(float temperatureC); // Update fan speed based on temperature input
int getFanDuty();                      // Get current fan PWM duty cycle value

#endif
