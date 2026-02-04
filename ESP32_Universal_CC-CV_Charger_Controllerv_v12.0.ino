//************************************************************************************
// Project : ESP32 Universal CC/CV Charger Controller
// Version : v12.0
// Date    : 2024-03-22
//
// Changelog:
//    
//
// Features:
//    - Full hysteresis-based state machine: BULK → ABSORPTION → FLOAT → BACKUP → IDLE → BULK
//    - Charge STATE logic (updateChargeMode) and PWM CONTROL logic (adjustPWM) separated
//    - All hysteresis values from battery_types.h (no magic numbers)
//    - Enforced minimum mode dwell time using existing timing infrastructure
//    - Overvoltage forces BACKUP with hard PWM shutdown
//    - IDLE exit requires both voltage + time hysteresis
//    - Hysteresis and safety limits in battery_types.h
//    - Battery-specific voltage/current hysteresis parameters
//    - Battery-specific bulk/float safety limits
//    - Convenience macros include new parameters
//    - CC/CV charging algorithm for stable DC input
//    - Simplified mode transitions for DC input charging
//    - All safety, UI, and hardware features maintained
//    - Web interface for CC/CV operation
//    - Battery-specific charging profiles preserved
//    - Universal DC Input (12-30V) via stable power supply
//    - Battery sensing via ADS1115 + ACS712 (auto calibration)
//    - Load control (AUTO / MANUAL) with protection
//    - DS18B20 thermal fan control
//    - 20x4 LCD + Web UI, WiFi + mDNS, audible alerts, 24h auto-reboot
//
// Hardware:
//    - MCU    : DOIT ESP32 DEVKIT V1 (38-pin)
//    - ADC    : ADS1115 @5V via bi-directional LLC (GAIN_TWOTHIRDS)
//    - Driver : IR2104 Half-Bridge Driver (Synchronous Buck)
//    - MOSFETs: High-Side N-CH + Low-Side N-CH
//    - Buzzer : Active Buzzer for audible notifications
//    - LLC    : I2C Bi-Directional Logic Level Converter (4-Channel)
//    - Display: 20x4 I2C LCD Screen
//
// Hardware Notes:
//    - INPUT/BAT sensed via shared divider
//      (100k / (17.5k) => (15k + 5k POT)) ~= 6.71x
//    - ACS712 requires zero-current calibration at startup
//    - I2C fixed at 200kHz with GPIO-based bus recovery
//    - PWM limited to user-defined % (70-85%) to ensure IR2104 bootstrap
//      capacitor recharge
//    - PWM resolution is 10-bit
//    - IR2104 SD (Shutdown) pin on GPIO 33
//      Logic: HIGH = Active, LOW = Shutdown (High-Z)
//      Shutdown occurs ONLY in IDLE/BACKUP modes after 10s delay
//    - LCD: 20x4 I2C display at address 0x27
//
//************************************************************************************



//************************************************************************************
// Configuration Flags  {Serial.print -> Enabled/Disabled}
//************************************************************************************
#define ENABLE_SERIAL_DEBUG 1  // 0 = Disabled, 1 = Enabled

//************************************************************************************
// Debug Print Macros
//************************************************************************************
#if ENABLE_SERIAL_DEBUG
#define DEBUG_PRINT(...) Serial.print(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DEBUG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#define DEBUG_PRINTLN(...)
#define DEBUG_PRINTF(fmt, ...)
#endif

//************************************************************************************
// Library Includes
//************************************************************************************
#include <Arduino.h>
#include <Wire.h>                // I2C communication library
#include <Adafruit_ADS1X15.h>    // ADS1115 ADC library
#include <LiquidCrystal_I2C.h>   // I2C LCD library
#include <math.h>                // Mathematical functions
#include <WiFi.h>                // WiFi connectivity library
#include <WiFiManager.h>         // Simplifies WiFi configuration and captive portal
#include <WebServer.h>           // ESP32 Web server library
#include <ESPmDNS.h>             // mDNS for hostname access
#include <Bounce2.h>             // Button debounce library
#include <OneWire.h>             // 1-Wire bus library
#include <DallasTemperature.h>   // DS18B20 temperature sensor library
#include <esp_task_wdt.h>        // ESP32 Task Watchdog Timer library
#include <Preferences.h>         // Flash storage for persistent settings

//************************************************************************************
// Application-specific headers
//************************************************************************************
#include "controller_types.h"
#include "battery_types.h"
#include "fan_control.h"
#include "charger_web.h"
#include "WiFiSignalHelpers.h"
#include "wifi_sys.h"
#include "buzzer_sys.h"

//************************************************************************************
// LEDC Compatibility Macros for ESP32 Core v3.0+
//************************************************************************************
#if ESP_ARDUINO_VERSION_MAJOR >= 3
// In Core v3.0, ledcAttach returns bool and takes (pin, freq, res). Channel is auto-managed.
#define COMPAT_LEDC_ATTACH(pin, channel, freq, res) ledcAttach(pin, freq, res)
// In Core v3.0, ledcWrite takes (pin, duty). Channel is ignored/deprecated.
#define COMPAT_LEDC_WRITE(pin, channel, duty) ledcWrite(pin, duty)
#else
// In Core v2.x, we must manually setup channel and attach pin.
#define COMPAT_LEDC_ATTACH(pin, channel, freq, res) do { ledcSetup(channel, freq, res); ledcAttachPin(pin, channel); } while(0)
// In Core v2.x, ledcWrite takes (channel, duty).
#define COMPAT_LEDC_WRITE(pin, channel, duty) ledcWrite(channel, duty)
#endif

//************************************************************************************
// Global Voltage Range Constants - Fallback Safety Values
//************************************************************************************
const float MIN_BATTERY_VOLTAGE = 10.5f; // Safe fallback for error conditions
const float MAX_BATTERY_VOLTAGE = 12.7f; // Safe fallback for error conditions

//************************************************************************************
// Global LCD Refresh Constants
//************************************************************************************
const int LCD_REFRESH_RATE_MS = 800; // LCD refresh interval in milliseconds
const unsigned long FLASH_CYCLE_MS = 2000UL; // LED flash cycle duration
const unsigned long FLASH_ON_DURATION_MS = 1000UL; // LED flash on duration

//************************************************************************************
// Watchdog Configuration for System Stability
//************************************************************************************
#define WATCHDOG_TIMEOUT 60    // Watchdog timeout in seconds
#define WATCHDOG_FEED_INTERVAL 8000  // Feed watchdog every 8 seconds (conservative)
unsigned long lastWatchdogFeed = 0;

//************************************************************************************
// Non-blocking Delay Structure for Smooth Operation
//************************************************************************************
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

// Create delay instances for different operations
NonBlockingDelay wifiDelay;
NonBlockingDelay sensorDelay;
NonBlockingDelay lcdDelay;
NonBlockingDelay beepDelay;
NonBlockingDelay logDelay;
NonBlockingDelay ipPrintDelay;
NonBlockingDelay watchdogDelay;
NonBlockingDelay eepromDelay; // Kept name for compatibility, used for persistence delays

//************************************************************************************
// Web Server and Global System Variables
//************************************************************************************
WebServer server(80); // Web server running on port 80
Preferences preferences; // Defined globally here for access across functions

// Sensor readings - consolidated global variables
float bat_voltage = 0.0, input_voltage = 0.0, Current_A = 0.0, temperatureC = 0.0;
int pwm_value = 0;
int pwm_last = 0; // Global tracking for soft-start logic

// System status variables
String mode_str = "(BULK)";
String load_status = "Off";
String loadControlModeStr = "A";
bool load_is_on = false;

// Pin assignments
int load_enable = 5;
LoadControlMode loadControlMode = AUTO;

//************************************************************************************
// SIMULATION_MODE  {Enabled/Disabled}
//************************************************************************************
#define SIMULATION_MODE 1  // 0 = real hardware, 1 = simulation

//************************************************************************************
// LCD and ADC Objects
//************************************************************************************
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_ADS1115 ads;

//************************************************************************************
// Custom LCD Icons (battery levels + power + PWM symbol)
//************************************************************************************
byte BATTERY_ICON[6][8] = {
  { 0b01110, 0b11011, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111 },
  { 0b01110, 0b11011, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111, 0b11111 },
  { 0b01110, 0b11011, 0b10001, 0b10001, 0b10001, 0b11111, 0b11111, 0b11111 },
  { 0b01110, 0b11011, 0b10001, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111 },
  { 0b01110, 0b11011, 0b10001, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111 },
  { 0b01110, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111 }
};

#define POWER_ICON 6
byte POWER_ICON_PATTERN[8] = {
  0b11111, 0b10101, 0b11111, 0b10101,
  0b11111, 0b10101, 0b11111, 0b00000
};

#define PWM_ICON 7
byte PWM_ICON_PATTERN[8] = {
  0b11101, 0b10101, 0b10101, 0b10101,
  0b10101, 0b10101, 0b10101, 0b10111
};

//************************************************************************************
// ADS1115 Channel Mapping (AIN0-AIN3)
//************************************************************************************
#define ADS_PIN_CURRENT_SENSOR    0    // AIN0 -> ACS712 Current Sensor Input
#define ADS_PIN_INPUT_VOLTAGE     2    // AIN2 -> DC Input Voltage Measurement
#define ADS_PIN_BATTERY_VOLTAGE   3    // AIN3 -> Battery Voltage Measurement

//************************************************************************************
// Current Sensor (ACS712) Configuration
//************************************************************************************
#define ACS712_VARIANT 5      // 5A module (0.185 V/A). Use 20 or 30 for others.
float ACS_SENSITIVITY = 0;    // Set dynamically in setup()
float MANUAL_OFFSET_CORRECTION = 0.0;

//************************************************************************************
// Voltage Divider Configuration for ADS1115 to Actual Voltage Conversion (0-30V Range)
//************************************************************************************
const float R1 = 100000.0; // Upper resistor value in ohms (100k)
const float R2 = 17500.0;  // Lower resistor value in ohms (17.5k) (15k + 5k POT )
const float dividerFactor = (R1 + R2) / R2; // Scale factor for voltage reading (~6.7143)

//************************************************************************************
// Battery Configuration and Convenience Macros
//************************************************************************************
BatteryType selectedBattery = LEAD_ACID_12V_7AH;
BatteryParams currentBattery = batteryTable[0];

// Core charging voltage macros
#define bulk_voltage_max       currentBattery.bulk_voltage_max
#define bulk_voltage_min       currentBattery.bulk_voltage_min
#define absorption_voltage_max currentBattery.absorption_voltage
#define float_voltage_max      currentBattery.float_voltage_max
#define float_voltage_min      currentBattery.float_voltage_min
#define battery_voltage_min    currentBattery.battery_voltage_min
#define input_voltage_min      currentBattery.input_voltage_min
#define OVERVOLTAGE_CUTOFF     currentBattery.overvoltage_cutoff

// Battery-specific safety limits (v10.5 addition)
#define BULK_SAFETY_LIMIT      currentBattery.bulk_safety_limit
#define FLOAT_SAFETY_LIMIT     currentBattery.float_safety_limit

// Battery-specific hysteresis parameters (v10.5 addition)
#define VOLTAGE_HYSTERESIS     currentBattery.voltage_hysteresis
#define CURRENT_HYSTERESIS     currentBattery.current_hysteresis

//************************************************************************************
// Load Protection Configuration
//************************************************************************************
#define LOAD_PROTECTION_CURRENT 3.5  // Amps - instant cutoff threshold
#define LOAD_OVERCURRENT_COUNT 3       // Number of consecutive readings to confirm overcurrent
#define LOAD_PROTECTION_RESET_TIME 5000UL  // 5 seconds auto-reset time ONLY

//************************************************************************************
// Global Load Protection Variables
//************************************************************************************
bool loadProtectionActive = false;
bool loadWasTripped = false;
unsigned long lastOvercurrentTime = 0;
int overcurrentCount = 0;
bool loadProtectionEnabled = true; // Can be toggled via web UI

// Hiccup Protection Counter
int protectionRetryCount = 0;
const int MAX_PROTECTION_RETRIES = 5; // Max retries before latching off
unsigned long lastRetryResetTime = 0; // Time tracking for retry counter reset
const unsigned long RETRY_RESET_TIMEOUT = 300000UL; // 5 minutes timeout for retry counter reset

//************************************************************************************
// GPIO Pin Assignments for DOIT ESP32 DEVKIT V1
//************************************************************************************
const int LED_CRITICAL  = 26; // Critical battery level LED
const int LED_MEDIUM    = 27; // Medium battery level LED
const int LED_FULL      = 32; // Full battery level LED

// BUZZER_PIN is now in buzzer_sys.cpp

const int PWM_out       = 25; // PWM output for charge control
const int BATTSTATUS_LED = 14; // Breathing battery status LED

