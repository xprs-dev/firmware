# XPRS Serial Console CLI

The XPRS firmware provides a serial console interface for device control, configuration, and debugging. Connect via UART at 115200 baud.

## Getting Started

Connect to the device's serial port (UART0) using a terminal emulator:

```
screen /dev/ttyUSB0 115200
# or
picocom -b 115200 /dev/ttyUSB0
```

You'll see the prompt:

```
XPRS Serial Console
Type 'help' for available commands

xprs>
```

## Features

- **Line editing**: Use arrow keys to navigate, backspace to delete
- **Command history**: Use up/down arrows to recall previous commands
- **Tab completion**: Press Tab to auto-complete commands
- **JSON output mode**: Switch to JSON output for automation scripts

## Command Reference

### System Commands

#### `status`
Display full device status including firmware version, WiFi, sensors, and memory.

```
xprs> status

=== XPRS Device Status ===

Firmware: 1.0.0
Board: ESP32S3-ePaper-1.54
Callsign: ESPAB12
Uptime: 1h 23m 45s

WiFi: Connected (192.168.1.50)

Sensors:
  Temperature: 23.5 C
  Humidity: 45.2 %

SD Card: Mounted (7.45 GB)
Heap: 245632 bytes free
```

#### `version`
Display firmware version.

```
xprs> version
1.0.0
```

#### `reboot`
Reboot the device.

```
xprs> reboot
Rebooting...
```

#### `heap`
Display free heap memory.

```
xprs> heap
Free heap: 245632 bytes
Minimum free heap: 198456 bytes
```

#### `uptime`
Display device uptime.

```
xprs> uptime
Uptime: 2h 15m 30s
```

#### `format [text|json]`
Get or set the output format. JSON mode is useful for automation.

```
xprs> format
Current format: text

xprs> format json
Output format set to JSON

xprs> status
{"version":"1.0.0","callsign":"ESPAB12","uptime":5025,"wifi":"connected","ip":"192.168.1.51"}
```

#### `log <level>`
Set the ESP-IDF log level.

Levels: `none`, `error`, `warn`, `info`, `debug`, `verbose`

```
xprs> log debug
Log level set to debug
```

### WiFi Commands

#### `wifi`
Display current WiFi status.

```
xprs> wifi
WiFi: Connected
IP: 192.168.1.50
```

#### `wifi_connect <ssid> [password]`
Connect to a WiFi network. Credentials are automatically saved to NVS for reconnection on reboot.

```
xprs> wifi_connect MyNetwork secret123
Connecting to MyNetwork...
```

For open networks, omit the password:

```
xprs> wifi_connect OpenNetwork
Connecting to OpenNetwork...
```

#### `wifi_disconnect`
Disconnect from the current WiFi network.

```
xprs> wifi_disconnect
Disconnecting from WiFi...
Disconnected
```

#### `wifi_clear`
Clear saved WiFi credentials from NVS. The device will not auto-reconnect on next boot.

```
xprs> wifi_clear
WiFi credentials cleared
```

#### `wifi_saved`
Display saved WiFi credentials (password is masked).

```
xprs> wifi_saved
Saved SSID: MyNetwork
Password: ********
```

### Display Commands

#### `display`
Show current display status.

```
xprs> display
Display rotation: 0 degrees
```

#### `display_rotate [angle]`
Rotate the display. Valid angles: `0`, `90`, `180`, `270`. Omit angle to cycle through rotations.

```
xprs> display_rotate 90
Display rotated to 90 degrees

xprs> display_rotate
Display rotated to 180 degrees
```

#### `display_refresh [-f]`
Trigger a display refresh. Use `-f` for a full refresh (clears ghosting on e-paper).

```
xprs> display_refresh
Performing partial display refresh...
Display refreshed

xprs> display_refresh -f
Performing full display refresh...
Display refreshed
```

### Configuration Commands

#### `config`
Display all configuration settings.

```
xprs> config

=== Configuration ===

Callsign: ESPAB12
Firmware: 1.0.0
Board: ESP32S3-ePaper-1.54
Display rotation: 0 degrees

WiFi SSID: MyNetwork
WiFi Password: ********
```

#### `config_reset`
Reset all configuration to factory defaults. Clears WiFi credentials, display settings, and application settings.

```
xprs> config_reset
Resetting all configuration...
Configuration reset. Reboot to apply changes.
```

### NVS (Non-Volatile Storage) Commands

Low-level commands for inspecting and modifying NVS storage.

#### `nvs_list`
List known NVS namespaces.

```
xprs> nvs_list
Known NVS namespaces:
  wifi_config - WiFi credentials
  display     - Display settings
  XPRS     - Application settings
```

#### `nvs_get <namespace> <key>`
Read a value from NVS.

```
xprs> nvs_get wifi_config ssid
wifi_config/ssid = "MyNetwork" (string)

xprs> nvs_get display rotation
display/rotation = 90 (i32)
```

#### `nvs_set <namespace> <key> <value> [-t <type>]`
Write a value to NVS. Default type is `str` (string).

Types: `str`, `i32`, `u32`

```
xprs> nvs_set XPRS device_name "My XPRS"
Set XPRS/device_name = My XPRS

xprs> nvs_set display rotation 180 -t i32
Set display/rotation = 180
```

#### `nvs_erase <namespace> [key]`
Erase a key or entire namespace from NVS.

```
xprs> nvs_erase XPRS device_name
Erased key 'device_name' from namespace 'XPRS'

xprs> nvs_erase wifi_config
Erased all keys from namespace 'wifi_config'
```

## JSON Output Mode

When `format json` is enabled, commands output machine-parseable JSON:

```
xprs> format json
Output format set to JSON

xprs> status
{"version":"1.0.0","callsign":"ESPAB12","uptime":5025,"wifi":"connected","ip":"192.168.1.51"}

xprs> wifi
{"status":"connected","ip":"192.168.1.50"}

xprs> heap
{"free":245632,"minimum":198456}

xprs> uptime
{"uptime":8130}

xprs> display
{"rotation":90}

xprs> config
{"callsign":"ESPAB12","version":"1.0.0","board":"ESP32S3-ePaper-1.54","display_rotation":90,"wifi_ssid":"MyNetwork"}
```

## Automation Example

Python script to interact with the console:

```python
import serial
import json

ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)

def send_command(cmd):
    ser.write(f"{cmd}\n".encode())
    response = ser.readline().decode().strip()
    return response

# Switch to JSON mode
send_command("format json")

# Get status
status = json.loads(send_command("status"))
print(f"Device: {status['callsign']}, Uptime: {status['uptime']}s")

# Connect to WiFi
send_command("wifi_connect MyNetwork secret123")
```
