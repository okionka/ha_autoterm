# ha_autoterm – ESPHome Diesel Heater Climate Component

An ESPHome **external component** that integrates Autoterm Air 2D / Planar 2D
diesel heaters with Home Assistant as a full **Climate** entity.

---

## Features

| Feature | Detail |
|---|---|
| Climate modes | `OFF`, `HEAT` |
| Fan modes | `ON` (ventilator active) / `OFF` |
| Presets | `By T Heater` · `By T Panel` · `By T Air` · `By Power` |
| Target temperature | 0–30 °C in 1 °C steps |
| Current temperature | Any ESPHome sensor – including HA entities |
| Diagnostic sensors | Battery voltage, intake temp, output temp, panel temp, fan Hz/RPM, fuel pump, error code, status code, firmware version |
| Status text sensors | Human-readable heater state + error description |
| Status report button | Formats full snapshot into a HA text sensor on demand |
| Run timer | Work-time in minutes (0 = unlimited) + remaining-time sensor |
| Power level | Settable 1–10, mainly relevant for the `By Power` preset |
| TCP diagnostic proxy | Official Autoterm Test software over WiFi instead of a USB-TTL adapter |
| Operating modes | **Bridge** (physical controller panel present) or **Virtual panel** (ESP32 standalone) |

---

## Operating modes

### Bridge mode – physical controller panel present

```
Physical Panel ──UART1.RX──► ControllerPanelComponent
                                  │ loop(): forward byte-by-byte (unchanged)
                                  │ log: "controller → heater: AA:03:..."
                              UART2.TX ──► Physical Heater

Physical Heater ──UART2.RX──► Autoterm2DClimate
                                  │ loop(): forward + parse
                                  │ log raw: "heater → controller: AA:04:..."
                                  │ log interpreted: "STATUS 0.1 (Standby) ..."
                              UART1.TX ──► Physical Panel
                              UART2.TX ──► Physical Heater  (HA commands)
```

- The physical panel drives the poll cycle (0x0F / 0x11 / 0x02 requests)
- All bytes are forwarded transparently – **no modification, no injection** by ControllerPanelComponent
- HA climate commands are injected on UART2 TX by Autoterm2DClimate

### Virtual-panel mode – ESP32 standalone (no physical panel)

```
Physical Heater ──UART2.RX──► Autoterm2DClimate
                                  │ loop(): parse + log
                                  │ virtual poll every 2 s:
                                  │   0x0F → 0x11 (room temp) → 0x02
                              UART2.TX ──► Physical Heater
```

- ESP32 drives the poll cycle itself every 2 seconds
- 0x11 panel temperature uses `current_temperature` (room sensor) or `target_temperature` as fallback
- HA commands injected identically to bridge mode
- Only UART2 required – no UART1 needed

---

## Repository layout

```
ha_autoterm/
├── autoterm2d.yaml           # Main ESPHome config (bridge mode example)
├── secrets.yaml.example      # Template – copy to secrets.yaml and fill in
├── components/
│   └── autoterm2d/
│       ├── __init__.py       # Component namespace
│       ├── climate.py        # ESPHome codegen – registers both components
│       ├── autoterm2d.h      # Autoterm2DClimate (UART2, heater side)
│       ├── panel.h           # ControllerPanelComponent (UART1, panel side)
│       └── diag_proxy.h      # DiagProxyMixin – TCP proxy for Autoterm Test
├── docs/                     # Additional guides (German)
│   ├── DOCUMENTATION.md
│   ├── PATCH_ANLEITUNG.md
│   ├── USB_Proxy_Anleitung.md
│   └── YAML_ERGAENZUNGEN.yaml
└── tools/
    └── tcp_to_com.py         # TCP ↔ virtual COM bridge for the host PC
```

---

## Hardware

The ESP32 sits between the Autoterm Comfort Control panel and the Autoterm Air 2D
heater, and is powered from the heater's own 12 V supply.

### Block diagram

