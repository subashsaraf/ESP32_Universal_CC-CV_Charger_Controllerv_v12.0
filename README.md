# ESP32 Universal CC-CV Charger Controller v12.0

![ESP32](https://img.shields.io/badge/ESP32-Compatible-green)
![CC-CV](https://img.shields.io/badge/Charging-CC--CV-blue)
![Status](https://img.shields.io/badge/Status-Stable-brightgreen)

## 📖 Introduction
The **ESP32 Universal CC-CV Charger Controller v12.0** is an advanced, modular, and safety-focused firmware that converts an **ESP32** into a **smart Constant-Current / Constant-Voltage (CC-CV) battery charger**.

This project is ideal for **DIY battery chargers, solar charging systems, lab power supplies, and educational power-electronics projects**.

---

## ✨ Features

### 🔋 Charging System
- True **CC → CV charging algorithm**
- Multi-stage charger state machine:
  - Idle
  - Bulk (Constant Current)
  - Absorption (Constant Voltage)
  - Float / Maintenance
  - Re-charge / Backup
- Hysteresis-based transitions (prevents PWM oscillation)
- Fully configurable battery parameters

### 🧠 Control & Safety
- Separate **PWM control** and **charging logic**
- ESP32 **LEDC PWM** based power control
- Over-voltage protection
- Over-current protection
- Over-temperature shutdown

### 📡 Interface & Monitoring
- Built-in **Wi-Fi web interface**
- Real-time monitoring:
  - Battery voltage
  - Charging current
  - Charging stage
  - Temperature
- **mDNS support** for easy browser access
- **20×4 I2C LCD** local display
- **Buzzer alerts** for system events

### 🌡 Thermal Management
- **DS18B20 temperature sensor** support
- Automatic **fan control**
- Thermal cut-off protection

---

## 🧩 Supported Hardware

### Controller
- ESP32 DevKit V1 (38-pin recommended)

### Sensors
- ADS1115 – High-resolution voltage sensing
- ACS712 – Current sensor with auto-zero calibration
- DS18B20 – Temperature sensor

### Power Stage
- IR2104 – Half-bridge MOSFET driver
- External high-side & low-side MOSFETs
- PWM-controlled DC-DC converter stage

### Interface
- 20×4 I2C LCD
- Active buzzer
- Cooling fan

---

## 🔋 Battery Configuration
All battery profiles are defined in:

battery_types.h

Configurable parameters:
- Battery chemistry
- Maximum charge voltage
- Maximum charge current
- Float voltage
- Hysteresis thresholds
- Safety cut-off limits

⚠️ **Incorrect battery settings can cause damage or fire. Configure carefully.**

---

## 📁 Project Structure

ESP32_Universal_CC-CV_Charger_Controllerv_v12.0/
│
├── ESP32_Universal_CC-CV_Charger_Controllerv_v12.0.ino
├── battery_types.h
├── charger_web.h / .cpp
├── wifi_sys.h / .cpp
├── fan_control.h / .cpp
├── buzzer_sys.h / .cpp
├── WiFiSignalHelpers.h / .cpp
└── README.md

---

## 🛠 Installation

1. Install **Arduino IDE**
2. Install **ESP32 Board Support**
3. Install required libraries:
   - WiFi / WebServer
   - OneWire & DallasTemperature
   - LiquidCrystal_I2C
   - Adafruit ADS1X15
4. Open the `.ino` file
5. Configure:
   - Wi-Fi credentials
   - Battery parameters
   - Pin assignments
6. Compile and upload to ESP32

---

## ⚠️ Safety Notice

⚠️ This project involves **high voltage and high current** circuits.

- Use proper isolation
- Add fuses and protection circuits
- Test with dummy loads first
- Never leave the charger unattended

**The author is not responsible for hardware damage or injury.**

---

## 🚀 Applications

- DIY CC-CV battery charger
- Solar charge controller
- Bench power supply (CC-CV mode)
- Battery maintenance system
- Educational & research projects

---

## 📜 License

This project is provided for **educational and personal use only**.  
Commercial use requires prior permission from the author.

---

## 👤 Author

**Subash Saraf**

ESP32 Power Electronics Projects

---

⭐ If you find this project useful, please **star the repository** on GitHub!