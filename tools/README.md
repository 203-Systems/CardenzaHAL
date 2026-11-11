# Upload and register a Cardenza app

[简体中文](README.zh-CN.md)

Use these host tools to send an ESP32-S3 app BIN to an installed **203 Launcher**,
then register its name, UUID and version. Run commands from the CardenzaHAL root.
These tools do not build firmware or publish an app to the online store.

## Set up

Use Python 3.9 or newer on Windows, macOS or Linux:

```sh
python -m venv .venv
# macOS / Linux
source .venv/bin/activate
# Windows PowerShell: .venv\Scripts\Activate.ps1
python -m pip install -r tools/requirements.txt
python -m serial.tools.list_ports -v
```

Replace `python` with `python3` if needed. Replace `COM8` below with your port
(e.g. `/dev/cu.usbmodem...` on macOS or `/dev/ttyACM0` on Linux).
Close serial monitors and other programs using that port.

The device must already have a working Launcher with the `flash firmware`,
`apps list` and `apps register` commands. Build the app for **Cardenza**, using
its expected shared partition layout. Prefer the app-only `build/<project>.bin`;
bootloader, partition-table and merged factory images are not app-only BINs.
Checking the ESP32-S3 chip ID cannot establish board pin compatibility.

## 1. Check and upload the app

Validate the image before connecting (no dependencies required):

```sh
python tools/serial_flasher.py --check -f ../MyApp/build/my_app.bin
```

Return to the Launcher's home screen, then upload:

```sh
python tools/serial_flasher.py --no-reset -p COM8 -f ../MyApp/build/my_app.bin -n MyApp
```

The tool waits for `Launcher ...`, sends `flash firmware MyApp <size>`, checks
`READY <size>`, streams 2048-byte chunks with exact `ACK <sent>/<size>` replies,
and expects `OK flashed, rebooting`. Launcher chooses the destination using its
existing allocation policy and reboots into the new app. This command does not
select an arbitrary flash address. `-n` is an ASCII label hint (1–64 letters,
digits, dots, underscores or hyphens); it does not register the app's identity.

Without `--no-reset`, the tool first calls esptool to reset the board. Use this
only when the USB-Serial-JTAG port is available. When the app uses HAL composite
USB, return to Launcher through the app's UI or the HAL system command `0x40`
(HID report `0xCB`, or MIDI SysEx `F0 23 40 F7`), then list ports again. A reset
alone can resume the current app, so manually returning to Launcher is preferred.
The tools do not send HID/MIDI commands automatically.

Input checks validate segment bounds, XOR checksum, an appended SHA-256 when
present, and ESP32-S3 chip ID. A full-flash merged BIN starting at flash address
zero is supported when its table is at `0x8000` and contains exactly one valid
app. Only that app is sent. Ambiguous images fail; supply an app-only BIN.
Use unsigned development images; signed/encrypted distribution is not supported.
The device still performs its own image validation.

## 2. Return to Launcher and register the installed image

Upload reboots into the app. Return to Launcher before running these commands.
Registration never flashes or resets the device.

```sh
python tools/register_app.py -p COM8 --list
python -c "import uuid; print(uuid.uuid4())"
```

Create `app.json` in your app project, using the newly generated UUID:

```json
{
  "id": "3fbb1fe8-2dc4-40d6-955d-8a16d626ece3",
  "target": "cardenza",
  "name": "My App",
  "description": "Music and creative tools",
  "version": "0.1.0",
  "build": 1
}
```

Keep one UUID for the same application across builds; do not reuse the example
UUID for different apps. Read the exact `label` from `--list` and substitute it
for `app0`:

```sh
python tools/register_app.py -p COM8 --label app0 --metadata ../MyApp/app.json
python tools/register_app.py -p COM8 --list
```

Expect `OK registered`, then a `Registered` entry with the intended UUID/name,
version and image SHA-256. The device hashes the installed image and stores the
registration in NVS. Verify the intended label before registering: this tool
registers whatever valid image is currently in that partition. A future upload
is a separate installation; inspect the list and register that build explicitly.

| Field | Constraint |
| --- | --- |
| `id` | Required lowercase UUID, including hyphens |
| `name` | Required, nonblank, at most 64 UTF-8 bytes |
| `description` | Optional, at most 160 UTF-8 bytes |
| `version` | Optional, at most 32 UTF-8 bytes |
| `build` | Optional integer, 0–4294967295; default 0 |
| `target` | Optional; must be `cardenza` when supplied |
| `source` | Optional `developer` (device default) or `manual` |

Text fields cannot contain ASCII control characters. The compact JSON payload
must fit 1024 UTF-8 bytes. Duplicate UUIDs in other partitions and attempts to
replace an existing identity are rejected by Launcher. Keep the existing UUID
for that app; register a distinct app in its own partition.

## Options and troubleshooting

Use either script's `--help` for all options. Both accept `-p/--port`,
`-b/--baud` (115200 by default) and `--boot-timeout` (15 seconds for upload,
10 seconds for registration). The upload script also accepts `--launcher` for
an explicit Launcher self-update through its temporary free partition; use only
a Cardenza Launcher app BIN and follow the upstream update guide below.

| Symptom | Action |
| --- | --- |
| Missing pyserial/esptool | Install `tools/requirements.txt` in the Python environment used to run the tool. `--no-reset` does not need esptool. |
| Port missing after app USB starts | Return to Launcher using the app UI or HAL system command `0x40`; list ports again. |
| Launcher timeout / `ERR busy` | Return to its home screen, close serial monitors, verify the port and retry with `--no-reset`. |
| Invalid BIN / multiple merged apps | Supply the unsigned Cardenza app-only BIN from the build directory. |
| ACK timeout / interrupted transfer | Stop the tool, return to Launcher and inspect its state before retrying. The tool does not retry binary chunks blindly. |
| No free space / rejected image | Read the device error; inspect installed apps and free space in Launcher. |
| Duplicate ID / changed identity | Inspect `--list`; use the correct partition and existing UUID. |

No backup is performed. Keep power connected during installation. A successful
host check establishes image structure and protocol behavior, not physical
hardware acceptance or power-loss safety. These HAL copies have offline tests;
actual upload, reboot and NVS persistence still require device validation.

## Source and maintenance

Adapted from MIT-licensed `tools/serial_flasher.py` and `tools/register_app.py`
in [203-launcher at 0b0a187](https://github.com/203-Systems/203-launcher/tree/0b0a187).
The inherited license is [LICENSE.launcher](LICENSE.launcher).
See the [application registration and update guide](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md)
and [serial console implementation](https://github.com/203-Systems/203-launcher/blob/main/src/serial_console.cpp).
When Launcher changes these commands, compare the protocol and rerun:

```sh
python -m unittest discover -s tools/tests -v
```