// === IR2104 SAFETY UPDATE ===
// Shutdown (SD) pin assignment for IR2104
// HIGH = Active, LOW = Shutdown (High-Z)
const int DRIVER_SD_PIN = 33;
unsigned long sdShutdownTimer = 0; // Timer for delayed shutdown in IDLE/BACKUP

const int ledChannel    = 1;    // LEDC channel for breathing LED
const int pwmFrequency  = 1000; // PWM frequency for breathing LED
const int pwmResolution = 8;    // PWM resolution for breathing LED

//************************************************************************************
// User-Configurable PWM Safety Limit for IR2104 Bootstrap Safety
// Allowed range: 70% to 85% ONLY to ensure bootstrap capacitor recharge
// ============================================================================
#define PWM_FULL_SCALE_LIMIT 70   // USER VALUE [%] — MUST be 70 to 85

#if (PWM_FULL_SCALE_LIMIT < 70) || (PWM_FULL_SCALE_LIMIT > 85)
#error "PWM_FULL_SCALE_LIMIT must be between 70% and 85% for IR2104 safety"
#endif

#define PWM_FULL_SCALE   1023  // Full 10-bit scale (0-1023)
#define PWM_MAX_DUTY     ((PWM_FULL_SCALE * PWM_FULL_SCALE_LIMIT) / 100) // User-configurable limit

// === IR2104 PWM CONFIGURATION ===
// Standardized to 10-bit (0-1023) for finer logic
// User limit guarantees Bootstrap Refresh (Low-Side ON for [100-LIMIT]% of cycle)
#define PWM_CHANNEL 0
//#define PWM_FREQ 8000 // Max PWM_FREQ 5000
#define PWM_FREQ 20000   // 25 kHz (above human hearing)
#define PWM_RES  10   // 10-bit resolution (0-1023) to match safety limits
// PWM_MAX_DUTY is now calculated above based on user-defined percentage

//************************************************************************************
// Auto-reboot Configuration for System Stability
//************************************************************************************
#define REBOOT_INTERVAL 86400000UL // 24 hours in milliseconds

unsigned long rebootStartTime = 0;
bool rebootNotified = false;
uint32_t lastRebootTime = 0;

//************************************************************************************
// Charge Mode State Machine (Updated for CC/CV)
//************************************************************************************
enum ChargeMode { BULK, ABSORPTION, FLOAT, BACKUP, IDLE };
ChargeMode mode = BULK;

//************************************************************************************
// Safe PWM Write Helper (Updated for IR2104 with User-Configurable Limit)
//************************************************************************************
void writePWMClamped(int duty) {
  // === IR2104 CHANGE ===
  // Strict user-defined % clamp.
  // At max duty (PWM_MAX_DUTY), Low-Side is ON for [100-PWM_FULL_SCALE_LIMIT]% of cycle.
  // This guarantees bootstrap capacitor recharge every cycle.
  int hardware_duty = duty;

  // Global Safety Clamp using user-configurable limit
  if (hardware_duty > PWM_MAX_DUTY) {
    hardware_duty = PWM_MAX_DUTY;
  }

  // Additional safety clamp to prevent negative values
  if (hardware_duty < 0) {
    hardware_duty = 0;
  }

  // Safety: If we are actively driving PWM, ensure SD is HIGH immediately
  if (hardware_duty > 0) {
    digitalWrite(DRIVER_SD_PIN, HIGH);
    sdShutdownTimer = 0; // Reset any pending shutdown timer
  }

  COMPAT_LEDC_WRITE(PWM_out, PWM_CHANNEL, hardware_duty);
}

//************************************************************************************
// Driver SD (Shutdown) State Manager
// Handles the 10-second delay for turning off SD pin in IDLE/BACKUP modes
//************************************************************************************
void manageDriverSD() {
  // If we are in an ACTIVE charging mode (BULK, ABS, FLOAT) OR PWM is running
  if ((mode != IDLE && mode != BACKUP) || pwm_value > 0) {
    digitalWrite(DRIVER_SD_PIN, HIGH); // Force Driver Active
    sdShutdownTimer = 0; // Reset timer
  }
  // If we are in IDLE or BACKUP (and PWM is 0)
  else {
    if (sdShutdownTimer == 0) {
      sdShutdownTimer = millis(); // Start timer
    } else if (millis() - sdShutdownTimer > 10000UL) {
      digitalWrite(DRIVER_SD_PIN, LOW); // Turn OFF Driver after 10s
    }
    // If < 10s, do nothing (Pin remains in last state, typically HIGH)
  }
}

//************************************************************************************
// LCD Timing and Blinking Control
//************************************************************************************
unsigned long last_lcd_update = 0;
unsigned long last_flash_update = 0;
bool mode_visible = true;

//************************************************************************************
// Sensor Filtering Parameters (IIR Filter)
//************************************************************************************
unsigned long lastModeChange = 0;
float filteredTemp = 0.0;

// EMA Filter Response Times (1Hz update):
// ---------------------------------------
// 0.1 = 6.6 seconds to 50%, 22 seconds to 90%
// 0.2 = 3.1 seconds to 50%, 10.3 seconds to 90%
// 0.3 = 1.9 seconds to 50%, 6.5 seconds to 90%
// 0.5 = 1.0 seconds to 50%, 3.3 seconds to 90%
// ---------------------------------------
float ALPHA_V    = 0.3;  // 1.9s to 50%, 6.5s to 90% - Fast voltage tracking
float ALPHA_C    = 0.2;  // 3.1s to 50%, 10s to 90% - Smooth noisy ACS712 current
float ALPHA_TEMP = 0.3;  // 1.9s to 50%, 6.5s to 90% - Responsive temperature control

const float noiseThreshold = 0.05; // Minimum current threshold to filter noise
const float NOISE_HYSTERESIS = 0.01f; // Hysteresis for noise filtering

//************************************************************************************
// ADS1115 Status and ACS712 Reference Values
//************************************************************************************
bool ads_connected = false;
float ACS_OFFSET_VOLTAGE = 2.5; // Default ACS712 zero-current offset voltage

//************************************************************************************
// DS18B20 Temperature Sensor Configuration
//************************************************************************************
#define DS18B20_PIN 4  // OneWire bus pin for DS18B20
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);
DeviceAddress ds18b20Addr;
bool ds18b20Found = false;
bool tempValid = false;

//************************************************************************************
// Idle Mode Entry Criteria - NOW BATTERY-SPECIFIC
//************************************************************************************
unsigned long idleStartTime = 0;
const float   IDLE_CURRENT_THRESHOLD    = 0.10; // Current threshold for idle mode (A)

unsigned long idleVoltageDropStart = 0;

// Load control debounce parameters
const float battery_voltage_hysteresis = 0.2; // Voltage hysteresis for load switching
unsigned long lastLoadChangeTime = 0;
const unsigned long debounceDelay = 5000; // Load switch debounce delay (ms)

//************************************************************************************
// Buzzer Control
//************************************************************************************
bool pendingLoadChange = false;
bool pendingTurnOn = false;
// Beep Priority Enum is now in controller_types.h
// Core buzzer functions and patterns are now in buzzer_sys.h/cpp

//************************************************************************************
// Button Configuration with Bounce2 Library
//************************************************************************************
const int BUTTON_MODE       = 19; // Battery/load mode toggle button
const int BUTTON_LOAD       = 16; // Load on/off button
const int BUTTON_WIFI_RESET = 15; // WiFi reset/config button
const int BUTTON_CALIBRATE = 17; // Calibration button (startup only)

Bounce buttonMode;
Bounce buttonLoad;
Bounce buttonWiFiReset;
Bounce buttonCalibrate;

//************************************************************************************
// Temporary Manual Load Control in AUTO Mode
//************************************************************************************
bool tempLoadOn = false;
unsigned long tempLoadStart = 0;
const unsigned long TEMP_LOAD_DURATION = 30000; // 30 seconds temporary load duration
bool autoBeepPending = false;

//************************************************************************************
// AUTO-mode Beep Patterns for Load Change Alerts
//************************************************************************************
bool nextToggleIsManual = false;
const int  AUTO_PRE_BEEP_COUNT   = 6;   // Number of beeps before load change
const int  AUTO_PRE_BEEP_ON_MS   = 120; // Beep on duration (ms)
const int  AUTO_PRE_BEEP_OFF_MS  = 120; // Beep off duration (ms)

const unsigned long TEMP_OFF_WARN_LEAD_MS = 2000; // Warning time before temp load off
const int  AUTO_EXPIRY_BEEP_COUNT  = 6;   // Number of beeps for temp load expiry
const int  AUTO_EXPIRY_BEEP_ON_MS  = 120; // Expiry beep on duration (ms)
const int  AUTO_EXPIRY_BEEP_OFF_MS = 120; // Expiry beep off duration (ms)

//************************************************************************************
// Critical Battery Warning Threshold
//************************************************************************************
const float CRIT_BATT_WARN_MARGIN = 0.10f; // Voltage margin for critical battery warning

//************************************************************************************
// Surge current protection timing
//************************************************************************************
static unsigned long lastPWMChange = 0;
const unsigned long MIN_PWM_CHANGE_INTERVAL = 20;  // 20ms minimum between PWM changes
static int lastPWMValue = 0;
static float lastCurrent = 0.0f;

//************************************************************************************
// System Health Monitoring
//************************************************************************************
struct SystemHealth {
  uint32_t totalUptime;
  uint16_t watchdogResets;
  uint16_t i2cRecoveries;
  uint16_t overloadEvents;
  uint32_t lastHeapCheck;
  uint32_t minFreeHeap;
};

SystemHealth systemHealth = {0, 0, 0, 0, 0, UINT32_MAX};

//************************************************************************************
// Schedule Load Toggle with Pre-alert Beeps
//************************************************************************************
void scheduleLoadToggle(bool turnOn) {
  // Prevent multiple simultaneous load changes to avoid conflicts
  if (pendingLoadChange || isBeepActive()) return; // Added beep active check

  // Set up pending load change for deferred execution
  pendingLoadChange = true;
  pendingTurnOn = turnOn;
  // Start appropriate pre-alert beep pattern based on control mode
  (loadControlMode == AUTO) ?
  startBeepPattern(AUTO_PRE_BEEP_COUNT, AUTO_PRE_BEEP_ON_MS, AUTO_PRE_BEEP_OFF_MS, BEEP_PRIORITY_NORMAL) :
  startLoadPreAlertBeep(turnOn);
}

//************************************************************************************
// Execute Scheduled Load Toggle After Beep Pattern
//************************************************************************************
void finalizeLoadToggle() {
  // Atomic flag protection with interrupt disable for ESP32
  static volatile bool inFinalize = false;
  portDISABLE_INTERRUPTS();  // Disable interrupts for atomic operation
  if (inFinalize) {
    portENABLE_INTERRUPTS();
    return;
  }
  inFinalize = true;
  portENABLE_INTERRUPTS();

  // Apply the scheduled load state change to hardware
  digitalWrite(load_enable, pendingTurnOn ? HIGH : LOW);
  load_is_on = pendingTurnOn;
  load_status = load_is_on ? "On " : "Off";
  lastLoadChangeTime = millis();

  // Handle temporary load in AUTO mode with expiration logic
  if (loadControlMode == AUTO && load_is_on && nextToggleIsManual) {
    tempLoadOn = true;
    tempLoadStart = millis();
    autoBeepPending = false;
  } else {
    tempLoadOn = false;
    autoBeepPending = false;
  }

  // Reset toggle flags to prepare for next operation
  nextToggleIsManual = false;
  pendingLoadChange = false;

  portDISABLE_INTERRUPTS();
  inFinalize = false; // Release lock
  portENABLE_INTERRUPTS();
}

