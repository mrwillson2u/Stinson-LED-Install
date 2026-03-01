# Stinson LED Controller

ESP32-based Art-Net DMX lighting controller for outdoor LED installations. Receives Art-Net data over WiFi and drives two serial LED buses. Designed for unattended outdoor operation with automatic recovery from WiFi outages, hardware watchdog protection, and OTA firmware updates.

## Architecture

Single-file firmware (`src/main.cpp`) running on an ESP32 DOIT DevKit V1 with the Arduino framework via PlatformIO.

### System Diagram

```
                        +-----------------+
  Home Assistant        |                 |     Serial Bus A (UART1)
  (Art-Net source) ---->|  ESP32 DevKit   |----> LED Controller A
        WiFi            |                 |
                        |   main.cpp      |     Serial Bus B (UART2)
  MQTT Broker <-------->|                 |----> LED Controller B
        WiFi            |                 |
                        |     DHT22 ------|  (temp/humidity sensor)
                        +-----------------+
```

### Data Flow

1. **Art-Net In:** ESP32 receives Art-Net UDP packets over WiFi
2. **DMX Callback:** `onDmxFrame()` stores channel data into `outputDataA[]` / `outputDataB[]`, sets `dataChanged` flag, and returns immediately (no blocking work in the callback)
3. **Packet Build:** `loop()` detects `dataChanged`, rebuilds 17-byte serial packets with prefix + data + checksum
4. **Serial Out:** Packets are written to Bus A (Serial1) and Bus B (Serial2) at 38400 baud
5. **Telemetry:** MQTT publishes color state (throttled to 1/sec), temperature, and humidity

### Main Loop Timing

The loop uses a three-tier serial refresh strategy:

| State | Refresh Rate | Trigger |
|-------|-------------|---------|
| **Active** | Every 575us (full rate) | New Art-Net frame received |
| **Holdoff** | Every 575us (full rate) | Within `ACTIVE_HOLDOFF_MS` (5s) of last Art-Net frame |
| **Idle** | Every `IDLE_REFRESH_MS` (500ms) | No recent Art-Net activity |

The holdoff window keeps serial output at full rate during the entire duration of a Home Assistant fade transition, which sends many Art-Net frames over several seconds.

## Serial LED Protocol

The downstream LED controllers use a proprietary serial protocol. Each write sends a **17-byte packet** over UART at **38400 baud, 8N1**:

```
Byte:  [0]  [1]  [2]  [3]  [4] [5] [6] ... [15]  [16]
       149   1   250   0   D0  D1  D2  ...  D11   CHK
       |--- prefix ---|   |--- 12 data bytes ---|  |checksum|
```

| Field | Bytes | Value | Description |
|-------|-------|-------|-------------|
| Prefix | 0-3 | `{149, 1, 250, 0}` | Fixed packet header |
| Data | 4-15 | 0-250 | Channel values (clamped, see below) |
| Checksum | 16 | computed | `(sum of all 16 preceding bytes) % 256` |

### Output Value Clamping

Output values are clamped to `MAX_OUTPUT_VALUE` (default: 250). The LED controllers interpret `0xFF` (255) as a control/sync byte rather than a data value, which causes a steady 500ms on/off blink pattern when any channel is set to 255. Clamping to 250 avoids this entirely with negligible brightness loss.

### Bus Wiring

| Bus | UART | TX Pin | RX Pin | Enable Pin | Baud |
|-----|------|--------|--------|------------|------|
| A | Serial1 | GPIO 17 | GPIO 16 | GPIO 21 | 38400 |
| B | Serial2 | GPIO 19 | GPIO 18 | GPIO 23 | 38400 |

Enable pins are held HIGH during normal operation.

## Art-Net Channel Mapping

The controller listens on a single Art-Net universe and maps 16 channels to two buses:

