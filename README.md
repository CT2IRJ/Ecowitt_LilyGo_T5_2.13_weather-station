# 🌤️ Ecowitt Weather Station Display for LilyGo T5 2.13" E-Paper

An ultra-low-power e-paper display and modern local web dashboard for **Ecowitt** weather stations, built for the **LilyGo T5 2.13" (V2.3 / V2.4)** ESP32 board.

---

## 🚀 One-Click Web Installer

Flash the firmware directly to your LilyGo T5 2.13" from your browser (Chrome, Edge, or Opera) without installing Arduino IDE or Python:

👉 **[Install via ESP Web Tools](https://esptool.spacehuhn.com/)**

---

## ✨ Features

* **Crisp E-Paper Summary (`250x122`):** Displays outdoor temperature, wind speed & cardinal direction, daily rainfall, outdoor humidity, solar radiation, station battery status, IP address, and last sync timestamp.
* **Modern Local Web Dashboard (`http://<ESP32-IP>/`):**
  * Responsive card layout with automatic **Light / Dark mode**.
  * Extended telemetry: Feels-like temperature, dew point, wind gusts & degrees, rain rate, UV index, barometric pressure, and indoor temperature/humidity.
  * **Interactive 24-Hour History Chart:** Lightweight SVG chart tracking Temperature (`°C`), Humidity (`%`), and Pressure (`hPa`) over the last 96 readings (15-minute intervals).
  * Manual sync button (`🔄 Atualizar`) and onboard button trigger (`IO39`).
* **Zero-Code Configuration (`http://<ESP32-IP>/setup`):**
  * Captive portal (`LilyGo-Weather-Setup`) via `WiFiManager` for effortless Wi-Fi onboarding.
  * Persistent NVS storage (`Preferences`) for Ecowitt **Application Key**, **API Key**, **Station MAC Address**, and custom refresh interval.
* **Automatic Daylight Saving Time:** Configured for Portugal (`WET0WEST`), easily adaptable to any POSIX timezone string.

---

## 🛠️ Hardware & Pinout

* **Board:** LilyGo T5 2.13" E-Paper (ESP32, `DEPG0213BN` / `GDEM0213B74` B&W driver)

| E-Paper Signal | ESP32 GPIO | Notes |
| :--- | :---: | :--- |
| **CS** | `5` | SPI Chip Select |
| **DC** | `17` | Data / Command |
| **RST** | `16` | Reset |
| **BUSY** | `4` | Busy status input |
| **POWER** | `12` | Display power control (V2.4 boards) |
| **BUTTON** | `39` | Onboard button (triggers immediate refresh) |

---

## ⚙️ Initial Setup Guide

1. **Flash the Firmware:** Use the [Web Installer](https://github.io) or compile `Ecowitt_LilygoT5_2.13.ino` with Arduino IDE.
2. **Connect to Wi-Fi:**
   * On first boot, the display instructs you to connect to the temporary Wi-Fi access point **`LilyGo-Weather-Setup`**.
   * Select your home Wi-Fi network and enter your password.
3. **Configure Ecowitt API Credentials:**
   * Note the IP address displayed in the bottom-left footer of the e-paper screen.
   * Open `http://<ESP32-IP>/` in your browser (it automatically redirects to `/setup` on first run).
   * Enter your **Application Key**, **API Key** (from your [Ecowitt.net](https://ecowitt.net) user profile), and your weather station's **MAC Address**.
   * Click **Guardar & Atualizar Ecrã**.

---

## 💻 Compiling from Source (Arduino IDE)

If you prefer to build and customize the code yourself:

1. Install the **ESP32** board package in Arduino IDE.
2. Install the following libraries via the Arduino Library Manager:
   * `GxEPD2` (by Jean-Marc Zingg)
   * `Adafruit GFX Library`
   * `WiFiManager` (by tzapu)
   * `ArduinoJson` (v7+)
3. Select **ESP32 Dev Module** (or **TTGO T1**) as your target board, open `Ecowitt_LilygoT5_2.13.ino`, and upload.

---

## 📄 License

This project is open-source and available under the [MIT License](LICENSE).