```
        Heater harness (4-wire)                    Panel harness (4-wire)
     ┌───────────────────────────┐              ┌──────────────────────────┐
     │ +12V  GND  TX(5V) RX(5V)  │              │ +12V  GND  TX(5V) RX(5V) │
     └───┬────┬─────┬──────┬─────┘              └───┬────┬─────┬──────┬────┘
         │    │     │      │                        │    │     │      │
         │    │     │      │   ┌────────────────────┘    │     │      │
         │    │     │      │   │  (+12V/GND passed through to the panel)
         │    │     │      │   │  ┌──────────────────────┘     │      │
         │    │     │      │   │  │                            │      │
   ┌─────┴────┴──┐  │      │   │  │                            │      │
   │  DC-DC buck │  │      │   │  │                            │      │
   │  12V → 5V   │  │      │   │  │                            │      │
   │  + C_in     │  │      │   │  │                            │      │
   └──────┬───┬──┘  │      │   │  │                            │      │
      5V  │   │ GND │      │   │  │                            │      │
          │   │     │      │   │  │                            │      │
    ┌─────┴───┴─────┴──────┴───┴──┴────────────────────────────┴──────┴────┐
    │              5 V side  (HV)   –  level shifter                       │
    │   HV = 5 V   HV1 ← heater TX   HV2 → heater RX                       │
    │               HV3 ← panel  TX   HV4 → panel  RX                      │
    ├──────────────────────────────────────────────────────────────────────┤
    │              3.3 V side (LV)                                         │
    │   LV = 3V3   LV1 → GPIO16      LV2 ← GPIO17                          │
    │               LV3 → GPIO3       LV4 ← GPIO1                          │
    └──────┬──────────────┬───────────────────────────────────────────────-┘
           │              │
      ┌────┴──────────────┴────────────────────────────────┐
      │  ESP32-WROOM-32 DevKit                             │
      │  5V ← buck output      3V3 → level shifter LV      │
      │  GPIO16/17 = UART2 (heater)                        │
      │  GPIO3/1   = UART0/UART1 (panel)  ⚠ see note       │
      │  Micro-USB → firmware flashing / serial console     │
      └────────────────────────────────────────────────────┘
```

### Bill of materials

| # | Part | Purpose |
|---|---|---|
| 1 | ESP32-WROOM-32 DevKit | Controller, mounted on female headers so it stays swappable |
| 2 | DC-DC buck converter 12 V → 5 V (≥ 500 mA) | Powers the ESP32 from the heater's 12 V line |
| 3 | Electrolytic capacitor ≈ 470 µF / 25 V at the buck input | Buffers the 12 V rail against starter/glow-plug dips |
| 4 | 4-channel bidirectional level shifter (BSS138 / TXS0108E) | Converts both UARTs between 5 V bus and 3.3 V ESP32 |
| 5 | 3 × 2-pin screw terminal | 12 V input, heater harness, panel harness |
| 6 | 2 × lever connector (WAGO 221 or similar) | 5 V and GND distribution rails |
| 7 | Perfboard + enclosure | Mechanical assembly |

### 1 — Power supply (12 V → 5 V)

The heater's supply line feeds the buck converter; its 5 V output powers the
ESP32 through the `5V` pin (not `3V3`, the on-board regulator is bypassed).

| From | To | Note |
|---|---|---|
| Heater harness `+12V` | Screw terminal `+` → buck `IN+` | Capacitor across `IN+`/`IN−` |
| Heater harness `GND` | Screw terminal `−` → buck `IN−` | Common ground for everything |
| Buck `OUT+` (5 V) | Lever connector → ESP32 `5V`, level shifter `HV` | Set the converter to 5.0 V **before** connecting the ESP32 |
| Buck `OUT−` | Lever connector → ESP32 `GND`, level shifter `GND` | |

> Automotive 12 V can dip hard while the glow plug draws current. The input
> capacitor and a buck converter rated well above the actual load keep the ESP32
> from browning out during ignition.

### 2 — Heater and panel connectors

Both harnesses carry the same four wires. `+12V` and `GND` are simply passed
through so the panel keeps its original supply.

| Wire | Heater side | Panel side |
|---|---|---|
| `+12V` | from heater | passed through to panel |
| `GND` | from heater | passed through to panel |
| `TX` (5 V, heater/panel output) | → level shifter `HV1` | → level shifter `HV3` |
| `RX` (5 V, heater/panel input) | ← level shifter `HV2` | ← level shifter `HV4` |

In **virtual-panel mode** the panel harness is left unconnected; only the heater
side and channels HV1/HV2 are used.

### 3 — Level shifter (5 V ↔ 3.3 V)

