//************************************************************************************
// fan_control.cpp
//
// Advanced PWM fan control for DOIT ESP32 DEVKIT V1 (38 PIN)
// Implements hysteresis, startup kick, and slew-rate limiting for smooth operation
//
// Key Features:
//   - Hysteresis prevents rapid fan on/off cycling around temperature thresholds
//   - Startup kick provides initial full-power pulse for reliable fan spin-up
//   - Slew-rate limiting ensures smooth PWM duty cycle transitions
//   - Input validation protects against invalid temperature readings
//   - Configurable via constants in fan_control.h
//   - Power-on sequence: 5-second delay, then 0-100% speed increase over 10 seconds
//
// Power-on Sequence:
//   - 0-5 seconds: Fan OFF (0% speed)
//   - 5-15 seconds: Fan speed increases gradually from 0% to 100%
//   - After 15 seconds: Normal temperature-controlled operation begins
//
// Hardware Configuration:
//   - PWM Frequency: 25 kHz (inaudible for most fans)
//   - PWM Resolution: Defined in FAN_PWM_RES (typically 8-10 bits)
//   - PWM Pin: Defined in FAN_PWM_PIN (connect to fan control input)
//
// DOIT ESP32 DEVKIT V1 (38 PIN) Specific:
//   - Uses LEDC peripheral for PWM generation
//   - Channel and pin configuration via FAN_PWM_CH and FAN_PWM_PIN
//   - Built-in millis() timer for timing operations
//************************************************************************************

#include "fan_control.h"
#include <cmath>

//************************************************************************************
// Internal state variables tracking fan control logic and timing
//************************************************************************************
static int s_fanDuty = 0;                    // Current PWM duty cycle (0-FAN_MAX_DUTY)
static bool s_fanOn = false;                 // Fan operational state (true = running)
static bool s_inKick = false;                // Startup kick active flag
static unsigned long s_kickStart = 0;        // Timestamp when kick started
static bool s_inPowerOnSequence = true;      // Power-on sequence active flag
static unsigned long s_powerOnStart = 0;     // Timestamp when power-on began

//************************************************************************************
// Timing constants for power-on sequence and control behavior
//************************************************************************************
constexpr unsigned long INITIAL_DELAY_MS = 5000;      // Fan off duration at startup
constexpr unsigned long SPEED_UP_DURATION_MS = 10000; // Ramp-up period after delay
constexpr unsigned long POWER_ON_SEQUENCE_MS = 15000; // Total power-on sequence time
constexpr int FAN_SLEW_STEP = 5;                      // Duty cycle change per update

//************************************************************************************
// TIME_DIFF
//   Macro to safely calculate time differences accounting for millis() overflow
//   Returns: unsigned long time difference in milliseconds
//************************************************************************************
#define TIME_DIFF(current, previous) ((unsigned long)((current) - (previous)))

//************************************************************************************
// clampi()
//   Constrains integer value within specified range [lo, hi]
//   Parameters:
//     v  - value to constrain
//     lo - minimum allowed value
//     hi - maximum allowed value
//   Returns: constrained integer value
//************************************************************************************
static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

//************************************************************************************
// setupFanPWM()
//   Initializes ESP32 LEDC peripheral for fan PWM control
//   Configures PWM frequency, resolution, and pin assignment
//   Sets initial fan state to OFF (0% duty cycle)
//************************************************************************************
void setupFanPWM() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // v3.0+ API: Pin-based
    ledcAttach(FAN_PWM_PIN, FAN_PWM_FREQ, FAN_PWM_RES);
#else
    // Configure LEDC channel with frequency and resolution from fan_control.h
    ledcSetup(FAN_PWM_CH, FAN_PWM_FREQ, FAN_PWM_RES);
    
    // Connect PWM output to specified pin on the DOIT ESP32 board
    ledcAttachPin(FAN_PWM_PIN, FAN_PWM_CH);
#endif

    // Initialize all fan control state variables to default values
    s_fanDuty = 0;
    s_fanOn = false;
    s_inKick = false;
    s_inPowerOnSequence = true;
    s_powerOnStart = millis();
    
    // Ensure fan starts in completely OFF state
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(FAN_PWM_PIN, 0);
#else
    ledcWrite(FAN_PWM_CH, 0);
#endif
}

