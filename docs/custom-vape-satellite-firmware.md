# Custom VAPE Satellite Firmware

This firmware turns Home Assistant Voice Preview Edition into a thin voice satellite for a Linux OpenAI Realtime backend.

The VAPE device is responsible for:

- local `Hey Jarvis` wake-word detection
- center button activation
- microphone capture after activation
- speaker playback for backend audio
- local wake and idle cue sounds
- LED state feedback

The Linux backend is responsible for OpenAI Realtime, Home Assistant tool calls, web search, session lifecycle, barge-in, and streamed response audio.

## Runtime Protocol

The firmware connects to the backend WebSocket endpoint configured by:

```yaml
substitutions:
  vape_satellite_url: "ws://<linux-backend-ip>:8765/vape"
```

The current transport is:

- JSON control frames over WebSocket
- binary `pcm_s16le` microphone audio from VAPE to Linux
- binary `pcm_s16le` playback audio from Linux to VAPE
- 16 kHz mono microphone input
- 48 kHz mono playback output

The backend sends `set_state` controls for `idle`, `listening`, `thinking`, and `speaking`. Firmware maps those states to the existing Voice PE LED scripts.

## Local Cue Sounds

The firmware plays local cue sounds for states that need immediate room feedback:

- wake word or center button activation: `wake_word_triggered_sound`
- return to idle: `mute_switch_on_sound`

Thinking/tool/error cue sounds are streamed from the Linux backend because the backend owns tool execution and OpenAI error classification.

## Per-Device Compile Overlay

Do not edit Wi-Fi credentials directly into committed files. Use an ignored file under `config/`, for example `config/vape-satellite-compile.yaml`:

```yaml
packages:
  base: !include ../home-assistant-voice.yaml

substitutions:
  vape_external_components_path: "../esphome/components"
  vape_satellite_url: "ws://192.168.1.50:8765/vape"

wifi:
  ssid: "YOUR_WIFI_SSID"
  password: "YOUR_WIFI_PASSWORD"
```

The repo ignores `config/`, so this file can contain local machine and network settings without being committed.

For multiple VAPE boxes, create one overlay per target if they need different backend URLs or Wi-Fi settings, for example:

```text
config/kitchen-vape.yaml
config/office-vape.yaml
```

`name_add_mac_suffix: true` is enabled, so multiple flashed VAPE boxes can coexist without sharing the same ESPHome node name.

## Compile

From the firmware repository:

```sh
python3 -m venv .venv
.venv/bin/pip install -U pip
.venv/bin/pip install -e .
.venv/bin/esphome compile config/vape-satellite-compile.yaml
```

If the virtualenv already exists, only the compile command is needed.

## Flash Over USB-C

Plug the VAPE into the computer over USB-C and find the serial device:

```sh
ls -l /dev/ttyACM* /dev/ttyUSB*
fuser -v /dev/ttyACM0 2>&1 || true
```

Upload:

```sh
.venv/bin/esphome upload config/vape-satellite-compile.yaml --device /dev/ttyACM0
```

After upload, the ESP32 resets. A short serial read should show Wi-Fi connection and micro wake word detection:

```sh
.venv/bin/python - <<'PY'
import serial, time

with serial.Serial("/dev/ttyACM0", 115200, timeout=0.5) as ser:
    end = time.time() + 20
    while time.time() < end:
        line = ser.readline()
        if line:
            print(line.decode("utf-8", errors="replace").rstrip())
PY
```

Look for:

- Wi-Fi connected
- IP address assigned
- `micro_wake_word` state `DETECTING_WAKE_WORD`
- `Detected 'Hey Jarvis'` when spoken

## Backend Requirement

The Linux backend must be reachable from the VAPE LAN address at the configured URL. The default backend listener is:

```text
ws://0.0.0.0:8765/vape
```

If the backend host changes, update `vape_satellite_url` in the overlay and flash the device again.
