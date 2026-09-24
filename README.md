# Bambu LED Status Bar

Standalone ESP32 firmware that drives a WS2811 LED strip as a progress/status bar for a
Bambu Lab printer, talking **MQTT directly to the printer** over LAN Mode — no Home
Assistant, no WLED, no cloud. Successor to
[Bambu Lab P2S LED Progress/Status Bar](https://makerworld.com/en/models/2172105-bambu-lab-p2s-led-progress-status-bar),
which used an ESP32 + WLED + Home Assistant automation to do the same job.

## Features

- Connects straight to the printer's LAN Mode MQTT broker (TLS, port 8883) — no other
  software required, not even Bambu Studio/Handy.
- Web-based setup wizard (WiFi + printer discovery) — no hardcoded credentials, no
  recompiling to configure a new printer.
- 7 printer states mapped to distinct LED animations (idle, heating, calibrating,
  printing, paused, finished, error), refined using the printer's sub-stage field so
  homing/leveling/filament-change show up as "calibrating" instead of a vague "heating".
- Colors and one global brightness are editable live from the web UI — no recompiling.
- Dims automatically when the printer's own chamber light is off, and shows a distinct
  "not connected" indicator when it loses contact with the printer.
- A "Finished" print returns to idle on its own after a fixed window, instead of staying
  green forever waiting for the printer to change its own state.
- Built-in diagnostics log viewable from the web UI — no USB cable or PlatformIO needed
  to see what the firmware is doing.
- Flashes over USB with PlatformIO; no cloud account, no app, no subscription.

## Hardware

- Any ESP32-WROOM-32 DevKit (no PSRAM required).
- A WS2811 (or WS2812/compatible, see `LED_COLOR_ORDER`) strip — 10 segments by default,
  one per 10% of print progress.

### Wiring

| Strip | ESP32 |
|---|---|
| **DIN** (data) | GPIO16 |
| **GND** | Any GND pin (all GND pins are the same net) |
| **+5V** | ESP32's 5V/VIN pin, **or** a separate 5V supply — see below |

- Put a ~300-500Ω resistor in series on the data line, right before the strip's first
  LED, if you see flicker or wrong colors on the first pixel.
- If colors come out swapped, change `LED_COLOR_ORDER` in
  [AppConfig.h](include/AppConfig.h) from `RGB` to `GRB`.
- **Power**: measured peak draw for 10 LEDs at full brightness is ~270mA, comfortably
  within a standard USB port's budget — powering everything from the ESP32's own USB is
  fine. If you use a separate 5V supply instead (e.g. a longer strip that draws more),
  its GND **must** still be tied to the ESP32's GND — never power the strip from a
  separate supply without a shared ground reference.

## Quick start

1. Install [VS Code](https://code.visualstudio.com/) + the **PlatformIO IDE** extension.
2. Open this folder in VS Code — PlatformIO reads `platformio.ini` and fetches the
   dependencies automatically.
3. Connect the ESP32 over USB and run:

```bash
pio run -t upload
```

4. To watch the logs:

```bash
pio device monitor
```

If the serial port isn't auto-detected, check which `COMx` (Windows) or `/dev/ttyUSBx`
(Linux/macOS) it enumerated as and pass `--upload-port` / `--monitor-port` explicitly, or
add `upload_port` / `monitor_port` to `platformio.ini`.

## First boot — configuring the device

1. With no WiFi saved yet, the ESP32 boots straight into **setup mode**: it creates its
   own WiFi network, `BambuLED-XXXX` (open, no password).
2. Connect to that network from your phone or laptop. The captive portal should open on
   its own; if not, browse to `http://192.168.4.1`.
3. **Step 1 (WiFi)**: tap **Refresh** to list nearby networks, pick yours and enter the
   password (or type the SSID manually for a hidden network), then **Connect**. The
   ESP32 joins your network in the background without dropping the setup AP.
4. **Step 2 (Printer)**: once connected, it starts listening for the printer's SSDP
   broadcast automatically. Pick yours from the list (fills in IP/serial), or use
   **Enter printer info manually** if it's on a different subnet/VLAN the broadcast
   can't reach. The **Access Code** always has to be typed in — the printer never
   broadcasts it for security reasons.
5. The device saves everything and reboots onto your network, dropping the setup AP. If
   anything goes wrong, `BambuLED-XXXX` reappears so you can try again.
6. Once connected, visit `http://bambuled.local` (or the IP from the serial log) for
   live status, or `/settings` for animation colors/brightness and diagnostics.

### Finding the printer's IP / Serial / Access Code (LAN Mode)

On the printer's screen: **Settings → WLAN → LAN Only Mode** (enable it). Then tap the
gear/info icon next to it to see the **Access Code** and **Serial**. The IP is on the
printer's own network screen (or your router's client list).

## Web UI

- **`http://bambuled.local`** — live status: network info, MQTT connection state, and a
  10-segment preview that mirrors exactly what the physical strip shows.
- **`http://bambuled.local/settings`** — pick 1-2 colors per state (the animation type
  itself is fixed, hand-picked to fit each state), one brightness slider for the whole
  strip, and a **Diagnostics** panel with the last ~30 firmware log lines — handy for
  troubleshooting without a USB cable or PlatformIO installed.

## How state detection works

The printer's `gcode_state` (RUNNING/PAUSE/FINISH/FAILED/PREPARE/IDLE) maps to a
simplified state, refined with two extra signals:

- **`stg_cur`** (sub-stage) splits the vague "getting ready" bucket into `heating`
  (actually heating the nozzle/bed) vs. `calibrating` (homing, bed leveling, filament
  load/unload) — including mid-print, since some jobs fold homing into the RUNNING phase
  rather than a separate PREPARE phase. See `mapStage()` in
  [PrinterMqtt.cpp](src/PrinterMqtt.cpp) for the full stage-ID table (sourced from the
  community-maintained [greghesp/ha-bambulab](https://github.com/greghesp/ha-bambulab)
  integration, since Bambu doesn't publish it anywhere).
- **`print_error`** catches errors that happen outside an active print job (e.g. a door
  sensor tripping while idle) that `gcode_state` alone would miss. It's ignored while
  `gcode_state == "IDLE"`, because on real hardware `print_error` stays non-zero (stale,
  from the last failed job) even after the printer genuinely goes back to idle — only a
  printer reboot clears it, and trusting it unconditionally kept the LED stuck red
  forever after any cancelled print.

## LED animations

Colors are ported from the original WLED `presets.json` this project replaces; the
effect *type* per state is fixed (not user-editable) and lives in
[LedAnimations.cpp](src/LedAnimations.cpp):

| State | Effect | Colors | Notes |
|---|---|---|---|
| idle | Plasmoid (WLED `Plasma`) | 2 | ambient color wave |
| heating | Loading | 1 | traveling pixel with a fading trail |
| calibrating | Bounce | 1 | physics-simulated bouncing ball — replaces WLED's audio-reactive `Gravfreq` effect, since there's no microphone here |
| printing | Percent | 2 (bar + leading pixel) | one segment per 10% progress; the in-progress segment pulses instead of sitting static |
| paused | Fade | 1 | breathing |
| finished | Fade | 1 (green) | breathing, same as paused; auto-reverts to idle after 60s (see below) |
| error | Fade | 1 | breathing, fast |
| *(disconnected)* | — | fixed | only the first pixel, fading blue↔red — not a configurable state |

A few behaviors worth knowing about:
- **Finished doesn't wait on the printer.** On real hardware, the printer doesn't
  reliably flip `gcode_state` back to `IDLE` once you remove the finished part — it may
  need an explicit ack on its own screen. Rather than stay green forever, the firmware
  just stops *showing* Finished after `FINISHED_DISPLAY_MS` (60s, in
  [LedAnimations.cpp](src/LedAnimations.cpp)) and reverts to idle regardless.
- **Chamber light off → whole strip dims to ~10%**, regardless of state — a reasonable
  signal nobody's actively watching the printer.
- **Long idle → dims further.** After `LED_IDLE_TO_SLEEP_MS` (30 min by default, in
  [AppConfig.h](include/AppConfig.h)) of uninterrupted idle, brightness drops to 1/5th.
- **Lost MQTT connection** shows a dedicated indicator (first pixel fading blue↔red, rest
  off) instead of quietly reusing another state's look.

## Project structure

```
BambuEsp32MQTT/
├── platformio.ini          # board, framework and dependencies
├── include/
│   ├── AppConfig.h          # pins, timeouts, AP/mDNS name, strip config — tune here
│   ├── Storage.h            # NVS-backed config read/write contract
│   ├── PrinterDiscovery.h   # SSDP printer discovery contract
│   ├── PrinterMqtt.h        # MQTT client contract
│   ├── PrinterStatus.h      # derived printer state struct/enum
│   ├── LedAnimations.h      # per-state FastLED animation contract
│   ├── AnimConfig.h         # animation config schema + NVS persistence
│   ├── Log.h                # small in-RAM ring buffer behind the Diagnostics panel
│   └── WebPages.h           # HTML/CSS/JS for the setup wizard, status and settings pages
└── src/
    ├── main.cpp              # AP (setup) vs. station mode, HTTP API, orchestration
    ├── Storage.cpp
    ├── PrinterDiscovery.cpp
    ├── PrinterMqtt.cpp       # MQTTS client, JSON parsing, state derivation
    ├── PrinterStatus.cpp
    ├── LedAnimations.cpp     # FastLED effects (Fade/Loading/Percent/Plasmoid/Bounce)
    ├── AnimConfig.cpp
    └── Log.cpp
```

## Reset / reconfiguration

The status page (`http://bambuled.local`) has two buttons:
- **Reconfigure WiFi** — erases only the WiFi credentials (keeps the printer config) and
  drops back into setup mode.
- **Factory reset** — erases everything (WiFi + printer) and starts fresh.

## Known limitations

**Occasional reboot from a TLS bug in the framework, not this project's code.** Every so
often (observed roughly every 1-3 minutes of active MQTT traffic, sometimes much less
often) the ESP32 can crash and reboot right after an `SSL - Verification of the message
MAC failed` / `invalid SSL record` error on the MQTT connection. This traces back to a
documented, unresolved bug in arduino-esp32's `WiFiClientSecure`/`NetworkClientSecure`
wrapper around mbedTLS (see
[espressif/arduino-esp32#8281](https://github.com/espressif/arduino-esp32/issues/8281),
[#9064](https://github.com/espressif/arduino-esp32/issues/9064)), not anything specific
to this firmware. It happens with every MQTT client library we tried (PubSubClient,
256dpi/MQTT, and ESP-IDF's own `esp_mqtt_client`), which is what points to a shared,
lower-level cause.

The recovery is automatic and clean — the board reboots, reconnects WiFi and MQTT on its
own within seconds, and nothing configured is lost (it's all in NVS). Because the strip
holds its last frame through a reboot (once it has its own power), the visible effect is
usually just a brief pause rather than anything dramatic. The same instability can
occasionally make the web UI slow to respond for a few seconds while it's happening.

Two different attempts at eliminating this at the root both hit real walls:
- Switching to the community [pioarduino](https://github.com/pioarduino/platform-espressif32)
  platform fork (newer ESP-IDF/mbedTLS) — this project already uses it (see
  `platformio.ini`), and it didn't remove the bug, only kept the crash frequency roughly
  the same as with the official platform.
- Bypassing `WiFiClientSecure` entirely with ESP-IDF's native `esp_mqtt_client` +
  `esp-tls` — the prebuilt SDK that ships with the Arduino framework doesn't have
  `CONFIG_ESP_TLS_INSECURE` compiled in, so `esp-tls` refuses to connect at all without a
  real CA certificate. Fixing that means switching to `framework = espidf, arduino` with
  a custom `sdkconfig` — a much larger, riskier build-system change, so we reverted.

If you want to chase this further, that framework switch is the next real lever; for
personal use, the automatic recovery is good enough that we shipped without it.

**The printer doesn't always clear its own error/failure state.** If a print is
cancelled or fails, the printer can keep reporting `gcode_state=FAILED` even after you
dismiss the error on its screen or home the toolhead — only a printer reboot reliably
clears it. We tried auto-sending the same "resume" command Bambu Studio/Handy send (which
does clear it, per [community reports](https://github.com/BambuTools/bambulabs_api/issues/183)) but it requires enabling **Developer Mode** on the printer (else every
third-party command is rejected with `HMS-0500-0500-0001-0007`), and even with that on,
it didn't clear the error reliably on our hardware — not worth the security trade-off of
leaving Developer Mode on for a feature that doesn't work consistently. If your printer
gets stuck showing red after a cancelled/failed print, reboot the printer.

## License

See [LICENSE](LICENSE) if present, otherwise this project has no explicit license yet —
ask before reusing commercially.
