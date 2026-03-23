# Chirp

Chirp is a Teensy 4.0 MIDI processing firmware with an on-device Wren runtime, USB control protocol, and flash-backed script/data storage.

## Hardware

- MCU board: Teensy 4.0
- MIDI breakout used in this project: https://www.tindie.com/products/deftaudio/teensy-40-midi-breakout-board-5in-5out-usb/

## Upstream Projects

- Wren language: https://wren.io/
- Wren source repo: https://github.com/wren-lang/wren
- Teensy platform: https://www.pjrc.com/teensy/
- Teensyduino / Arduino core package: https://www.pjrc.com/teensy/td_download.html

## Toolchain And Libraries Used

From the current build configuration/logs:

- Arduino FQBN: `teensy:avr:teensy40:usb=serialmidi`
- Teensy platform/core: `teensy:avr` version `1.60.0`
- Core variant: `teensy4`
- Libraries:
  - `MIDI Library` version `5.0.2`
  - `LittleFS` version `1.0.0`
  - `SPI` version `1.0`

## Repository Layout

- `chirp/chirp.ino`: Arduino sketch entrypoint
- `src/`: firmware components (MIDI router, USB protocol handlers, Wren bridge/host)
- `src/include/`: headers and protocol definitions
- `tools/chirp_fs.py`: host-side serial tool for managing files on device flash
- `scripts/*.wren`: user Wren scripts uploaded to `/scripts/user` on the device
- `midi_maps/*.json`: user data uploaded to `/userdata` on device flash
- `third_party/wren-json/`: JSON parser module for Wren

## Build And Flash

From repo root:

```bash
make
```

Upload firmware (press Teensy button when prompted):

```bash
make upload
```

Upload scripts + maps to on-device flash:

```bash
make upload-fs
```

Full flow (build + upload + filesystem sync):

```bash
make upload-all
```

## Python Host Tool Setup (`tools/chirp_fs.py`)

### 1) Install Python dependency

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install pyserial
```

If you do not want a venv, install system/user-wide:

```bash
pip3 install --user pyserial
```

### 2) Verify serial device access

Default serial port is `/dev/ttyACM0`. If needed, pass `-p`.

```bash
./tools/chirp_fs.py -p /dev/ttyACM0 ping
```

### 3) Common commands

```bash
./tools/chirp_fs.py -p /dev/ttyACM0 list
./tools/chirp_fs.py -p /dev/ttyACM0 sync --delete scripts/ midi_maps/ third_party/wren-json/
./tools/chirp_fs.py -p /dev/ttyACM0 reboot
./tools/chirp_fs.py -p /dev/ttyACM0 monitor
```

## Notes

- Device flash layout used by sync:
  - `/scripts/user`: user Wren scripts
  - `/scripts/builtin`: runtime/modules (e.g. `json.wren`)
  - `/userdata`: JSON maps and cached user data
- The D-Station script forwards translated CC to MIDI port `1`, MIDI channel `10`.