The heater bus is **5 V TTL**; the ESP32 is **not** 5 V tolerant. Feed `HV` from
the buck's 5 V output and `LV` from the ESP32's `3V3` pin, and share `GND`.

| Channel | 5 V side (HV) | 3.3 V side (LV) | ESP32 | Direction |
|---|---|---|---|---|
| 1 | heater TX | LV1 | GPIO16 (UART2 RX) | heater → ESP32 |
| 2 | heater RX | LV2 | GPIO17 (UART2 TX) | ESP32 → heater |
| 3 | panel TX | LV3 | GPIO3 (UART RX) | panel → ESP32 |
| 4 | panel RX | LV4 | GPIO1 (UART TX) | ESP32 → panel |

### 4 — ESP32 and firmware updates

The micro-USB port is used for the initial flash and for a serial console.

> **⚠️ GPIO1 / GPIO3 are the USB serial pins.** The panel harness shares them
> with the USB-to-UART bridge, so:
>
> - `logger:` must keep `baud_rate: 0`, otherwise ESPHome's log output is
>   injected into the panel bus and corrupts the protocol.
> - **Disconnect the panel harness before flashing over USB**, otherwise the
>   panel and the USB bridge drive the same line against each other.
> - After the first flash, use **OTA** — then the panel can stay connected.
>
> If you would rather keep USB permanently usable, move the panel UART to two
> free pins (for example GPIO25 / GPIO26) — the pins are exposed as substitutions:

```yaml
substitutions:
  controller_tx_pin: GPIO1     # panel  – also USB serial TX
  controller_rx_pin: GPIO3     # panel  – also USB serial RX
  heater_tx_pin:     GPIO17    # heater
  heater_rx_pin:     GPIO16    # heater
```

---

## Quick start

### 1. Add to your ESPHome config

```yaml
external_components:
  - source: github://okionka/ha_autoterm@main
    components: [autoterm2d]
```

ESPHome fetches `autoterm2d.h`, `panel.h`, and `climate.py` from GitHub on every compile.

### 2. Create your secrets file

```bash
cp secrets.yaml.example secrets.yaml
# Fill in wifi_ssid, wifi_password, api_encryption_key, ota_password, ap_password
```

### 3. Configure operating mode

**Bridge mode** (physical panel connected to UART1):

```yaml
uart:
  - id: uart_1_controller
    baud_rate: 9600
    tx_pin: GPIO1
    rx_pin: GPIO3

  - id: uart_2_heater
    baud_rate: 9600
    tx_pin: GPIO17
    rx_pin: GPIO16

climate:
  - platform: autoterm2d
    name: "Diesel Heater"
    uart_id: uart_2_heater         # heater UART
    panel_uart_id: uart_1_controller  # controller panel UART → bridge mode
    temperature_sensor: room_temperature
```

**Virtual-panel mode** (ESP32 directly on heater, no physical panel):

```yaml
uart:
  - id: uart_2_heater
    baud_rate: 9600
    tx_pin: GPIO17
    rx_pin: GPIO16

climate:
  - platform: autoterm2d
    name: "Diesel Heater"
    uart_id: uart_2_heater         # heater UART only
    # panel_uart_id omitted → virtual-panel mode
    temperature_sensor: room_temperature
```

### 4. Set your room temperature sensor

```yaml
sensor:
  - platform: homeassistant
    id: room_temperature
    entity_id: sensor.your_temperature_sensor   # ← change this
    unit_of_measurement: "°C"
    filters:
      - filter_out: nan
```

Any ESPHome sensor works: `platform: homeassistant`, `platform: dht`, `platform: dallas`, etc.

### 5. Flash

```bash
esphome run autoterm2d.yaml
```

Or use the **ESPHome Dashboard** in Home Assistant → Install → Wirelessly.

---

## Climate entity in Home Assistant

| HA control | Effect |
|---|---|
| Mode `OFF` | Sends shutdown command (0x03) |
| Mode `HEAT` | Starts heater with current settings (0x01) |
| Target temperature | Sets temperature setpoint 0–30 °C |
| Fan `ON` / `OFF` | Controls ventilator (start/settings command) |
| Preset | Selects regulation strategy (see below) |

### Presets