//************************************************************************************
// Battery Selection Menu at Startup (20-second maximum timeout)
// Uses non-blocking beep patterns for responsive UI
//************************************************************************************
void batterySelectionMenu() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Select Battery:");

  int menuIndex = selectedBattery;
  unsigned long menuStart = millis();
  bool selected = false;
  unsigned long lastButton1 = 0, lastButton2 = 0;
  int lastCountdown = 21;

  const int MENU_TIMEOUT_SEC = 20; // Maximum 20 seconds as requested
  const int pageSize = 3;

  // Menu loop with timeout and user interaction handling
  while (!selected && (millis() - menuStart < (unsigned long)MENU_TIMEOUT_SEC * 1000UL)) {
    buttonMode.update();
    buttonLoad.update();

    // Display menu page with pagination support
    int page = menuIndex / pageSize;
    int start = page * pageSize;
    for (int r = 0; r < pageSize; r++) {
      int idx = start + r;
      lcd.setCursor(0, r + 1);
      if (idx < BATTERY_TYPE_COUNT) {
        lcd.print(idx == menuIndex ? ">" : " ");
        lcd.print(batteryTable[idx].name);
        int len = strlen(batteryTable[idx].name);
        for (int j = len + 1; j < 19; j++) lcd.print(" ");
      } else {
        lcd.print("                    ");
      }
    }

    // Update countdown timer display
    int countdown = MENU_TIMEOUT_SEC - ((millis() - menuStart) / 1000);
    if (countdown != lastCountdown) {
      lcd.setCursor(15, 0); lcd.print("     ");
      lcd.setCursor(15, 0);
      lcd.print("[");
      if (countdown < 10) lcd.print(" ");
      lcd.print(countdown); lcd.print("s]");
      lastCountdown = countdown;
    }

    // Handle button presses with debounce and guaranteed beep (NON-BLOCKING)
    if (buttonMode.fell() && millis() - lastButton1 > 200) {
      menuIndex = (menuIndex + 1) % BATTERY_TYPE_COUNT;
      startBeepPattern(1, 80, 50, BEEP_PRIORITY_NORMAL, true); // Non-blocking force beep
      lastButton1 = millis();
    }
    if (buttonLoad.fell() && millis() - lastButton2 > 200) {
      selected = true;
      startBeepPattern(1, 150, 50, BEEP_PRIORITY_NORMAL, true); // Non-blocking force beep
      break;
    }

    updateBeepPattern();
    delay(10);
  }

  // Save selected battery or use previous with Preferences
  if (selected) {
    // Only write to Preferences if battery type actually changed
    if (selectedBattery != (BatteryType)menuIndex) {
      selectedBattery = (BatteryType)menuIndex;
      // Save to Preferences
      preferences.begin("settings", false);
      preferences.putUChar("batType", (uint8_t)selectedBattery);
      preferences.end();
      DEBUG_PRINTF("Battery type changed to %s, Preferences updated\n",
                   batteryTable[selectedBattery].name);
    }
  } else {
    // Timeout - Load from Preferences
    preferences.begin("settings", true);
    uint8_t savedType = preferences.getUChar("batType", 0);
    preferences.end();

    selectedBattery = (BatteryType)savedType;
    if (selectedBattery >= BATTERY_TYPE_COUNT) selectedBattery = LEAD_ACID_12V_7AH;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Timeout, using");
    lcd.setCursor(0, 1); lcd.print("previous battery!");
    ensureBeep(150, 100); // Guaranteed beep for timeout
    delay(1000);
  }

  currentBattery = batteryTable[selectedBattery];
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Battery Selected:");
  lcd.setCursor(0, 1); lcd.print(currentBattery.name);
  // Debug log
  DEBUG_PRINTLN("Battery Selected:");
  DEBUG_PRINTLN(currentBattery.name);
  ensureBeep(200, 100); // Guaranteed confirmation beep
  delay(1000);
}

//************************************************************************************
// ACS712 Zero-current Offset Calibration with Preferences Storage (10-second window)
// Performs statistical sampling with median calculation for accurate calibration
//************************************************************************************
void performCalibration() {
  DEBUG_PRINTLN("Starting calibration process...");
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Start Calibrating...");
  // GUARANTEED BEEP WHEN STARTING CALIBRATION
  ensureBeep(300, 150); // Long confirmation beep

  // Turn off PWM for calibration to ensure accurate readings
  writePWMClamped(0);

  delay(500); // Brief pause before starting

  lcd.setCursor(0, 1); lcd.print("Collect Sampling...");
  lcd.setCursor(0, 2); lcd.print("Progress: ");
  // Optimized sampling for speed and accuracy
  const int SAMPLES = 500;
  // Move large array to static storage to reduce stack pressure
  static float readings[SAMPLES];
  int validCount = 0;
  // Collect samples for statistical accuracy with audible feedback
  for (int i = 0; i < SAMPLES; i++) {
    // Use defined channel for current sensor
    int16_t raw = ads.readADC_SingleEnded(ADS_PIN_CURRENT_SENSOR);
    float v = ads.computeVolts(raw);

    // Validate reading range more strictly (2.0V to 3.0V for 5V sensor @ VCC/2)
    if (v > 2.0f && v < 3.0f) {
      readings[validCount++] = v;
    }

    // Update progress display and beep synchronously every 25 samples
    if (i % 25 == 0) {
      lcd.setCursor(10 + (i / 25), 2);
      lcd.print(".");

      // Force beep to bypass rate limits inside this tight loop
      buzzOn(true);
      delay(20);
      buzzOff(true);
    }

    // Feed watchdog every 10 samples for safety
    if (i % 10 == 0) feedWatchdog();
    delay(2); // Small delay to prevent issues
  }

  // Check if we have enough valid samples
  if (validCount < SAMPLES * 0.7) {
    DEBUG_PRINTLN("ERROR: Too many invalid samples!");
    lcd.setCursor(0, 3); lcd.print("ERR Invalid Samples!");

    // Three beeps for error indication - guaranteed
    for (int i = 0; i < 3; i++) {
      ensureBeep(150, 150); // Guaranteed error beeps
    }

    delay(1500);
    return;
  }

  // Simple bubble sort for median calculation (sort only for median)
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = i + 1; j < validCount; j++) {
      if (readings[i] > readings[j]) {
        float t = readings[i];
        readings[i] = readings[j];
        readings[j] = t;
      }
    }
  }

  // Calculate median value from sorted samples
  float median = (validCount > 0) ?
                 readings[validCount / 2] : 2.5;

  DEBUG_PRINTF("Calibration: %.3f V\n", median);

  // Display calibration result
  lcd.setCursor(0, 3);
  lcd.print("Result: ");
  lcd.print(median, 3);
  lcd.print("V");

  delay(1500);

  // Save calibration to Preferences with validation
  saveCalibrationOffset(median);
  // Update global offset for immediate use in current measurements
  ACS_OFFSET_VOLTAGE = median;

  lcd.setCursor(0, 3); lcd.print("SAVED:  ");
  lcd.print(median, 3);
  lcd.print("V ");

  DEBUG_PRINTF("Calibration saved: %.3f V\n", median);

  // Three beeps for successful calibration confirmation - guaranteed
  for (int i = 0; i < 3; i++) {
    ensureBeep(200, 200); // Guaranteed success beeps
  }

  delay(1000);

  // Clear LCD and restore normal display with icons
  lcd.clear();
  SETUP_POWER_BATT_ICON();
}

//************************************************************************************
// Automatic Calibration Countdown (10-second window)
// Provides user with opportunity to calibrate during startup
//************************************************************************************
void autoCalibrationCountdown() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Installation Done");
  lcd.setCursor(0, 1); lcd.print("Press to Start Cal");
  buttonCalibrate.update();
  unsigned long countdownStart = millis();
  const unsigned long COUNTDOWN_DURATION = 10000; // 10 seconds
  int lastCountdown = -1;
  bool calibrationTriggered = false;
  while (millis() - countdownStart < COUNTDOWN_DURATION) {
    buttonCalibrate.update();
    // Check for button press to trigger calibration
    if (buttonCalibrate.fell()) {
      ensureBeep(150, 50); // Guaranteed beep for button press
      performCalibration();
      calibrationTriggered = true;
      break;
    }

    // Update countdown display every second
    int remaining = (COUNTDOWN_DURATION - (millis() - countdownStart)) / 1000;
    if (remaining != lastCountdown) {
      lcd.setCursor(0, 2); lcd.print("Time Out in ");
      lcd.print(remaining);
      lcd.print("s ");
      lastCountdown = remaining;

      // Beep for countdown feedback
      if (remaining <= 5) {
        // Use non-blocking force beep for smoother countdown
        startBeepPattern(1, 50, 50, BEEP_PRIORITY_NORMAL, true);
      }
    }

    updateBeepPattern();
    delay(50);
  }

  // If no button press within 10 seconds, continue without calibration
  if (!calibrationTriggered) {
    lcd.setCursor(0, 3);
    lcd.print("Skipping calib...");

    // Three beeps for skip confirmation - guaranteed
    for (int i = 0; i < 3; i++) {
      ensureBeep(100, 100); // Guaranteed skip beeps
    }

    // Show current calibration value to user
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Current Calibration");
    lcd.setCursor(0, 1); lcd.print("Offset: ");
    lcd.print(ACS_OFFSET_VOLTAGE, 3);
    lcd.print("V");
    lcd.setCursor(0, 2); lcd.print("Starting System...");
    // Print current calibration value to debug
    DEBUG_PRINTF("Skipping calibration. Using current offset: %.3f V\n", ACS_OFFSET_VOLTAGE);

    delay(4000);
    // Restore normal display with icons
    lcd.clear();
    SETUP_POWER_BATT_ICON();
  }
}

//************************************************************************************
// Save Calibration Offset (Migrated to Preferences)
// Implements wear leveling by using different keys for calibration data
//************************************************************************************
void saveCalibrationOffset(float offset) {
  preferences.begin("calibration", false);
  // Implement simple wear leveling by rotating between three keys
  static uint8_t calibKeyIndex = 0;
  const char* CALIB_KEYS[] = {"cal1", "cal2", "cal3"};

  // Save to current key
  preferences.putFloat(CALIB_KEYS[calibKeyIndex], offset);
  // Update key index for next save (rotating between 0,1,2)
  calibKeyIndex = (calibKeyIndex + 1) % 3;
  // Also save to main key for backward compatibility
  preferences.putFloat("offset", offset);

  preferences.end();
  // Update runtime variable immediately
  ACS_OFFSET_VOLTAGE = offset;

  DEBUG_PRINTF("Calibration offset %.3f V saved to Preferences (key: %s)\n",
               offset, CALIB_KEYS[(calibKeyIndex + 2) % 3]); // Show which key was used
}

//************************************************************************************
// Load Calibration Offset (Migrated to Preferences)
// Loads from wear-leveled keys with fallback to main key
//************************************************************************************
bool loadCalibrationOffset() {
  preferences.begin("calibration", true); // Read-only

  // Try wear-leveled keys first, then fall back to main key
  float offset = -99.0f;
  const char* CALIB_KEYS[] = {"cal1", "cal2", "cal3"};

  for (int i = 0; i < 3; i++) {
    float testOffset = preferences.getFloat(CALIB_KEYS[i], -99.0f);
    if (testOffset > 0.5f && testOffset < 4.5f) {
      offset = testOffset;
      DEBUG_PRINTF("Loaded calibration from key %s: %.3f V\n", CALIB_KEYS[i], offset);
      break;
    }
  }

  // If wear-leveled keys failed, try main key
  if (offset < 0.5f || offset > 4.5f) {
    offset = preferences.getFloat("offset", -99.0f);
  }

  preferences.end();

  // Validate the offset is within reasonable range (0.5V to 4.5V)
  if (offset > 0.5f && offset < 4.5f) {
    ACS_OFFSET_VOLTAGE = offset;
    DEBUG_PRINTF("Loaded calibration offset: %.3f V\n", ACS_OFFSET_VOLTAGE);
    return true;
  } else {
    DEBUG_PRINTF("Invalid/No calibration offset in Preferences: %.3f V\n", offset);
    return false;
  }
}

//************************************************************************************
// LCD: Load Custom Icons and Draw Static Anchors (Top Row Labels)
// Creates battery level icons, power icon, and PWM icon for LCD display
//************************************************************************************
void SETUP_POWER_BATT_ICON() {
  // Load custom battery level icons
  for (int batchar = 0; batchar < 6; ++batchar) lcd.createChar(batchar, BATTERY_ICON[batchar]);
  // Load power and PWM icons
  lcd.createChar(POWER_ICON, POWER_ICON_PATTERN);
  lcd.createChar(PWM_ICON, PWM_ICON_PATTERN);

  // Draw static labels on top row
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("POW");
  lcd.setCursor(4, 0); lcd.write(POWER_ICON);
  lcd.setCursor(7, 0); lcd.print("BAT");
  lcd.setCursor(19, 0); lcd.print("C");
  lcd.setCursor(19, 1); lcd.write(PWM_ICON);
}

//************************************************************************************
// Sensor Installation Screen (Startup)
// Displays detection status of DS18B20 and ADS1115 sensors
//************************************************************************************
void ShowSensorInstallationScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sensor Installation");

  lcd.setCursor(0, 1);
  lcd.print("T-Sens DS18B20 ");
  lcd.print(ds18b20Found ? "OK" : "FAIL");

  lcd.setCursor(0, 2);
  lcd.print("C-Sens ADS1115 ");
  lcd.print(ads_connected ? "OK" : "FAIL");
  bool allOK = ds18b20Found && ads_connected;
  if (allOK) {
    unsigned long t0 = millis();
    while (millis() - t0 < 3000) {
      updateBeepPattern();
      delay(10);
    }
  }
}

//************************************************************************************
// Load Mode Persistence: AUTO=0, MANUAL=1
// Uses Preferences Library for reliable flash storage
//************************************************************************************
void persistLoadMode() {
  // Only write if value has changed
  preferences.begin("settings", false);
  uint8_t modeByte = (loadControlMode == MANUAL) ? 1 : 0;
  uint8_t current = preferences.getUChar("loadMode", 255); // Default invalid

  if (current != modeByte) {
    preferences.putUChar("loadMode", modeByte);
    DEBUG_PRINTF("Load mode changed to %s, Preferences updated\n",
                 (loadControlMode == AUTO) ? "AUTO" : "MANUAL");
  }
  preferences.end();
}