//************************************************************************************
// updateFanPWM()
//   Main fan control algorithm processing temperature input and updating PWM output
//   Implements power-on sequence, hysteresis, startup kick, and slew-rate limiting
//   v8.9 Fix: Reset filtered temperature on sensor disconnect
//   Parameters:
//     temperatureC - Current temperature reading in Celsius
//************************************************************************************
void updateFanPWM(float temperatureC) {
    // Validate temperature input to prevent incorrect fan behavior from bad readings
    // Rejects NaN, infinite, and out-of-range values (-55°C to 125°C sensor limits)
    if (!isfinite(temperatureC) || temperatureC < -55.0f || temperatureC > 125.0f) {
        // [FIX v8.9] Reset to safe default on invalid reading
        if (s_inPowerOnSequence == false) {
            s_fanDuty = 0;
            #if ESP_ARDUINO_VERSION_MAJOR >= 3
                ledcWrite(FAN_PWM_PIN, s_fanDuty);
            #else
                ledcWrite(FAN_PWM_CH, s_fanDuty);
            #endif
        }
        return;
    }

    // Execute power-on sequence if still active (first 15 seconds after startup)
    if (s_inPowerOnSequence) {
        // Calculate elapsed time since system power-on
        unsigned long elapsed = TIME_DIFF(millis(), s_powerOnStart);
        
        // Phase 1: Initial 5-second delay - Keep fan completely OFF
        if (elapsed < INITIAL_DELAY_MS) {
            s_fanDuty = 0;
            #if ESP_ARDUINO_VERSION_MAJOR >= 3
                ledcWrite(FAN_PWM_PIN, s_fanDuty);
            #else
                ledcWrite(FAN_PWM_CH, s_fanDuty);
            #endif
            return;
        } 
        // Phase 2: 10-second ramp-up period - Gradually increase fan speed
        else if (elapsed < POWER_ON_SEQUENCE_MS) {
            // Calculate progress through the speed ramp (0.0 to 1.0)
            unsigned long rampElapsed = elapsed - INITIAL_DELAY_MS;
            float progress = static_cast<float>(rampElapsed) / SPEED_UP_DURATION_MS;
            progress = fminf(fmaxf(progress, 0.0f), 1.0f);
            
            // Linear interpolation from 0% to 100% duty cycle
            s_fanDuty = lroundf(progress * FAN_MAX_DUTY);
            s_fanDuty = clampi(s_fanDuty, 0, FAN_MAX_DUTY);
            #if ESP_ARDUINO_VERSION_MAJOR >= 3
                ledcWrite(FAN_PWM_PIN, s_fanDuty);
            #else
                ledcWrite(FAN_PWM_CH, s_fanDuty);
            #endif
            return;
        } 
        // Phase 3: Transition to normal temperature-controlled operation
        else {
            s_inPowerOnSequence = false;
            s_fanOn = false;
            s_inKick = false;
        }
    }

    // Normal operation - Calculate target duty cycle based on temperature
    int target = 0;

    // Map temperature to PWM duty cycle with hysteresis protection
    if (temperatureC >= FAN_TEMP_MAX) {
        // Maximum temperature threshold reached - Run fan at full speed
        target = FAN_MAX_DUTY;
    } else if (temperatureC <= FAN_TEMP_OFF) {
        // Temperature below turn-off threshold - Shut down fan completely
        target = 0;
    } else if (temperatureC >= FAN_TEMP_ON) {
        // Temperature between turn-on and maximum - Linear speed scaling
        float frac = (temperatureC - FAN_TEMP_ON) / (FAN_TEMP_MAX - FAN_TEMP_ON);
        frac = fminf(fmaxf(frac, 0.0f), 1.0f);
        target = FAN_MIN_DUTY + lroundf(frac * (FAN_MAX_DUTY - FAN_MIN_DUTY));
        target = clampi(target, FAN_MIN_DUTY, FAN_MAX_DUTY);
    } else {
        // Hysteresis zone - Maintain current operational state to prevent oscillation
        target = s_fanOn ? FAN_MIN_DUTY : 0;
    }

    // Handle fan state transitions and startup kick activation
    if (target > 0 && !s_fanOn) {
        // Fan transitioning from OFF to ON - Initiate startup kick sequence
        s_fanOn = true;
        s_inKick = true;
        s_kickStart = millis();
    } else if (target == 0) {
        // Fan should be OFF - Disable any active startup kick
        s_fanOn = false;
        s_inKick = false;
    }

    // Apply startup kick if currently active (high-power pulse for reliable spin-up)
    if (s_inKick && (TIME_DIFF(millis(), s_kickStart) < FAN_STARTUP_KICK_MS)) {
        s_fanDuty = FAN_STARTUP_DUTY;
        #if ESP_ARDUINO_VERSION_MAJOR >= 3
            ledcWrite(FAN_PWM_PIN, s_fanDuty);
        #else
            ledcWrite(FAN_PWM_CH, s_fanDuty);
        #endif
        return;
    }

    // Gradually adjust duty cycle toward target using slew-rate limiting
    // Prevents abrupt speed changes that could cause mechanical stress or noise
    if (s_fanDuty < target) {
        // Increase duty cycle in small steps toward target value
        s_fanDuty = clampi(s_fanDuty + FAN_SLEW_STEP, 0, target);
    } else if (s_fanDuty > target) {
        // Decrease duty cycle in small steps toward target value
        s_fanDuty = clampi(s_fanDuty - FAN_SLEW_STEP, target, FAN_MAX_DUTY);
    }

    // Update PWM hardware only when duty cycle actually changes
    // Optimizes performance by avoiding unnecessary register writes
    static int lastDuty = -1;
    if (s_fanDuty != lastDuty) {
        #if ESP_ARDUINO_VERSION_MAJOR >= 3
            ledcWrite(FAN_PWM_PIN, s_fanDuty);
        #else
            ledcWrite(FAN_PWM_CH, s_fanDuty);
        #endif
        lastDuty = s_fanDuty;
    }
}

//************************************************************************************
// getFanDuty()
//   Returns current fan PWM duty cycle for monitoring or debugging purposes
//   Returns: int - Current duty cycle value (0 to FAN_MAX_DUTY)
//************************************************************************************
int getFanDuty() {
    return s_fanDuty;
}