| Preset | Protocol mode | Description |
|---|---|---|
| `By T Heater` | 1 | Heater uses its own board temperature sensor |
| `By T Panel` | 2 | Heater uses temperature from 0x11 messages (controller panel or virtual panel) |
| `By T Air` | 3 | Heater uses its external air temperature sensor. If `air_temperature_source` is configured, the ESP32 sends 0x11 with that HA sensor value. Only while the heater is running; changes are limited to 2 °C per update and NaN / out-of-range values are dropped |
| `By Power` | 4 | Fixed power level, no thermostat control |

### Diagnostic sensors & entities

| Entity | Type | Description |
|---|---|---|
| Heater Status | Text sensor | Human-readable state string |
| Heater Status Code | Sensor | Numeric `major * 256 + minor` |
| Heater Error | Text sensor | Error description ("OK", "Flame blowout", …) |
| Heater Error Code | Sensor | Numeric error code 0–37 |
| Heater Firmware Version | Text sensor | e.g. `2.1.3.4` (from 0x06 response) |
| Heater Status Report | Text sensor | Updated by Status Report button |
| Status Report | Button | Formats full snapshot into Heater Status Report |
| Heater Board Temperature | Sensor | Intake air temperature (°C) |
| Heater Battery Voltage | Sensor | Supply voltage (V) |
| Heater Air Temperature | Sensor | Output / external sensor temp (NaN if disconnected) |
| Panel Temperature | Sensor | Controller panel ambient temp (from 0x11) |
| Heater Power Level (reported) | Sensor | Power level reported by the heater, 1–10 |
| Ventilator Power | Sensor | Fan actual frequency (Hz) |
| Heater Remaining Time | Sensor | Remaining work time in minutes (0 = unlimited) |
| Heater Timer | Number | Run time 0–255 min; 0 = unlimited |
| Heater Power Level | Number | Sets power level 1–10 (used by the `By Power` preset) |

---

## Run timer

The heater's own work-time function is exposed as a number entity:

| Value | Behaviour |
|---|---|
| `0` | Unlimited – runs until stopped (payload bytes `0xFF 0xFF`) |
| `1`–`255` | Shuts down automatically after N minutes (payload `0x00 N`) |

The timer value is applied with the next start/settings command. **Heater Remaining
Time** mirrors the countdown reported by the heater in its `0x02` echo.

```yaml
number:
  - platform: template
    name: "Heater Timer"
    min_value: 0
    max_value: 255
    step: 1
    initial_value: 0
    optimistic: true
    mode: box
    set_action:
      lambda: id(diesel_heater)->set_work_time((uint8_t)x);
```

---

## TCP diagnostic proxy

`DiagProxyMixin` exposes the heater UART as a raw TCP socket so the official
**Autoterm Test** software can talk to the heater over WiFi instead of a
USB-TTL adapter.

| Setting | Value |
|---|---|
| Port | `diagnostic_port:` in the climate config (default `8888`) |
| Framing | None – raw byte stream, identical to a physical COM port |
| Serial settings in the software | 9600 8N1 |
| Concurrent clients | Exactly one |
| Authentication | None – protect via VLAN / firewall |

While a client is connected, the virtual-panel poll cycle pauses so the
diagnostic software owns the bus. In bridge mode the physical panel keeps
polling and the client listens in and may inject its own commands.

On the host PC, `tools/tcp_to_com.py` bridges the TCP socket to a virtual COM
port (com0com on Windows, PTY on Linux/macOS) — see `docs/USB_Proxy_Anleitung.md`.

The implementation uses lwip BSD sockets, so it builds under both the Arduino
and the ESP-IDF framework (ESPHome 2026.7+ uses ESP-IDF for ESP32 targets).

---

## Logger configuration

The component uses three log tags:

| Tag | Content | Recommended level |
|---|---|---|
| `autoterm2d` | Interpreted protocol (STATUS, SETTINGS, TX commands) | `DEBUG` |
| `heater` | Raw frame hex from heater (AA:04:…) | `DEBUG` |
| `controller` | Raw frame hex from panel (AA:03:…) – bridge mode only | `DEBUG` |
| `diag_proxy` | TCP client connect / disconnect events | `INFO` |

Recommended logger config:

```yaml
logger:
  level: DEBUG
  logs:
    api: INFO
    wifi: INFO
    ota: INFO
    sensor: INFO
    text_sensor: INFO
    climate: INFO
```