//************************************************************************************
// Restore Load Mode from Preferences
// Loads saved load control mode from flash storage
//************************************************************************************
void restoreLoadMode() {
  preferences.begin("settings", true);
  uint8_t modeByte = preferences.getUChar("loadMode", 0); // Default 0 (AUTO)
  preferences.end();

  loadControlMode = (modeByte == 1) ? MANUAL : AUTO;
  loadControlModeStr = (loadControlMode == AUTO) ? "A" : "M";
}

//************************************************************************************
// Load Protection Function - Call in main loop
// Monitors current and triggers protection if threshold exceeded
//************************************************************************************
void checkLoadProtection() {
  // Skip if protection is disabled
  if (!loadProtectionEnabled) {
    overcurrentCount = 0; // Reset counter
    return;
  }

  // Reset retry counter after timeout (5 minutes)
  if (protectionRetryCount > 0 &&
      millis() - lastRetryResetTime > RETRY_RESET_TIMEOUT) {
    protectionRetryCount = 0;
    lastRetryResetTime = millis();
    DEBUG_PRINTLN("Load protection retry counter reset after timeout");
  }

  // Check for overcurrent condition
  if (Current_A >= LOAD_PROTECTION_CURRENT) {
    overcurrentCount++;
    lastOvercurrentTime = millis();
    // If overcurrent detected LOAD_OVERCURRENT_COUNT times consecutively
    if (overcurrentCount >= LOAD_OVERCURRENT_COUNT) {
      // INSTANT CUTOFF - Immediate load shutdown

      // Only increment retry count once per trip event
      if (!loadWasTripped) {
        protectionRetryCount++;
        lastRetryResetTime = millis(); // Reset timeout on new fault
      }

      digitalWrite(load_enable, LOW);
      load_is_on = false;
      load_status = "off";
      loadProtectionActive = true;
      loadWasTripped = true;
      lastLoadChangeTime = millis();

      // Reset detection counter immediately
      overcurrentCount = 0;
      // Play distinctive alarm pattern for overload - guaranteed
      startBeepPattern(5, 200, 100, BEEP_PRIORITY_CRITICAL, true); // 5 long beeps - immediate

      DEBUG_PRINTF("LOAD PROTECTION TRIPPED: %.2fA >= %.2fA\n",
                   Current_A, LOAD_PROTECTION_CURRENT);
    }
  } else {
    // Reset counter when current is below threshold
    overcurrentCount = 0;
  }

  // Auto-reset protection after 5 seconds if load was tripped
  if (loadWasTripped && (millis() - lastOvercurrentTime > LOAD_PROTECTION_RESET_TIME)) {
    resetLoadProtectionAuto();
  }
}

//************************************************************************************
// Auto Reset Load Protection (called after timeout)
// Includes logic to Latch OFF after MAX_PROTECTION_RETRIES for safety
//************************************************************************************
void resetLoadProtectionAuto() {
  if (loadWasTripped) {
    // Check if we have delayed maximum retries (Hiccup Protection)
    if (protectionRetryCount < MAX_PROTECTION_RETRIES) {
      // Safe to reset
      loadWasTripped = false;
      loadProtectionActive = false;
      overcurrentCount = 0;

      // Clear any existing messages on LCD line 3
      lcd.setCursor(0, 3);
      lcd.print("                ");
      DEBUG_PRINTLN("Load protection auto-reset after 5 seconds");
    } else {
      // Critical Fault - Latch Off
      lcd.setCursor(0, 3);
      lcd.print("SHORT CKT FAULT!"); // Notify user
      DEBUG_PRINTLN("Critical Fault: Max load retries exceeded. Latching OFF.");
    }
  }
}

//************************************************************************************
// Handle Load Control Buttons: MODE toggle, LOAD queue, AUTO manual override
// Uses consistent button debounce for all buttons (250ms)
//************************************************************************************
void handleLoadButtons() {
  buttonMode.update();
  buttonLoad.update();

  // Consistent button debounce for all buttons
  static unsigned long lastButtonPress = 0;
  const unsigned long BUTTON_DEBOUNCE = 250; // Consistent 250ms debounce for all buttons

  // Toggle between AUTO and MANUAL load control modes
  if (buttonMode.fell()) {
    if (millis() - lastButtonPress < BUTTON_DEBOUNCE) return;
    lastButtonPress = millis();

    loadControlMode = (loadControlMode == AUTO) ?
                      MANUAL : AUTO;
    loadControlModeStr = (loadControlMode == AUTO) ?
                         "A" : "M";
    // Non-blocking beep to fix button lag
    startBeepPattern(1, 100, 50, BEEP_PRIORITY_NORMAL, true);

    persistLoadMode();
  }

  // Handle load on/off button press
  if (buttonLoad.fell()) {
    // Consistent debounce with MODE button
    if (millis() - lastButtonPress < BUTTON_DEBOUNCE) {
      return; // Ignore bounce
    }
    lastButtonPress = millis();
    // Manual Reset of Latched Fault
    if (protectionRetryCount >= MAX_PROTECTION_RETRIES) {
      // Reset Faults
      protectionRetryCount = 0;
      loadWasTripped = false;
      loadProtectionActive = false;
      overcurrentCount = 0;

      // Feedback
      lcd.setCursor(0, 3);
      lcd.print("     Reset      ");
      startBeepPattern(3, 80, 40, BEEP_PRIORITY_HIGH, true);
      DEBUG_PRINTLN("User manually reset load protection.");
      return;
    }

    // Normal Load Toggle Logic (Only works if NOT tripped)
    if (!loadWasTripped && !pendingLoadChange) {
      bool wantOn = !load_is_on;
      nextToggleIsManual = true;
      scheduleLoadToggle(wantOn);
    }
  }

  // Handle temporary load expiration in AUTO mode
  if (tempLoadOn && loadControlMode == AUTO) {
    unsigned long elapsed = millis() - tempLoadStart;
    if (!autoBeepPending && elapsed >= (TEMP_LOAD_DURATION - TEMP_OFF_WARN_LEAD_MS)) {
      autoBeepPending = true;
      startBeepPattern(AUTO_EXPIRY_BEEP_COUNT, AUTO_EXPIRY_BEEP_ON_MS, AUTO_EXPIRY_BEEP_OFF_MS, BEEP_PRIORITY_NORMAL);
    }
    if (elapsed >= TEMP_LOAD_DURATION) {
      tempLoadOn = false;
      autoBeepPending = false;
      digitalWrite(load_enable, LOW);
      load_is_on = false;
      load_status = "Off";
      lastLoadChangeTime = millis();
    }
  }

  // Execute pending load changes after beep pattern completion
  if (pendingLoadChange && !isBeepActive()) {
    finalizeLoadToggle();
  }
}

//************************************************************************************
// Manual Calibration Button Handler (Startup Only - 10 seconds)
// Allows calibration during initial startup phase only
//************************************************************************************
void handleCalibrationButton() {
  // Calibration button only works during startup phase (10 seconds)
  static bool startupPhase = true;
  static unsigned long startupTimeout = 10000; // 10 seconds startup window
  static unsigned long startTime = millis();
  // Disable calibration after startup timeout period
  if (millis() - startTime > startupTimeout) {
    startupPhase = false;
    return;
  }

  // Only allow calibration during startup phase
  if (!startupPhase) {
    return;
  }

  buttonCalibrate.update();

  if (buttonCalibrate.fell()) {
    DEBUG_PRINTLN("Manual calibration requested during startup");
    ensureBeep(150, 50); // Guaranteed beep for button press
    performCalibration();
    // Extend startup time after calibration to allow for completion
    startTime = millis();
  }
}

//************************************************************************************
// Watchdog Management Functions
// Ensures system stability by preventing hangs and crashes
//************************************************************************************
void initWatchdog() {
  // Initialize the Task Watchdog Timer
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WATCHDOG_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // Trigger on all cores
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_init(&twdt_config);
#else
  esp_err_t err = esp_task_wdt_init(WATCHDOG_TIMEOUT, true); // Enable panic so ESP32 restarts
#endif

  if (err != ESP_OK) {
    DEBUG_PRINTF("Failed to initialize watchdog: %d\n", err);
  } else {
    DEBUG_PRINTF("Watchdog initialized with %d second timeout\n", WATCHDOG_TIMEOUT);
  }

  // Add current task to watchdog
  err = esp_task_wdt_add(NULL);
  if (err != ESP_OK) {
    DEBUG_PRINTF("Failed to add task to watchdog: %d\n", err);
  } else {
    DEBUG_PRINTLN("Current task added to watchdog");
  }

  lastWatchdogFeed = millis();
}

void feedWatchdog() {
  // Feed the watchdog periodically to prevent reset
  if (millis() - lastWatchdogFeed >= WATCHDOG_FEED_INTERVAL) {
    esp_task_wdt_reset();
    lastWatchdogFeed = millis();
  }
}

void disableWatchdog() {
  // Disable watchdog before operations that might take longer than timeout
  esp_task_wdt_delete(NULL);
  DEBUG_PRINTLN("Watchdog disabled");
}

void reenableWatchdog() {
  // Re-enable watchdog after long operations
  esp_task_wdt_add(NULL);
  lastWatchdogFeed = millis();
  DEBUG_PRINTLN("Watchdog re-enabled");
}

//************************************************************************************
// I²C Bus Recovery Function
// Enhanced I²C bus recovery with proper clock pulse sequence and STOP condition
//************************************************************************************
void recoverI2CBus() {
  DEBUG_PRINTLN("I²C bus recovery initiated...");
  systemHealth.i2cRecoveries++; // Track recovery attempts

  // Release I²C pins
  Wire.end();
  delay(10);
  // Configure pins as GPIO
  pinMode(21, OUTPUT);  // SDA
  pinMode(22, OUTPUT);  // SCL

  // I²C bus recovery procedure (9 clock pulses with SDA high)
  for (int i = 0; i < 9; i++) {
    digitalWrite(22, LOW); // SCL low
    delayMicroseconds(5);
    digitalWrite(21, HIGH);   // Ensure SDA is released (HIGH)
    delayMicroseconds(5);
    digitalWrite(22, HIGH);   // SCL high
    delayMicroseconds(5);
    digitalWrite(22, LOW); // SCL low for next cycle
    delayMicroseconds(5);
  }

  // Create STOP condition (SCL high, then SDA high)
  digitalWrite(22, HIGH);
  delayMicroseconds(5);
  digitalWrite(21, HIGH);
  delayMicroseconds(5);
  // Return to I²C mode
  pinMode(21, INPUT_PULLUP);
  pinMode(22, INPUT_PULLUP);
  delay(50);

  // Reinitialize I²C
  Wire.begin(21, 22);
  Wire.setClock(200000);
  delay(100);
  DEBUG_PRINTLN("I²C bus recovery completed");
}

//************************************************************************************
// Estimate Battery Percentage (0-100%)
// Battery-Type-Specific Linear Mapping with smoothing for stable readings
//************************************************************************************
int getBatteryPercentage(float voltage) {
  float min_voltage, max_voltage;
  // Battery-type-specific voltage ranges for linear mapping
  switch (selectedBattery) {
    case LEAD_ACID_12V_7AH:
      // Lead Acid: 10.5V (discharged) to 12.7V (fully charged)
      min_voltage = 10.5f;
      max_voltage = 12.7f;
      break;

    case LIFEPO4_12V:
      // LiFePO4: 10.0V (discharged) to 13.8V (fully charged)
      min_voltage = 10.0f;
      max_voltage = 13.8f;
      break;
    case LIION_3S:
      // Li-ion: 9.0V (discharged) to 12.6V (fully charged)
      min_voltage = 9.0f;
      max_voltage = 12.6f;
      break;

    default:
      // Fallback to safe defaults
      min_voltage = MIN_BATTERY_VOLTAGE;
      max_voltage = MAX_BATTERY_VOLTAGE;
      break;
  }

  // Constrain voltage to prevent out-of-range calculations
  voltage = constrain(voltage, min_voltage, max_voltage);
  // Proper static variable initialization
  static float smoothedVoltage = 0.0f;
  static bool firstRun = true;
  const float SMOOTHING_FACTOR = 0.1;

  if (firstRun) {
    smoothedVoltage = voltage; // Use actual voltage on first run
    firstRun = false;
  } else {
    smoothedVoltage += (voltage - smoothedVoltage) * SMOOTHING_FACTOR;
  }

  // Division by zero safety check
  if (fabs(max_voltage - min_voltage) < 0.01) return 0;
  // Linear mapping: percentage = (voltage - min) / (max - min) * 100
  float percentage = ((smoothedVoltage - min_voltage) / (max_voltage - min_voltage)) * 100.0;
  // Return constrained and rounded percentage
  return round(constrain(percentage, 0.0, 100.0));
}