| Art-Net Channel | Destination |
|-----------------|-------------|
| 0-7 | Bus A (`outputDataA[0-7]`) |
| 8-15 | Bus B (`outputDataB[0-7]`) |

Channels beyond 16 are ignored. Each bus has 12 data slots in the serial packet, but only the first 8 are populated from Art-Net (slots 8-11 remain zero).

## Hardware

- **MCU:** ESP32 DOIT DevKit V1 (240MHz, 320KB RAM, 4MB Flash)
- **Sensor:** DHT22 on GPIO 27 (temperature + humidity, read every 5 minutes)
- **Config Button:** GPIO 0 (pull LOW to launch WiFi config portal)
- **LED Buses:** Two serial LED controllers connected via UART (see bus wiring above)

## Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- `python-dotenv` in PlatformIO's Python environment:
  ```bash
  ~/.platformio/penv/bin/pip install python-dotenv
  ```

### Configuration

1. Copy `template.env` to `.env`:
   ```bash
   cp template.env .env
   ```

2. Edit `.env` with your values:
   ```env
   HOSTNAME=outdoor-led-control
   MQTT_ENABLED=true
   MQTT_IP=10.0.1.12
   MQTT_USER=homeassistant
   MQTT_PASSWORD=your-password
   UPLOAD_PORT=10.0.1.44
   STATIC_IP=10.0.1.44
   GATEWAY=10.0.1.1
   SUBNET=255.255.255.0
   ```

| Variable | Required | Description |
|----------|----------|-------------|
| `HOSTNAME` | No | mDNS hostname (default: `outdoor-led-control`) |
| `MQTT_ENABLED` | No | Set to `true` to enable MQTT telemetry |
| `MQTT_IP` | If MQTT enabled | MQTT broker IP address |
| `MQTT_USER` | If MQTT enabled | MQTT username |
| `MQTT_PASSWORD` | If MQTT enabled | MQTT password |
| `UPLOAD_PORT` | For OTA | ESP32 IP address for OTA uploads |
| `STATIC_IP` | No | Static IP for ESP32 (leave blank for DHCP) |
| `GATEWAY` | With STATIC_IP | Gateway IP |
| `SUBNET` | With STATIC_IP | Subnet mask |

Environment variables are injected as C++ preprocessor defines at build time via `extra_script.py`.

### Static IP vs DHCP

If `STATIC_IP` is set, the ESP32 will use that address. If left blank, it falls back to DHCP. Static IP is recommended when the ESP32 is behind a WiFi repeater where the router's DHCP server can't assign reservations.

The device is also reachable via mDNS at `<HOSTNAME>.local` (e.g., `outdoor-led-control.local`), though mDNS reliability varies across network segments and repeaters.

## Build & Upload

```bash
# Compile firmware
pio run -e esp32doit-devkit-v1

# Upload via USB/serial
pio run -e esp32doit-devkit-v1 --target upload

# Upload via OTA (requires UPLOAD_PORT set in .env)
pio run -e esp32_ota --target upload

# Monitor serial output
pio device monitor -b 115200
```

The `esp32doit-devkit-v1` environment always uploads via USB. The `esp32_ota` environment uses the IP from `UPLOAD_PORT` in `.env`. The first upload after major firmware changes must be via USB since the device needs the new OTA handler code.

Firmware binary: `.pio/build/esp32doit-devkit-v1/firmware.bin`

### Platform Version

The platform is pinned to `espressif32@6.9.0` (Arduino-ESP32 2.x / ESP-IDF 4.4). Newer platform versions (espressif32 55+) use ESP-IDF 5.x which breaks the `esp_dmx` library dependency.

## MQTT

### Topics

| Topic | Direction | Payload | Description |
|-------|-----------|---------|-------------|
| `outdoor-led/temp` | Publish | `"23.50"` | Enclosure temperature (Celsius) |
| `outdoor-led/humidity` | Publish | `"45.20"` | Enclosure humidity (%) |
| `outdoor-led/color` | Publish | `"R,G,B,W,R,G,B,W"` | Current channel values (Bus A first 4, Bus B first 4) |
| `outdoor-led/availability` | Publish | `"online"` / `"offline"` | Device availability (Last Will and Testament) |