---

## Serial Protocol

> References:
> [helloworld.schlussdienst.net](https://helloworld.schlussdienst.net/blog/hacking-autoterm-planar-2d) ·
> [kalutep/AutotermHeaterController](https://github.com/kalutep/AutotermHeaterController/blob/main/serial_communication_protocol.md) ·
> [schroeder-robert/autoterm-air-2d-serial-control](https://github.com/schroeder-robert/autoterm-air-2d-serial-control)

### Physical layer

| Parameter | Value |
|---|---|
| Interface | UART |
| Baud rate | 9600 (heater auto-negotiates 1200 / 2400 / 9600 at startup) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Voltage | **5 V TTL** |

All communication is initiated by the controller panel. The heater only responds.

---

### Frame structure

```
Byte 0    Byte 1    Byte 2        Byte 3   Byte 4      Byte 5 … N   N+1   N+2
──────────────────────────────────────────────────────────────────────────────
0xAA      Sender    Payload len   0x00     Command ID  Payload      CRC16 (MSB first)
```

| Field | Values |
|---|---|
| Preamble | Always `0xAA` |
| Sender | `0x03` = Controller panel · `0x04` = Heater |
| Payload length | Number of bytes in payload field (0 for poll requests) |
| Reserved | Always `0x00` |
| Command ID | See table below |
| CRC-16 | CRC-16/MODBUS (poly `0x8005`, reflected `0xA001`, init `0xFFFF`), **MSB first** |

> The CRC variant and byte order were verified against captured traffic from a
> physical controller panel — all frames match CRC-16/MODBUS with the high byte
> transmitted first.

---

### Command reference

| ID | Name | Direction | Payload len |
|---|---|---|---|
| `0x01` | Start heater | Controller → Heater | 6 |
| `0x02` | Settings request / echo | Controller ↔ Heater | 0 / 6 |
| `0x03` | Stop | Controller → Heater | 0 |
| `0x06` | Firmware version | Controller ↔ Heater | 0 / 5 |
| `0x0F` | Status | Controller ↔ Heater | 0 / 19 |
| `0x11` | Panel temperature | Controller ↔ Heater | 1 |

Poll cycle (normal operation): `0x0F` → `0x11` → `0x02` → repeat every ~2 s.

---

### `0x01` / `0x02` – Start / Settings payload (6 bytes)

| Byte | Field | Notes |
|---|---|---|
| 0 | Work-time flag | `0x00` = use work time from byte 1 · `0xFF` = unlimited |
| 1 | Work time | Minutes (valid when flag = `0x00`), `0xFF` when unlimited. Heater default: 120 min |
| 2 | Mode | `1` = By T Heater · `2` = By T Panel · `3` = By T Air · `4` = By Power |
| 3 | Target temperature | °C, range 1–30 (the component clamps HA values into this range) |
| 4 | Ventilation | Controller→Heater: `1` = On · `2` = Off ·  Heater→Controller: `0` = Off · `1` = On |
| 5 | Power level | 0–9 on the wire, shown as 1–10 in HA (used in By Power mode; updated by the heater's PID in other modes) |

The heater sends its `0x02` echo continuously, including while in standby, so
target temperature, preset, power level and remaining time stay in sync in HA
even when the heater is off.

`0x01` triggers ignition. `0x02` with empty payload reads settings; with payload updates them live.

---

### `0x03` – Stop

No payload. Initiates shutdown / purge sequence.

```
Controller → AA 03 00 00 03 5D 7C
```

---

### `0x0F` – Status response payload (19 bytes)

| Offset | Field | Type | Formula |
|---|---|---|---|
| 0 | Status major | `uint8` | See state table |
| 1 | Status minor | `uint8` | See state table |
| 2 | Error code | `uint8` | 0 = OK, see error table |
| 3 | Temp 1 – intake air | `int8` | °C · `0x7F` = disconnected |
| 4 | Temp 2 – output / ext | `int8` | °C · `0x7F` = disconnected |
| 5–6 | Supply voltage | `uint16` BE | `÷ 10` → V |
| 7–8 | Flame / heat exchanger temp | `uint16` BE | Kelvin − 273.15 → °C |
| 11 | Fan setpoint | `uint8` | Hz · `× 60` → RPM |
| 12 | Fan actual | `uint8` | Hz · `× 60` → RPM |
| 14 | Fuel pump frequency | `uint8` | `÷ 100` → Hz (typical 0.5–2 Hz) |

#### Status codes (Major.Minor)

| Major | Minor | State |
|---|---|---|
| 0 | 0 | Sleep / Off |
| 0 | 1 | Standby |
| 1 | 0 | Purge: cooling flame sensor |
| 1 | 1 | Purge: ventilating combustion chamber |
| 2 | 1 | Pre-heat (glow plug on) |
| 2 | 2 | Ignition seq 1 |
| 2 | 3 | Ignition seq 2 |
| 2 | 4 | Ramp up |
| 3 | 0 | **Heating – PID active** |
| 3 | 35 | Ventilation only (fan mode) |
| 3 | 4 | Cooling down |
| 4 | 0 | Shutdown complete |

#### Error codes

| Code | Description |
|---|---|
| 0 | No error |
| 1 | Overheat |
| 2 | Potential overheat |
| 5 | Flame sensor fault |
| 6 | Temperature sensor fault |
| 9 | Glow plug fault |
| 10 | Motor / fan RPM fault |
| 11 | Air temperature sensor fault |
| 12 | Over voltage (> 16 V on 12 V system) |
| 13 | No start – ignition failed |
| 15 | Under voltage (< 10 V on 12 V system) |
| 17 | Fuel pump fault |
| 20 | No communication |
| 29 | Flame blowout during operation |
| 33 | Control lockout (3× overheat) |
| 37 | **Hard lockout** – send unlock command `0x0D` to reset |

---

### `0x06` – Firmware version response (5 bytes)

| Byte | Field |
|---|---|
| 0 | Major |
| 1 | Minor |
| 2 | Patch |
| 3 | Build |
| 4 | Bootloader version |

Requested once by the ESP32 after receiving the first valid status message.

---

### `0x11` – Panel temperature (1 byte)

| Byte | Field | Type |
|---|---|---|
| 0 | Panel temperature | `int8` °C |

The controller reports its ambient temperature once per poll cycle. The heater
echoes the same value. Used by the heater in **By T Panel** mode.

In virtual-panel mode the ESP32 sends the configured `temperature_sensor` value.
In **By T Air** mode with `air_temperature_source` configured, the ESP32 sends
that sensor's value (smoothed to max 2 °C change per update).

---

### Annotated log example (heater idle, bridge mode)

```
# ControllerPanelComponent forwards panel bytes:
[D][controller]: → heater: AA:03:00:00:02         ← settings request (no payload)

# Autoterm2DClimate receives heater response:
[D][heater]:     → controller: AA:04:06:00:02:00:78:02:0F:00:05:39:3D
[D][autoterm2d]: SETTINGS    mode=By T Panel  target=15°C  vent=Off  level=6/10  time=120min

[D][controller]: → heater: AA:03:00:00:0F         ← status poll
[D][heater]:     → controller: AA:04:13:00:0F:00:01:00:14:7F:00:91:01:25:...
[D][autoterm2d]: STATUS 0.1 (Standby) Err:0(OK) T-in:20°C T-out:n/a 14.5V Flame:20°C Fan:0/0Hz Pump:0.00Hz

[D][controller]: → heater: AA:03:01:00:11:0D      ← panel temp 13 °C
[D][heater]:     → controller: AA:04:01:00:11:0D:B8:25
[D][autoterm2d]: PANEL TEMP  13°C
```

### Annotated log example (virtual-panel mode)

```
[I][autoterm2d]: Starting in VIRTUAL-PANEL mode (ESP32 drives poll cycle)

[D][autoterm2d]: VPANEL 0x0F status request
[D][heater]:     → virtual-panel: AA:04:13:00:0F:...
[D][autoterm2d]: STATUS 0.1 (Standby) ...

[D][autoterm2d]: TX 0x11 12°C (virtual panel)
[D][heater]:     → virtual-panel: AA:04:01:00:11:0C:...

[D][autoterm2d]: VPANEL 0x02 settings request
[D][heater]:     → virtual-panel: AA:04:06:00:02:...
[D][autoterm2d]: SETTINGS    mode=By T Heater  target=15°C  vent=On  level=5/10  time=120min
```

---

## License

MIT