//************************************************************************************
// Non-blocking Welcome Screen with Delays
// Displays welcome message with version information
//************************************************************************************
void showWelcomeScreenNonBlocking() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ESP32 CC/CV Charger");
  lcd.setCursor(2, 1); lcd.print("Controller v12.0");
  lcd.setCursor(6, 2); lcd.print("Dev By");
  lcd.setCursor(4, 3); lcd.print("Subash Saraf");

  playPowerOnBeep();
  waitForBeepPattern(200); // Non-blocking 2 second wait
  NonBlockingDelay welcomeDelay;
  unsigned long startTime = millis();
  while (millis() - startTime < 2000) {
    updateBeepPattern();
    feedWatchdog(); // Feed watchdog during initialization
    delay(10);
  }
}

//************************************************************************************
// System Health Monitoring and Memory Management
// Tracks system performance and detects potential issues
//************************************************************************************
void monitorSystemHealth() {
  static unsigned long lastHealthCheck = 0;
  unsigned long now = millis();

  // Check every 30 seconds
  if (now - lastHealthCheck > 30000) {
    lastHealthCheck = now;
    // Update system health metrics
    systemHealth.totalUptime = now / 1000; // Convert to seconds
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t minFree = ESP.getMinFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    // Track minimum free heap
    if (freeHeap < systemHealth.minFreeHeap) {
      systemHealth.minFreeHeap = freeHeap;
    }

    // Log memory status periodically
    DEBUG_PRINTF("[HEALTH] Uptime: %lus, Free Heap: %u, Min Free: %u, Max Alloc: %u\n",
                 systemHealth.totalUptime, freeHeap, minFree, maxAlloc);
    // Warning if heap is getting low
    if (freeHeap < 20000) {
      DEBUG_PRINTLN("[WARNING] Low heap memory detected!");
    }
  }
}

//************************************************************************************
// Graceful Shutdown Procedure
// Safely powers down system components before restart or shutdown
//************************************************************************************
void gracefulShutdown() {
  DEBUG_PRINTLN("Starting graceful shutdown...");
  // Turn off PWM output
  writePWMClamped(0);

  // Disable load
  digitalWrite(load_enable, LOW);
  // Turn off LEDs
  digitalWrite(LED_CRITICAL, LOW);
  digitalWrite(LED_MEDIUM, LOW);
  digitalWrite(LED_FULL, LOW);
  COMPAT_LEDC_WRITE(BATTSTATUS_LED, ledChannel, 0);

  // Explicitly ensure IR2104 is shutdown (High-Z)
  digitalWrite(DRIVER_SD_PIN, LOW);
  // Display shutdown message
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Shutting down...");
  // Play shutdown beep
  ensureBeep(300, 150);
  delay(1000);
  // Turn off LCD backlight
  lcd.noBacklight();

  DEBUG_PRINTLN("Graceful shutdown completed");
}

//************************************************************************************
// Arduino Setup: Initialize Peripherals, WiFi, Sensors, PWM, UI
// Main initialization function called once at startup
//************************************************************************************
void setup() {
#if ENABLE_SERIAL_DEBUG
  Serial.begin(115200);
  // Version Check Print
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  DEBUG_PRINTLN("Core Version: v3.0+ (LEDC API Updated)");
#else
  DEBUG_PRINTLN("Core Version: v2.x (Legacy LEDC API)");
#endif
#endif

  // FIX: SIGNAL NOISE (Ground Bounce)
  WiFi.setSleep(false);
  // Initialize watchdog first
  initWatchdog();
  // FIX: I2C SPEED INCOMPATIBILITY (Logic Level Converter)
  Wire.begin(21, 22);
  Wire.setClock(200000);// 200kHz - 2x faster updates

  lcd.init();
  lcd.backlight();
  // Show welcome screen at startup
  setupBuzzer(); // Initialize Buzzer module

  showWelcomeScreenNonBlocking();

  pinMode(BUTTON_MODE, INPUT_PULLUP);
  buttonMode.attach(BUTTON_MODE);
  buttonMode.interval(50);
  pinMode(BUTTON_LOAD, INPUT_PULLUP);
  buttonLoad.attach(BUTTON_LOAD);
  buttonLoad.interval(50);

  pinMode(BUTTON_WIFI_RESET, INPUT_PULLUP);
  buttonWiFiReset.attach(BUTTON_WIFI_RESET);
  buttonWiFiReset.interval(50);
  pinMode(BUTTON_CALIBRATE, INPUT_PULLUP);
  buttonCalibrate.attach(BUTTON_CALIBRATE);
  buttonCalibrate.interval(50);

  buzzOff();

  playPowerOnBeep();
  waitForBeepPattern(600);

  // Use new wifi_sys module
  setupNetwork();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Searching for WiFi");
  DEBUG_PRINTLN("Searching for WiFi...");

  // Complete WiFi setup with services (moved to wifi_sys)
  bool wifiConnected = setupWiFiAndServices();
  // Continue with hardware initialization
  pinMode(load_enable, OUTPUT);
  digitalWrite(load_enable, LOW);
  pinMode(BATTSTATUS_LED, OUTPUT);
  pinMode(LED_CRITICAL, OUTPUT);
  digitalWrite(LED_CRITICAL, LOW);
  pinMode(LED_MEDIUM, OUTPUT);
  digitalWrite(LED_MEDIUM, LOW);
  pinMode(LED_FULL, OUTPUT);
  digitalWrite(LED_FULL, LOW);

  // === IR2104 INITIALIZATION ===
  // Initialize SD (Shutdown) pin to LOW (Drivers OFF)
  pinMode(DRIVER_SD_PIN, OUTPUT);
  digitalWrite(DRIVER_SD_PIN, LOW);

  restoreLoadMode();

  // PWM SAFE BOOT: Apply EXACT order - attach, write 0, delay
  digitalWrite(PWM_out, LOW);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PWM_out, PWM_FREQ, PWM_RES);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_out, PWM_CHANNEL);
#endif
  ledcWrite(PWM_out, 0);
  delay(1);

  // Use compatibility macro for v3.0+ support
  COMPAT_LEDC_ATTACH(BATTSTATUS_LED, ledChannel, pwmFrequency, pwmResolution);

  // Ensure PWM and Driver state are safe (OFF)
  writePWMClamped(0);

  // Log PWM configuration for debugging
  DEBUG_PRINTF("PWM Configuration: User Limit = %d%% (%d/1023)\n",
               PWM_FULL_SCALE_LIMIT, PWM_MAX_DUTY);
  DEBUG_PRINTF("IR2104 Bootstrap Safety: Low-side ON time = %d%% of cycle\n",
               100 - PWM_FULL_SCALE_LIMIT);

  ds18b20.begin();
  ds18b20Found = ds18b20.getAddress(ds18b20Addr, 0);
  if (ds18b20Found) {
    ds18b20.setResolution(ds18b20Addr, 12);
    ds18b20.setWaitForConversion(false);
    ds18b20.requestTemperatures();
    DEBUG_PRINTLN("DS18B20: Found");
  } else {
    DEBUG_PRINTLN("DS18B20: NOT found");
  }

#if !SIMULATION_MODE
  ads.setGain(GAIN_TWOTHIRDS);
  ads_connected = false;
  for (int attempts = 0; attempts < 5 && !ads_connected; attempts++) {
    ads_connected = ads.begin();
    if (!ads_connected) {
      DEBUG_PRINTF("ADS1115 not detected! Attempt %d\n", attempts + 1);
      lcd.setCursor(0, 0);
      lcd.print("ADS Retry "); lcd.print(attempts + 1);

      // Non-blocking delay
      NonBlockingDelay adsRetryDelay;
      unsigned long retryStart = millis();
      while (millis() - retryStart < 1000) {
        if (adsRetryDelay.isComplete(100)) {
          updateBeepPattern();
          feedWatchdog(); // Feed watchdog during sensor initialization
        }
        delay(10);
      }
    }
  }
  if (!ads_connected) {
    ShowSensorInstallationScreen();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ADS1115 not found!");
    DEBUG_PRINTLN("Failed to initialize ADS1115 after 5 attempts.");

    // Non-blocking infinite loop
    NonBlockingDelay errorDelay;
    while (1) {
      if (errorDelay.isComplete(1000)) {
        updateBeepPattern();
        feedWatchdog(); // Feed watchdog in error state
      }
      delay(10);
    }
  }
#else
  ads_connected = true;
#endif

  ShowSensorInstallationScreen();
  if       (ACS712_VARIANT == 5)  ACS_SENSITIVITY = 0.185;
  else if (ACS712_VARIANT == 20) ACS_SENSITIVITY = 0.100;
  else if (ACS712_VARIANT == 30) ACS_SENSITIVITY = 0.066;
  else {
    DEBUG_PRINTLN("Invalid ACS712 variant selected!");
    lcd.setCursor(0, 0); lcd.print("Invalid ACS712 Config");
    // Non-blocking infinite loop
    NonBlockingDelay configErrorDelay;
    while (1) {
      if (configErrorDelay.isComplete(1000)) {
        updateBeepPattern();
        feedWatchdog(); // Feed watchdog in error state
      }
      delay(10);
    }
  }

  // Load battery type from Preferences
  preferences.begin("settings", true);
  uint8_t savedType = preferences.getUChar("batType", 0); // Default to 0
  preferences.end();

  selectedBattery = (BatteryType)savedType;
  if (selectedBattery >= BATTERY_TYPE_COUNT) selectedBattery = LEAD_ACID_12V_7AH;
  currentBattery = batteryTable[selectedBattery];

  batterySelectionMenu();
  writePWMClamped(0);
  // Non-blocking delay
  NonBlockingDelay setupDelay;
  unsigned long setupStart = millis();
  while (millis() - setupStart < 2000) {
    if (setupDelay.isComplete(100)) {
      updateBeepPattern();
      feedWatchdog(); // Feed watchdog during setup
    }
    delay(10);
  }

  // Load calibration offset from Preferences
  if (!loadCalibrationOffset()) {
    DEBUG_PRINTLN("Using default offset: 2.5V");
    ACS_OFFSET_VOLTAGE = 2.5;
  }

#if !SIMULATION_MODE
  autoCalibrationCountdown(); // 10-second window for calibration
#endif

  setupFanPWM();
  SETUP_POWER_BATT_ICON();
  // Non-blocking delay
  NonBlockingDelay finalDelay;
  unsigned long finalStart = millis();
  while (millis() - finalStart < 1000) {
    if (finalDelay.isComplete(100)) {
      updateBeepPattern();
      feedWatchdog(); // Feed watchdog during final setup
    }
    delay(10);
  }

  rebootStartTime = millis();
  lastWatchdogFeed = millis(); // Reset watchdog feed timer

  // Initial system health check
  DEBUG_PRINTF("System initialized. Free heap: %d bytes\n", ESP.getFreeHeap());
  DEBUG_PRINTF("Min free heap: %d bytes\n", ESP.getMinFreeHeap());
}

