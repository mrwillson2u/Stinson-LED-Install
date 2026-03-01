# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based Art-Net DMX lighting controller. Receives Art-Net data over WiFi and outputs to two serial LED buses. Includes MQTT telemetry, OTA firmware updates, WiFi configuration portal, and DHT22 temperature/humidity monitoring.

## Build & Upload Commands

Requires `python-dotenv` (`pip install python-dotenv`).

```bash
# Compile firmware
pio run -e esp32doit-devkit-v1

# Upload via USB/serial
pio run -e esp32doit-devkit-v1 --target upload

# Upload via OTA (set UPLOAD_PORT in .env to ESP32 IP)
pio run -e esp32_ota --target upload

# Monitor serial output
pio device monitor -b 115200
```

Firmware binary output: `.pio/build/esp32doit-devkit-v1/firmware.bin`

## Configuration

Environment variables are injected as C++ preprocessor defines at build time via `extra_script.py`, which reads from `.env`. Copy `template.env` to `.env` and fill in values. Key variables: `HOSTNAME`, `MQTT_ENABLED`, `MQTT_IP`, `MQTT_USER`, `MQTT_PASSWORD`, `UPLOAD_PORT`.

## Architecture

Single-file application (`src/main.cpp`) running on ESP32 with Arduino framework.

**Data flow:** Art-Net WiFi packets → `onDmxFrame()` callback → serial writes to Bus A (UART1) and Bus B (UART2).

**Serial protocol:** Each bus write sends a 17-byte packet: 4-byte prefix `{149, 1, 250, 0}` + 12 data bytes + 1 checksum byte (sum of prefix + data, mod 256). Writes are timing-sensitive with 575μs delays between bytes.

**Hardware pin assignments:**
- Bus A: TX=17, RX=16, Enable=21 (Serial1, 38400 baud)
- Bus B: TX=19, RX=18, Enable=23 (Serial2, 38400 baud)
- DHT22: GPIO 27
- Config trigger button: GPIO 0

**Art-Net mapping:** Universe channels 0-7 → Bus A, channels 8-15 → Bus B (max 16 channels).

**Key timing constraints:**
- DHT reads every 5 minutes; Art-Net processing pauses for 30s around DHT reads
- MQTT reconnect retry every 5 seconds
- Serial byte writes spaced 575μs apart

**MQTT topics:** `outdoor-led/temp`, `outdoor-led/humidity`, `outdoor-led/color`

## Dependencies (platformio.ini)

- `someweisguy/esp_dmx` - DMX communication
- `rstephan/ArtnetWifi` - Art-Net protocol
- `tzapu/WiFiManager` - WiFi captive portal config
- `robtillaart/DHTNEW` - DHT22 sensor
- `knolleary/PubSubClient` - MQTT client