### Availability (Last Will)

The MQTT connection is configured with a Last Will message: if the ESP32 disconnects unexpectedly, the broker automatically publishes `"offline"` to `outdoor-led/availability`. On successful connect, the device publishes `"online"` (retained). This integrates with Home Assistant's availability tracking.

### Publish Rates

- **Temperature/Humidity:** Every 5 minutes (only on successful DHT read)
- **Color state:** At most once per second, only when Art-Net data has changed
- **Availability:** On connect/disconnect

## Reliability Features

### Hardware Watchdog (30s)

The ESP32 Task Watchdog Timer is initialized with a 30-second timeout. If `loop()` ever stalls (hung WiFi call, blocked serial, etc.), the ESP32 automatically reboots. The watchdog is fed on every loop iteration and during OTA progress updates.

### WiFi Auto-Recovery

- **Boot:** WiFi connect has a 60-second timeout, config portal has a 120-second timeout. If both fail, the ESP32 reboots to retry.
- **Runtime:** If WiFi disconnects, `WiFi.reconnect()` is called immediately. If WiFi stays down for 5 minutes, the ESP32 reboots for a clean reconnection.

### OTA Stability

During an OTA firmware upload, all non-essential processing (Art-Net, serial output, MQTT, DHT reads) is paused via the `otaInProgress` flag. This frees CPU and WiFi bandwidth for the upload, significantly improving reliability over weak WiFi links or repeaters. The watchdog is fed during OTA progress so it doesn't trigger mid-upload.

### DHT Error Handling

Sensor values are only read and published to MQTT when the DHT22 returns a successful read (`DHTLIB_OK`). Error conditions (checksum, timeout, not ready) are logged but do not publish stale or garbage data.

## Tunable Constants

These are `#define` values at the top of `src/main.cpp`:

| Constant | Default | Description |
|----------|---------|-------------|
| `VERBOSE_OUTPUT` | `false` | Enable detailed serial logging (disable for production -- adds significant CPU load during Art-Net processing) |
| `IDLE_REFRESH_MS` | `500` | Serial refresh rate when idle (ms). Lower = more frequent keep-alive packets to LED controllers. |
| `MAX_OUTPUT_VALUE` | `250` | Maximum value sent to LED controllers. Prevents 0xFF byte being misinterpreted as a control character. |
| `ACTIVE_HOLDOFF_MS` | `5000` | How long to maintain full-rate serial output after the last Art-Net frame (ms). Should cover the longest fade transition. |

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| `rstephan/ArtnetWifi` | ^1.6.1 | Art-Net protocol (UDP) |
| `tzapu/WiFiManager` | ^2.0.17 | WiFi captive portal for initial setup |
| `robtillaart/DHTNEW` | ^0.5.3 | DHT22 temperature/humidity sensor |
| `knolleary/PubSubClient` | ^2.8 | MQTT client |
| `someweisguy/esp_dmx` | ^4.1.0 | DMX communication (build dependency) |

## WiFi Configuration

On first boot (or if saved credentials fail), the ESP32 launches a WiFi configuration portal as an access point. Connect to it and configure your WiFi credentials through the web interface. Press the button on GPIO 0 at any time to re-launch the config portal.

## Project Structure

```
.
├── src/
│   └── main.cpp          # All firmware code (single-file architecture)
├── platformio.ini         # PlatformIO build config (USB + OTA environments)
├── extra_script.py        # Pre-build script: injects .env as preprocessor defines
├── template.env           # Template for environment variables
├── .env                   # Local config (not committed -- copy from template.env)
├── CLAUDE.md              # AI assistant context file
└── README.md              # This file
```