//************************************************************************************
// Arduino Loop: Main Non-blocking Cycle
// Main execution loop with state machine and periodic tasks
//************************************************************************************
void loop() {
  // Feed the watchdog regularly to prevent reset
  feedWatchdog();
  // Handle all operations with non-blocking delays
  handleWiFiResetButton(); // now in wifi_sys.cpp
  updateBeepPattern();
  handleLoadButtons();
  handleCalibrationButton();
  handleWebServer();
  wifiMaintain(); // now in wifi_sys.cpp

  // Non-blocking IP print every 30 seconds
  if (ipPrintDelay.isComplete(30000)) {
    bool connected = (WiFi.status() == WL_CONNECTED);
    DEBUG_PRINT("WiFi Status: ");
    DEBUG_PRINT(connected ? "Connected" : "Disconnected");
    if (connected) {
      DEBUG_PRINT(" | IP: ");
      DEBUG_PRINT(WiFi.localIP());
      DEBUG_PRINT(" | RSSI: ");
      DEBUG_PRINT(WiFi.RSSI());
      DEBUG_PRINTLN(" dBm");
    } else {
      DEBUG_PRINTLN(" | IP: 0.0.0.0 | RSSI: N/A");
    }
  }

  if (!ads_connected) {
    if (sensorDelay.isComplete(5000)) { // Increased to 5 seconds for recovery attempts
      // Attempt I²C bus recovery before retrying
      recoverI2CBus();
      ads_connected = ads.begin();
      if (!ads_connected) {
        lcd.setCursor(0, 1);
        lcd.print("Retrying ADS...    ");
        DEBUG_PRINTLN("Retrying ADS1115 with bus recovery...");
      } else {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("ADS1115 Connected!");
        // Non-blocking delay
        NonBlockingDelay connectedDelay;
        unsigned long connectStart = millis();
        while (millis() - connectStart < 1000) {
          if (connectedDelay.isComplete(100)) {
            updateBeepPattern();
            feedWatchdog(); // Feed watchdog during connection
          }
          delay(10);
        }
      }
    }
    return;
  }

  // REMOVED: writePWMClamped(pwm_value) - PWM is written ONLY in control logic
  Auto_Reboot_Logic();
  // Run sensors and logic every 100ms to reduce blocking latency
  if (sensorDelay.isComplete(100)) {
    readSensors();
    // CHECK LOAD PROTECTION AFTER READING SENSORS
    checkLoadProtection();
    updateChargeMode();

    // CRITICAL FIX: Call adjustPWM() for active charging modes
    // This ensures PWM is actively controlled regardless of state transitions
    if (mode != BACKUP && mode != IDLE) {
      // Determine target voltage and current based on mode
      float targetVoltage, maxCurrent;
      switch (mode) {
        case BULK:
          targetVoltage = min(bulk_voltage_max, BULK_SAFETY_LIMIT);
          maxCurrent = currentBattery.bulk_current;
          break;
        case ABSORPTION:
          targetVoltage = absorption_voltage_max;
          maxCurrent = currentBattery.absorption_current;
          break;
        case FLOAT:
          targetVoltage = min(float_voltage_max, FLOAT_SAFETY_LIMIT);
          maxCurrent = currentBattery.float_current;
          break;
        default:
          targetVoltage = min(bulk_voltage_max, BULK_SAFETY_LIMIT);
          maxCurrent = currentBattery.bulk_current;
          break;
      }
      adjustPWM(targetVoltage, maxCurrent);
    }

    // Manage Driver SD (Shutdown) pin state
    manageDriverSD();
    updateLoadStatus();
  }

  // Update LCD every 500ms
  if (lcdDelay.isComplete(500)) {
    Update_Main_LCD();
  }

  BattStatusLED_Pulse();
  updateBatteryStatusLED();
  SPINNER_Status();
  Battery_Power_Display();
  // Log data every 1 second
  if (logDelay.isComplete(1000)) {
    logDataToSerial();
  }

  // Monitor system health
  monitorSystemHealth();

  updateFanPWM(temperatureC);
  // Small yield delay to prevent issues in tight loops
  delay(1);
}

//************************************************************************************
// Auto Reboot with 30s Heads-up on LCD
// Automatic system reboot every 24 hours for stability
//************************************************************************************
void Auto_Reboot_Logic() {
  static bool counting = false;
  static unsigned long countdownStart = 0;
  static NonBlockingDelay rebootDelay;

  unsigned long uptime = millis() - rebootStartTime;
  if (!counting && uptime >= REBOOT_INTERVAL - 30000UL) {
    counting = true;
    rebootNotified = true;
    countdownStart = millis();
    DEBUG_PRINTLN("Rebooting system...in 30 Sec");
  }

  if (counting) {
    if (rebootDelay.isComplete(1000)) { // Update every second
      unsigned long elapsed = millis() - countdownStart;
      int remaining = 30 - (elapsed / 1000);
      if (remaining < 0) remaining = 0;

      lcd.setCursor(0, 3);
      lcd.print("Rebooting in " + String(remaining) + "s        ");

      if (elapsed >= 30000UL) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Rebooting system...");
        DEBUG_PRINTLN("Rebooting now...");
        // Give time for LCD update before reboot
        delay(1000);
        ESP.restart();
      }
    }
  }
}

//************************************************************************************
// Read Sensors (ADS1115 + DS18B20)
// Enhanced with hysteresis for current noise filtering
// Enhanced temperature sensor validation with rate-of-change limits
//************************************************************************************
void readSensors() {
#if SIMULATION_MODE
  input_voltage = 24.00; // 24.0;// 0.0for Testing where no Input Voltage 
  bat_voltage   = 14.40;
  Current_A     = 0.02;
  temperatureC  = 32.0;
  tempValid     = true;
  return;
#endif

  // Read DC input voltage from defined channel
  int16_t raw_input = ads.readADC_SingleEnded(ADS_PIN_INPUT_VOLTAGE);
  float adc_input_V = ads.computeVolts(raw_input);
  float new_input = adc_input_V * dividerFactor;
  // Read battery voltage from defined channel
  int16_t raw_batt = ads.readADC_SingleEnded(ADS_PIN_BATTERY_VOLTAGE);
  float adc_batt_V = ads.computeVolts(raw_batt);
  float new_batt = adc_batt_V * dividerFactor;

  // Apply filtering
  input_voltage = ALPHA_V * new_input + (1 - ALPHA_V) * input_voltage;
  bat_voltage   = ALPHA_V * new_batt  + (1 - ALPHA_V) * bat_voltage;
  // Read current from ACS712 sensor on defined channel
  int16_t raw_I = ads.readADC_SingleEnded(ADS_PIN_CURRENT_SENSOR);
  float v2 = ads.computeVolts(raw_I);
  float current = (v2 - ACS_OFFSET_VOLTAGE) / ACS_SENSITIVITY;
  current += MANUAL_OFFSET_CORRECTION;
  if (current < 0) current = -current;
  // Add hysteresis for noise filtering
  static float lastFilteredCurrent = 0.0f;
  if (fabs(current) < noiseThreshold + NOISE_HYSTERESIS &&
      fabs(lastFilteredCurrent) < noiseThreshold + NOISE_HYSTERESIS) {
    current = 0.0f;
  }
  lastFilteredCurrent = current;

  Current_A = ALPHA_C * current + (1 - ALPHA_C) * Current_A;
  // Temperature sensor reading with enhanced validation
  if (ds18b20Found) {
    if (ds18b20.isConversionComplete()) {
      float tC = ds18b20.getTempC(ds18b20Addr);
      // Temperature validation with rate-of-change limits
      static float lastValidTemp = 25.0f;
      static unsigned long lastTempRead = 0;
      const float MAX_TEMP_RATE_CHANGE = 5.0f; // Max 5°C per second

      if (tC != DEVICE_DISCONNECTED_C && tC > -55.0f && tC < 125.0f) {
        unsigned long now = millis();
        float timeDelta = (now - lastTempRead) / 1000.0f; // Convert to seconds

        // Check for sudden unrealistic changes
        if (timeDelta > 0 && fabs(tC - lastValidTemp) / timeDelta <= MAX_TEMP_RATE_CHANGE) {
          tempValid = true;
          filteredTemp = ALPHA_TEMP * tC + (1.0f - ALPHA_TEMP) * filteredTemp;
          temperatureC = filteredTemp;
          lastValidTemp = tC;
          lastTempRead = now;
        } else {
          // Use filtered value if rate is too high
          temperatureC = filteredTemp;
          tempValid = false;
        }
      } else {
        // Reset filtered temperature on invalid read
        filteredTemp = lastValidTemp; // Keep last known good value
        temperatureC = filteredTemp;
        tempValid = false;
      }
      ds18b20.requestTemperatures();
    }
  } else {
    tempValid = false;
  }
}

