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