//************************************************************************************
// Charge Mode State Machine - v12.0 FULL HYSTERESIS-BASED CONTROLLER
// Implements complete state transitions: BULK → ABSORPTION → FLOAT → BACKUP → IDLE → BULK
// Strict separation: Only handles state transitions, timers, and hysteresis decisions
// All hysteresis values sourced from battery_types.h (no magic numbers)
//************************************************************************************
void updateChargeMode() {
  static unsigned long floatHoldStart = 0UL;
  static unsigned long absorptionHoldStart = 0UL;
  static unsigned long overvoltageStart = 0UL;
  static unsigned long idleStartTime = 0UL;
  static unsigned long idleVoltageDropStart = 0UL;
  unsigned long now = millis();

  // MODE FLAPPING PREVENTION: Enforce minimum dwell time
  bool canTransition = (now - lastModeChange >= currentBattery.min_mode_time_ms);

  // === IR2104 CHANGE ===
  // When resetting PWM, ensure we respect the synchronous topology.
  // 0 = Low-Side ON (Freewheeling) via Driver.

  // Check if input voltage is sufficient for charging
  if (input_voltage <= input_voltage_min) {
    if (mode != BACKUP) {
      mode = BACKUP;
      // Show (BATT) when load is ON, (B-IDL) when load is OFF
      mode_str = (load_is_on && !loadWasTripped) ? "(BATT)" : "(B-IDL)";
      // Full Reset
      pwm_value = 0; pwm_last = 0; writePWMClamped(pwm_value);
      lastModeChange = now; floatHoldStart = 0; absorptionHoldStart = 0; idleStartTime = 0; idleVoltageDropStart = 0;
      DEBUG_PRINTLN(F("Entering BACKUP mode due to low input voltage"));
    } else {
      // Already in BACKUP mode - update display based on current load status
      mode_str = (load_is_on && !loadWasTripped) ? "(BATT)" : "(B-IDL)";
    }
    return;
  }

  // Exit BACKUP when input voltage is restored
  if (mode == BACKUP && input_voltage > input_voltage_min) {
    mode = BULK;
    mode_str = "(BULK)";
    // Full Reset for Soft Start
    pwm_value = 0; pwm_last = 0; writePWMClamped(0);
    lastModeChange = now; floatHoldStart = 0; absorptionHoldStart = 0; idleStartTime = 0; idleVoltageDropStart = 0;
    DEBUG_PRINTLN(F("Exiting BACKUP -> Restarting from BULK"));
    return;
  }

  // Overvoltage protection - FORCE BACKUP with hard PWM shutdown
  if (bat_voltage >= OVERVOLTAGE_CUTOFF) {
    if (overvoltageStart == 0) overvoltageStart = now;
    if (now - overvoltageStart < 3000) {
      // Rapid PWM reduction during overvoltage
      pwm_value = max(pwm_value - 30, 0); // Increased step for 10-bit range
      writePWMClamped(pwm_value);
      DEBUG_PRINTLN(F("Overvoltage! Reducing PWM..."));
    } else {
      // Persistent overvoltage - force BACKUP
      mode = BACKUP;
      // Show (B-IDL) during overvoltage (safety state, load should be OFF)
      mode_str = "(B-IDL)";
      pwm_value = 0;
      pwm_last = 0;
      writePWMClamped(pwm_value);
      lastModeChange = now; floatHoldStart = 0; absorptionHoldStart = 0; idleVoltageDropStart = 0;
      DEBUG_PRINTLN(F("Overvoltage persists -> Switching to BACKUP!"));
    }
    return;
  } else {
    overvoltageStart = 0;
  }

  // IDLE mode entry (battery fully charged) - FIXED: Only from FLOAT mode
  if (mode == FLOAT && canTransition) {
    // Battery must be at float voltage with low current to enter IDLE
    bool atFloatVoltage = (bat_voltage >= (float_voltage_min - VOLTAGE_HYSTERESIS)) && 
                          (bat_voltage <= (float_voltage_max + VOLTAGE_HYSTERESIS));
    bool lowCurrent = (Current_A <= IDLE_CURRENT_THRESHOLD);
    
    if (atFloatVoltage && lowCurrent) {
      if (idleStartTime == 0) idleStartTime = now;
      // Use battery-specific idle entry time
      if ((now - idleStartTime) >= currentBattery.idle_entry_time_ms) {
        mode = IDLE;
        mode_str = "(IDLE)";
        pwm_value = 0; pwm_last = 0; writePWMClamped(0);
        lastModeChange = now; floatHoldStart = 0; absorptionHoldStart = 0; idleVoltageDropStart = 0;
        DEBUG_PRINTLN(F("Entering IDLE mode (battery fully charged)"));
        return;
      }
    } else {
      idleStartTime = 0; // Reset timer if conditions no longer met
    }
  } else if (mode != IDLE) {
    idleStartTime = 0; // Reset timer if not in FLOAT mode
  }

  // IDLE mode management - requires voltage + time hysteresis to exit
  if (mode == IDLE) {
    if (bat_voltage <= (float_voltage_min - VOLTAGE_HYSTERESIS)) {
      if (idleVoltageDropStart == 0) idleVoltageDropStart = now;
      // Use battery-specific idle exit time
      if ((now - idleVoltageDropStart) >= currentBattery.idle_exit_time_ms) {
        mode = BULK;
        mode_str = "(BULK)";
        lastModeChange = now;
        idleStartTime = 0;
        idleVoltageDropStart = 0;
        floatHoldStart = 0;
        absorptionHoldStart = 0;
        DEBUG_PRINTLN(F("IDLE -> BULK: Battery voltage low for specified time"));
        return;
      }
    } else {
      idleVoltageDropStart = 0;
    }

    if (Current_A > (IDLE_CURRENT_THRESHOLD + CURRENT_HYSTERESIS)) {
      mode = BULK;
      mode_str = "(BULK)";
      lastModeChange = now;
      idleStartTime = 0;
      idleVoltageDropStart = 0;
      floatHoldStart = 0;
      absorptionHoldStart = 0;
      DEBUG_PRINTLN(F("Exiting IDLE mode (current increased)"));
      return;
    }

    pwm_value = 0; pwm_last = 0; writePWMClamped(0);
    return;
  }

  // MAIN STATE TRANSITIONS
  switch (mode) {
    case BULK:
      if (canTransition) {
        bool reachedV = (bat_voltage >= (bulk_voltage_max + VOLTAGE_HYSTERESIS));
        if (reachedV) {
          mode = ABSORPTION;
          mode_str = "(ABSO)";
          lastModeChange = now;
          floatHoldStart = 0;
          absorptionHoldStart = 0;
          DEBUG_PRINTLN(F("Transition: BULK -> ABSORPTION"));
        }
      }
      break;
    case ABSORPTION:
      if (canTransition) {
        bool atV = (bat_voltage >= absorption_voltage_max);
        bool atI = (Current_A <= (currentBattery.absorption_current + CURRENT_HYSTERESIS));
        if (atV && atI) {
          if (absorptionHoldStart == 0) {
            absorptionHoldStart = now;
            DEBUG_PRINTLN(F("Absorption hold timer started"));
          } else if (now - absorptionHoldStart >= currentBattery.absorption_time_ms) {
            // Use battery-specific absorption time
            mode = FLOAT;
            mode_str = "(FLOT)";
            lastModeChange = now;
            floatHoldStart = 0;
            absorptionHoldStart = 0;
            DEBUG_PRINTLN(F("Transition: ABSORPTION -> FLOAT"));
          }
        } else {
          absorptionHoldStart = 0;
        }
      }
      break;
    case FLOAT:
      if (canTransition) {
        // SAFE FLOAT EXIT: Requires voltage drop below min with hysteresis
        bool lowV = (bat_voltage < (float_voltage_min - VOLTAGE_HYSTERESIS));
        bool highI = (Current_A > (currentBattery.float_current + CURRENT_HYSTERESIS));
        if (lowV || highI) {
          if (floatHoldStart == 0) {
            floatHoldStart = now;
            DEBUG_PRINTLN(F("Float exit timer started (V or I out of range)"));
          } else if (now - floatHoldStart >= currentBattery.float_exit_time_ms) {
            // Use battery-specific float exit time
            mode = BULK;
            mode_str = "(BULK)";
            lastModeChange = now;
            floatHoldStart = 0;
            absorptionHoldStart = 0;
            DEBUG_PRINTLN(F("Transition: FLOAT -> BULK"));
          }
        } else {
          floatHoldStart = 0;
        }
      }
      break;
    default:
      break;
  }
}
//************************************************************************************
// Adjust PWM Based on Voltage/Current Error (CC/CV Algorithm)
// Fixed-step control for stability with noisy ADC and inductive components
// All hysteresis values sourced from battery_types.h (no magic numbers)
//************************************************************************************
void adjustPWM(float targetVoltage, float maxCurrent) {
  if (mode == BACKUP || mode == IDLE) {
    pwm_value = 0;
    pwm_last = 0;
    writePWMClamped(pwm_value);
    return;
  }

  // SOFT-START RE-TRIGGER PREVENTION: Execute only once per charge cycle
  static bool softStartDone = false;
  if (mode == BACKUP || mode == IDLE) {
    softStartDone = false;
  }

  // CURRENT-QUALIFIED SOFT START: Don't ramp unless current is flowing
  const float SOFTSTART_MIN_CURRENT = 0.01f; // Lower to 10mA for smoother startup
  if (!softStartDone && Current_A < SOFTSTART_MIN_CURRENT) {
    // Don't start soft-start unless we see some current
    pwm_value = 20;
    pwm_last  = 20;
    writePWMClamped(20);   // allow tiny PWM to detect current
    return;
  }
  softStartDone = true;

  // Surge current protection with timing control
  unsigned long now = millis();
  if (now - lastPWMChange < MIN_PWM_CHANGE_INTERVAL) {
    return; // Skip PWM update too soon
  }

  // Rate limiting based on current
  const float currentRampRate = 0.8f; // Slower ramp rate for stability
  float maxDeltaCurrent = currentRampRate * (MIN_PWM_CHANGE_INTERVAL / 1000.0f);
  if (fabs(Current_A - lastCurrent) > maxDeltaCurrent) {
    // Current changing too fast - limit PWM change
    int maxPWMDelta = 2; // HALVED: 2 instead of 4
    if (pwm_value > lastPWMValue) {
      pwm_value = min(lastPWMValue + maxPWMDelta, pwm_value);
    } else {
      pwm_value = max(lastPWMValue - maxPWMDelta, pwm_value);
    }
  }

  lastCurrent = Current_A;
  lastPWMValue = pwm_value;
  lastPWMChange = now;

  // === IR2104 CHANGE ===
  // ANTI-COLLAPSE PROTECTION
  // In a Synchronous Buck, if Input < Bat, we must NOT switch,
  // otherwise we might boost current backwards from Bat to Input
  // (depending on diode configuration).
  // Force OFF if input headroom is gone.
  //  if (input_voltage < (bat_voltage + 0.5f)) { // Tightened threshold
  //    pwm_value = 0; // Force Low-Side ON (Freewheel/Block)

  if (input_voltage < (bat_voltage + 1.5f)) { // Looser threshold
    pwm_value = max(pwm_value - 20, 0); // Gradual reduction instead of hard cutoff


    pwm_last = 0;
    writePWMClamped(pwm_value);
    return;
  }

  // Detect if we are close to collapsing to inhibit aggressive PWM increases
  bool nearCollapse = (input_voltage < (bat_voltage + 2.0));

  // MODE-SPECIFIC ABSOLUTE VOLTAGE SAFETY: Use correct safety limit based on mode
  float currentSafetyLimit = BULK_SAFETY_LIMIT;
  if (mode == FLOAT) {
    currentSafetyLimit = FLOAT_SAFETY_LIMIT;
  }

  // SAFETY OVERRIDE ENFORCEMENT: Check absolute maximum limits
  if (bat_voltage > currentSafetyLimit || Current_A > maxCurrent * 1.5f) {
    // Safety violation - reduce PWM aggressively and freeze PWM increase
    pwm_value = max(pwm_value - 30, 0);
    writePWMClamped(pwm_value);
    return; // Freeze PWM increase while safety violation exists
  }

  // === IR2104 CHANGE ===
  // Updated Control Limit to user-defined percentage
  const int SAFETY_MAX_PWM = PWM_MAX_DUTY;

  // FIXED-STEP CONTROL: Small fixed steps for stability (no magic numbers)
  const int PWM_STEP_UP = 1;      // HALVED: 1 instead of 8 (0.1% of full scale)
  const int PWM_STEP_DOWN = 2;    // HALVED: 2 instead of 4 (0.2% of full scale)
  const int PWM_STEP_SAFETY = 64; // Large step for safety violations (6.3% of full scale)

  // FIXED-STEP LOGIC: Determine if we need more or less power
  bool need_more_power = false;
  bool need_less_power = false;

  if (mode == BULK) {
    // CONSTANT CURRENT MODE: Target is current, not voltage
    float currentError = maxCurrent - Current_A;

    if (currentError > 0.2f) {
      // Need more current - increase PWM with fixed step
      need_more_power = true;
    } else if (currentError < -0.1f) {
      // Need less current - decrease PWM with fixed step
      need_less_power = true;
    }

    // Voltage ceiling check for CC mode
    if (bat_voltage > bulk_voltage_max) {
      need_less_power = true; // Voltage too high in CC mode
    }

  } else {
    // CONSTANT VOLTAGE MODE (Absorption/Float): Target is voltage
    float voltageError = targetVoltage - bat_voltage;

    if (voltageError > 0.1f) {
      // Voltage below target - increase PWM with fixed step
      need_more_power = true;
    } else if (voltageError < -0.05f) {
      // Voltage above target - decrease PWM with fixed step
      need_less_power = true;
    }

    // Current ceiling in CV mode
    if (Current_A > maxCurrent) {
      need_less_power = true; // Current too high in CV mode
    }
  }

  // FIXED-STEP EXECUTION: Apply small fixed steps
  if (need_more_power) {
    pwm_value += PWM_STEP_UP;      // small fixed step up
    DEBUG_PRINTF("Fixed-step: Need more power. PWM: %d\n", pwm_value);
  }
  else if (need_less_power) {
    pwm_value -= PWM_STEP_DOWN;    // small fixed step down
    DEBUG_PRINTF("Fixed-step: Need less power. PWM: %d\n", pwm_value);
  }

  // Smooth limiting using global pwm_last (The ramping controller)
  int delta = pwm_value - pwm_last;
  if (delta > 16) { // Scaled x4
    pwm_value = pwm_last + 16; // Absolute safety cap on rate of rise
  } else if (delta < -16) { // Scaled x4
    pwm_value = pwm_last - 16;
  }

  // === IR2104 CHANGE ===
  // Ensure the control variable never exceeds the user-defined limit
  pwm_value = constrain(pwm_value, 0, SAFETY_MAX_PWM);
  pwm_last = pwm_value;
  writePWMClamped(pwm_value);
}



//************************************************************************************
// Load Control Policy (AUTO mode)
// Simple rule ONLY:
// - Input power removed (below input_voltage_min) → LOAD ON
// - Input power present → LOAD OFF
// Battery low, overload, debounce logic preserved
//************************************************************************************
void updateLoadStatus() {

  if (loadControlMode != AUTO) {
    if (pendingLoadChange) finalizeLoadToggle();
    return;
  }

  if (tempLoadOn) {
    if (pendingLoadChange) finalizeLoadToggle();
    return;
  }

  unsigned long now = millis();

  if ((now - lastLoadChangeTime) >= debounceDelay &&
      !pendingLoadChange &&
      !loadWasTripped) {

    // Detect input power
    bool inputMissing = (input_voltage < input_voltage_min);

    // Battery safety
    bool battSafeOn    = (bat_voltage >= (battery_voltage_hysteresis + battery_voltage_min));
    bool battTooLowOff = (bat_voltage <  (battery_voltage_min - battery_voltage_hysteresis));

    // FINAL LOAD DECISION (ONLY input based)
    bool shouldTurnOnLoad  = (!load_is_on && inputMissing && battSafeOn);
    bool shouldTurnOffLoad = ( load_is_on && (!inputMissing || battTooLowOff) );

    if (shouldTurnOnLoad || shouldTurnOffLoad) {
      nextToggleIsManual = false;
      scheduleLoadToggle(shouldTurnOnLoad);
    }
  }

  if (pendingLoadChange) finalizeLoadToggle();
}



//************************************************************************************
// Breathing LED (Maps Brightness/Pulse Speed to Voltage)
// Enhanced algorithm with smoother animation using sine wave modulation
//************************************************************************************
void BattStatusLED_Pulse() {
  static unsigned long lastPulseUpdate = 0;
  static float pulsePhase = 0.0;
  unsigned long now = millis();
  // Update every 25ms for smoother animation
  if (now - lastPulseUpdate < 25) return;
  lastPulseUpdate = now;

  // Convert battery voltage to millivolts for processing
  int voltage_mV = bat_voltage * 1000;
  // Turn off LED for critically low voltage
  if (voltage_mV <= 8500) {
    // Use compatibility macro for v3.0+ support
    COMPAT_LEDC_WRITE(BATTSTATUS_LED, ledChannel, 0);
    return;
  }

  // Full brightness for very high voltage
  if (voltage_mV > 13700) {
    // Use compatibility macro for v3.0+ support
    COMPAT_LEDC_WRITE(BATTSTATUS_LED, ledChannel, 255);
    return;
  }

  // Constrain voltage to operational range
  voltage_mV = constrain(voltage_mV, 8000, 14000);
  // Calculate voltage ratio for dynamic behavior
  float voltageRatio = (voltage_mV - 8000) / float(14000 - 8000);
  // Dynamic brightness range based on voltage
  int brightnessMax = 180 + voltageRatio * (255 - 180); // 180-255 range
  int baseBrightness = 10;  // Minimum brightness to avoid complete darkness
  int pulseAmplitude = brightnessMax - baseBrightness;
  // Variable pulse speed - higher voltage = faster pulse
  float speedFactor = 0.015 + voltageRatio * 0.045; // 0.015-0.06 range
  pulsePhase += speedFactor;
  if (pulsePhase > TWO_PI) pulsePhase -= TWO_PI;
  // Sine wave modulation for smooth breathing effect
  float sineValue = (sin(pulsePhase) + 1.0) / 2.0;
  int pwmValue = baseBrightness + sineValue * pulseAmplitude;

  // Apply PWM to breathing LED using compatibility macro
  COMPAT_LEDC_WRITE(BATTSTATUS_LED, ledChannel, pwmValue);
}

//************************************************************************************
// Update Battery Status LEDs and LCD Display
// Uses battery-type-specific thresholds for accurate LED indication
//************************************************************************************
void updateBatteryStatusLED() {
  const int pct = constrain(getBatteryPercentage(bat_voltage), 0, 100);
  // Determine battery-type-specific thresholds for LED indication
  int crit_threshold, warn_threshold, good_threshold;
  switch (selectedBattery) {
    case LEAD_ACID_12V_7AH:
      crit_threshold = 20; // 20% for Lead Acid
      warn_threshold = 50; // 50% for Lead Acid
      good_threshold = 95; // 95% for Lead Acid
      break;

    case LIFEPO4_12V:
      crit_threshold = 10; // 10% for LiFePO4 (more aggressive protection)
      warn_threshold = 30; // 30% for LiFePO4
      good_threshold = 90; // 90% for LiFePO4
      break;

    case LIION_3S:
      crit_threshold = 15; // 15% for Li-ion (aggressive protection)
      warn_threshold = 25; // 25% for Li-ion
      good_threshold = 85; // 85% for Li-ion
      break;

    default:
      crit_threshold = 20;
      warn_threshold = 50;
      good_threshold = 95;
      break;
  }

  const bool critByVoltage = (bat_voltage <= (battery_voltage_min + CRIT_BATT_WARN_MARGIN));
  const bool critical = critByVoltage || (pct <= crit_threshold);

  bool criticalOn = false;
  bool mediumOn = false;
  bool fullOn = false;
  bool criticalFlash = false;
  bool mediumFlash = false;
  bool fullFlash = false;

  if (pct <= crit_threshold) {
    criticalOn = true;
    criticalFlash = true;
  } else if (pct <= warn_threshold) {
    criticalOn = true;
    mediumOn = true;
    mediumFlash = true;
  } else if (pct <= good_threshold) {
    criticalOn = true;
    mediumOn = true;
    fullOn = true;
    fullFlash = true;
  } else {
    criticalOn = true;
    mediumOn = true;
    fullOn = true;
  }

  static unsigned long lastFlashMillis = 0;
  const unsigned long FLASH_CYCLE_MS = 2000UL;
  const unsigned long FLASH_ON_DURATION_MS = 1000UL;
  const unsigned long now = millis();
  unsigned long elapsed = now - lastFlashMillis;

  bool shouldToggle = (elapsed >= FLASH_CYCLE_MS);
  if (shouldToggle) {
    lastFlashMillis = now;
  }

  bool flashState = (elapsed < FLASH_ON_DURATION_MS);
  digitalWrite(LED_CRITICAL, criticalOn && (!criticalFlash || flashState) ? HIGH : LOW);
  digitalWrite(LED_MEDIUM,   mediumOn && (!mediumFlash || flashState) ? HIGH : LOW);
  digitalWrite(LED_FULL,     fullOn && (!fullFlash || flashState) ? HIGH : LOW);

  const char* statusText;
  if (pct <= crit_threshold) {
    statusText = "{Crit}";
  } else if (pct <= warn_threshold) {
    statusText = "{Warn}";
  } else if (pct <= good_threshold) {
    statusText = "{Good}";
  } else {
    statusText = "{Full}";
  }

  lcd.setCursor(7, 2);
  lcd.print(statusText);
  int pad = 8 - strlen(statusText);
  while (pad-- > 0) lcd.print(' ');

  static bool wasCritical = false;
  static unsigned long lastBuzzerMillis = 0;
  const unsigned long CRIT_BEEP_PERIOD_MS = 60000UL;

  if (pct <= crit_threshold) {
    if (!wasCritical || (now - lastBuzzerMillis >= CRIT_BEEP_PERIOD_MS)) {
      playCriticalBeep();
      lastBuzzerMillis = now;
    }
  }

  wasCritical = (pct <= crit_threshold);
}

//************************************************************************************
// LCD Update (Throttled)
// Updates display with system status information using proper battery voltage mapping
// Uses battery-type-specific voltage ranges for accurate icon display
// Includes division by zero guard for duty calculation
//************************************************************************************
void Update_Main_LCD() {
  // Throttle LCD updates to prevent flickering and reduce processing overhead
  if (millis() - last_lcd_update < LCD_REFRESH_RATE_MS) return;
  last_lcd_update = millis();

  // Map battery voltage to icon index (0-5) using battery-type-specific range
  int battLevelIndex;
  float min_volt, max_volt;
  switch (selectedBattery) {
    case LEAD_ACID_12V_7AH:
      min_volt = 10.5f;
      max_volt = 12.7f;
      break;
    case LIFEPO4_12V:
      min_volt = 10.0f;
      max_volt = 13.5f;
      break;
    case LIION_3S:
      min_volt = 9.0f;
      max_volt = 12.6f;
      break;
    default:
      min_volt = MIN_BATTERY_VOLTAGE;
      max_volt = MAX_BATTERY_VOLTAGE;
      break;
  }

  battLevelIndex = (int)((bat_voltage - min_volt) * 5.0 / (max_volt - min_volt));
  battLevelIndex = constrain(battLevelIndex, 0, 5);

  // Display battery level icon in top right corner
  lcd.setCursor(11, 0); lcd.write(battLevelIndex);
  // Display temperature with degree symbol in top right corner
  lcd.setCursor(15, 0);
  if (tempValid) {
    int roundedTemp = round(temperatureC);
    lcd.print(roundedTemp);
  } else {
    lcd.print("--");
  }
  lcd.write(223); // Degree symbol (°)

  // Display power or battery icon based on input voltage availability
  lcd.setCursor(0, 2);
  if (input_voltage <= input_voltage_min) {
    // Low input voltage - show battery icon
    lcd.write(battLevelIndex);
    if (!rebootNotified) {
      lcd.setCursor(0, 3);
      lcd.write(battLevelIndex);
    }
  } else {
    // Adequate input voltage - show power icon
    lcd.write(POWER_ICON);
    if (!rebootNotified) {
      lcd.setCursor(0, 3);
      lcd.write(POWER_ICON);
    }
  }

  // Display input and battery voltages with 2 decimal places
  lcd.setCursor(0, 1);
  lcd.print(input_voltage, 2);
  lcd.print("v ");
  lcd.setCursor(7, 1); lcd.print(bat_voltage, 2); lcd.print("v    ");
  // === IR2104 CHANGE ===
  // Display % relative to the user-defined safety limit (PWM_MAX_DUTY)
  // User sees 100% when system is at max allowed power (user-defined %)
  int maxDuty = PWM_MAX_DUTY;
  // Division by zero guard with proper implementation
  int dutyPct = 0;
  if (maxDuty > 0) {
    dutyPct = (pwm_value * 100) / maxDuty;
    dutyPct = constrain(dutyPct, 0, 100); // Ensure within bounds
  }

  lcd.setCursor(15, 1);
  lcd.print("   "); // Fixed-width clear

  // Display PWM duty percentage
  lcd.setCursor(15, 1);
  lcd.print(dutyPct);
  lcd.print("%");
  // Display current in amps (A) or milliamps (mA) based on magnitude
  lcd.setCursor(1, 2);
  if (Current_A >= 1.0) {
    lcd.print(Current_A, 2);
    lcd.print("A ");
  } else {
    lcd.print(Current_A * 1000, 0);
    lcd.print("mA ");
  }

  // Display battery percentage and battery level icon using battery-type-specific calculation
  lcd.setCursor(15, 2); lcd.print(getBatteryPercentage(bat_voltage)); lcd.print("% ");
  lcd.setCursor(19, 2); lcd.write(battLevelIndex);

  // Toggle mode visibility every second for flashing effect
  if (millis() - last_flash_update >= 1000) {
    last_flash_update = millis();
    mode_visible = !mode_visible;
  }

  // Display WiFi connection status with blinking indicator when connected
  {
    static unsigned long lastWiFiBlink = 0;
    static bool wifiBlink = false;
    if (WiFi.status() == WL_CONNECTED) {
      // Blink WiFi indicator every 400ms when connected
      if (millis() - lastWiFiBlink >= 400) {
        lastWiFiBlink = millis();
        wifiBlink = !wifiBlink;
      }
      lcd.setCursor(13, 0); lcd.print(wifiBlink ? "!" : " ");
    } else {
      // Clear WiFi indicator when disconnected
      lcd.setCursor(13, 0);
      lcd.print(" ");
    }
  }

  // Display mode, load status, and control mode on bottom line (unless reboot countdown active)
  if (!rebootNotified) {
    lcd.setCursor(7, 3);
    // Flash mode string on/off for visibility
    if (mode_visible) lcd.print(mode_str);
    else lcd.print("        ");
    lcd.setCursor(15, 3);
    if (loadWasTripped) {
      lcd.print("off");
    } else {
      lcd.print(load_status);
    }
    lcd.setCursor(19, 3);
    if (loadProtectionEnabled && loadWasTripped) {
      lcd.print("P"); // Protection active/tripped
    } else {
      lcd.print(loadControlModeStr);
    }
  }
}

//************************************************************************************
// Spinner (Visual Activity Indicator)
// Provides visual feedback of system activity
//************************************************************************************
void SPINNER_C() {
  static uint8_t index = 0;
  const char spinner_chars[] = { '*', '*', '*', ' ', ' ' };
  lcd.setCursor(12, 0); lcd.print(spinner_chars[index++ % sizeof(spinner_chars)]);
}
void SPINNER_X() {
  static uint8_t index = 0;
  const char spinner_chars[] = { 'X', 'X', 'X', ' ', ' ' };
  lcd.setCursor(12, 0); lcd.print(spinner_chars[index++ % sizeof(spinner_chars)]);
}
void SPINNER_Status() {
  static unsigned long lastUpdate = 0;
  const unsigned long interval = 300;
  if (millis() - lastUpdate >= interval) {
    lastUpdate = millis();
    if (pwm_value > 0) SPINNER_C(); else SPINNER_X();
  }
}

//************************************************************************************
// Power Readout (W): Input when Present, Battery Otherwise
// Displays real-time power consumption or generation
//************************************************************************************
void Battery_Power_Display() {
  if (rebootNotified) return;
  lcd.setCursor(1, 3);

  const float POWER_THRESHOLD = 0.02f;

  if (input_voltage > input_voltage_min) {
    // Input is available - show input power
    float inputPower = Current_A * input_voltage;
    if (inputPower >= POWER_THRESHOLD) {
      lcd.print(inputPower, 1); lcd.print("W ");
    } else {
      lcd.print(" 0.0W ");
    }
  } else {
    // No input - show battery power consumption (no negative sign)
    float batteryPower = Current_A * bat_voltage;
    if (batteryPower >= POWER_THRESHOLD) {
      lcd.print(batteryPower, 1); lcd.print("W ");
    } else {
      lcd.print(" 0.0W ");
    }
  }
}

//************************************************************************************
// Serial Logs (Rate-limited to 1 Hz)
// Includes division by zero guard in duty calculation for safety
//************************************************************************************
void logDataToSerial() {
  static unsigned long last = 0;
  const unsigned long period = 1000;
  if (millis() - last < period) return;
  last = millis();

  DEBUG_PRINT("["); DEBUG_PRINT(millis() / 1000);
  DEBUG_PRINT("s] ");
  DEBUG_PRINT("Input V: "); DEBUG_PRINT(input_voltage, 2); DEBUG_PRINT(" V  ");
  DEBUG_PRINT("Batt V: ");  DEBUG_PRINT(bat_voltage, 2); DEBUG_PRINT(" V  ");
  DEBUG_PRINT("Current: "); DEBUG_PRINT(Current_A, 2); DEBUG_PRINT(" A  ");

  // === IR2104 CHANGE ===
  // Log % relative to user-defined limit
  int maxDuty = PWM_MAX_DUTY;
  // Division by zero guard
  int dutyPct = 0;
  if (maxDuty > 0) {
    dutyPct = (pwm_value * 100) / maxDuty;
    dutyPct = constrain(dutyPct, 0, 100);
  }

  DEBUG_PRINT("PWM: ");     DEBUG_PRINT(dutyPct);           DEBUG_PRINT("%  ");
  DEBUG_PRINT("(Limit: ");  DEBUG_PRINT(PWM_FULL_SCALE_LIMIT); DEBUG_PRINT("%)  ");

  // Assumes fan_control.h exposes getFanDuty() and FAN_MAX_DUTY
  int fanDutyPct = (getFanDuty() * 100) / FAN_MAX_DUTY;
  DEBUG_PRINT("FanPWM: ");  DEBUG_PRINT(fanDutyPct);
  DEBUG_PRINT("%  ");
  DEBUG_PRINT("Temp: ");    DEBUG_PRINT(temperatureC, 1);  DEBUG_PRINT(" °C  ");
  DEBUG_PRINT("Mode: ");    DEBUG_PRINT(mode_str);         DEBUG_PRINT("  ");
  DEBUG_PRINT("Load: ");    DEBUG_PRINT(load_status);
  DEBUG_PRINT(" | LoadMode: "); DEBUG_PRINT(loadControlModeStr);

  DEBUG_PRINT(" | Calib: "); DEBUG_PRINT(ACS_OFFSET_VOLTAGE, 3); DEBUG_PRINTLN(" V");
}
