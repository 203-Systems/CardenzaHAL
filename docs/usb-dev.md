# The development channel over USB

Every firmware on CardenzaHAL answers a computer's questions about itself over the HAL's system channel, without
a line of the firmware's own: what it is, its partitions, its memory and tasks, the SD card, the I2C bus, its
screen, and its last crash; and it takes keys pressed from the computer and files for the card. `203cli board` is
the computer's side (203-CLI); a web page reaches it over Web MIDI, WebHID, WebUSB or Web Serial.
`CardenzaHAL::Init` starts USB (Kconfig `CONFIG_CARDENZA_HAL_USB_AUTOSTART`, on by default; before 2026-11-24 a
firmware had to call `USB::Init` itself, and the template did not), so every firmware has it.

The board's settings ([SETTINGS.md](SETTINGS.md): volume, its limit, brightness, key lights,
language) are on the same channel (`SETTINGS` … below), in every firmware that starts USB, whatever
`CONFIG_CARDENZA_HAL_USB_DEV` says: `203cli board settings` reads and changes them.

It costs a firmware nothing until a computer asks: no task, no buffer and no polling. A question starts a
worker task of priority 1 (6 KB of internal RAM for its stack, created then and deleted 10 s after the last
question); a screenshot copies one frame at the firmware's next `Present`; nothing else in the firmware's paths
changes. `CONFIG_CARDENZA_HAL_USB_DEV` (Kconfig, on by default) leaves the development channel's questions out:
they are then answered with the error `off`; `HELLO`, `INFO` and the settings are still answered.

What it never does: send the device token, or reach files outside the SD card. The only writes are the ones asked
for: files on the card (`PUT`, `RM`, `MKDIR`, `MV`), erasing the core dump (`COREDUMP_ERASE`), and a setting
(`SETTING_SET`, `SETTINGS_RESET`), which the HAL writes to NVS as it writes every change of the settings: a second
after the last one, on its own task (SETTINGS.md).

## Transport

The system command `0x45` (`USB::kCommandDev`), next to the other system commands (`usb/usb.h`), over every way
the system channel goes (whichever the firmware's USB classes have; a device always has one); the answer comes
back the same way:

- **HID**, report `0xCB`, 63 bytes: request `[0x45, id, op, length, args…]`, args raw (`length` of them, at
  most 52); answer
  `[0xC5, id, seq_lo, seq_hi, flags, length, payload…]` (payload at most 57 bytes; `length` says how many).
- **SysEx**: request `F0 23 45 id op <args, packed> F7`; answer `F0 24 45 id seq_lo seq_hi flags <payload,
  packed> F7` (payload at most 48 bytes before packing). Packing (7-bit): each group of up to 7 bytes becomes a
  byte of their top bits (bit *i* = the top bit of byte *i* of the group) followed by the 7 bytes' low 7 bits.
  A request's args are at most 52 bytes after unpacking (the SysEx the HAL collects is 63 bytes).

- **WebUSB** (the "Cardenza WebUSB" interface: vendor class, no endpoints; a page claims it): the HID form as
  control transfers. Request: OUT, `bmRequestType` vendor | interface, `bRequest` `0x10` (`USB::kWebUsbSend`),
  `wIndex` the interface number, data `[0xCB, 0x45, id, op, length, args…]` (the report ID first, at most 64
  bytes). Answers: IN, `bRequest` `0x11` (`USB::kWebUsbReceive`), `wLength` 64: `[0xCB, 0xC5, id, …]` (63 bytes
  after the ID), or no data while none waits: ask again (every few milliseconds while an answer is due). The
  device keeps 8 answers for the page; a page that stops asking for 250 ms counts as gone. Report `0xFF` (the
  firmware's own) goes the same way. The BOS names the landing page (`CONFIG_CARDENZA_HAL_USB_WEBUSB_URL`,
  stage.203.io) and gives Windows the Microsoft OS 2.0 descriptors that bind WinUSB to the interface, with no
  driver to install (DeviceInterfaceGUID `{3C0D4A20-0203-4C5A-B1E5-CA4DE2A0B0E1}`).
- **The serial port** (CDC, when the firmware has it): the HID form as text lines, `sys <hex>` for a report
  `0xCB` (`sys 45 01 01 00`: INFO, id 1) and `app <hex>` for a report `0xFF`; each answer report comes back as a
  line `sys <126 hex digits>`. It also takes words, for people and tools that read lines (below).

`id` (0–127) is the computer's, chosen per request; every answer to it carries it. `seq` counts the answer's
messages from 0, 14 bits (`seq_lo` the low 7, `seq_hi` the next 7), wrapping. `flags`: bit 0 `LAST` (the
answer's final message), bit 1 `ERROR` (this message's payload is the reason, UTF-8, and it is the last). A
computer that finds a gap in `seq` has lost part of the answer: it asks again.

Requests are queued (8 at most) and answered one after the other. A request that finds the queue full is
answered at once with the error `busy`. `ARG` (`0x0E`) lengthens a request beyond 52 bytes: its args are
appended to that id's argument buffer (512 bytes), and the next other request with the same id gets that buffer
in front of its own args. `CANCEL` (`0x7F`, any id) stops the answer being sent (it ends with the error
`cancelled`), drops what is queued and releases keys pressed from the computer.

The system command `0x46` (`USB::kCommandSeen`, answered `0xC6`), over any way: a 203 tool or the community's
website has seen this board. With the board's setting `webusb_landing` at "first time" (the default,
[SETTINGS.md](SETTINGS.md)) the BOS no longer names the WebUSB landing page from the next enumeration on (every
Chromium browser running shows a notice for it at each plug-in); WebUSB itself works the same without it. The
board also marks itself seen the first time a computer configures it with the page offered, so a new board shows
the notice once even if no 203 tool ever connects. Send it whenever a tool or a page connects to a board.

The protocol version in `IDENTIFY`'s answer (`0x44`) is 2 from this channel on, 3 from the settings on, 4 from
the serial port and WebUSB on (INFO is then answered also with `CONFIG_CARDENZA_HAL_USB_DEV` off, and says how
the board shows up on USB).

## The serial port's words

A firmware with the serial port (`CONFIG_CARDENZA_HAL_USB_SERIAL`, or added by the HAL when the firmware has no
other command channel: no MIDI, HID or WebUSB) answers lines as the 203 Launcher's console does, so a tool treats
every 203 device the same way. Lines end with `\n` (`\r` is ignored), at most 160 characters; the baud rate does
not matter. The console (stdout, stderr, `ESP_LOG`) is on the same port, on by default; answers are whole lines,
never cut by it.

| line | answer |
| --- | --- |
| `version` | `Firmware <project name> <version>` (the Launcher's: `Launcher <version>`) |
| `info`, `hello`, `partitions`, `memory`, `storage`, `status`, `i2c`, `settings`, `coredump` | that request's JSON, one line (`coredump`: COREDUMP_INFO) |
| `launcher` | `OK launcher`, then the restart into the Launcher's home screen (its ticket) |
| `download` | `OK download`, then the ROM's download mode: the board shows up as USB-Serial-JTAG (303A:1001), a new port |
| `reboot` | `OK reboot`, then the firmware restarts |
| `log on`, `log off` | `OK log on` / `OK log off`: the console on this port |
| `seen` | `OK seen`: as the system command 0x46 (below) |
| `sys <hex>`, `app <hex>` | reports `0xCB` / `0xFF` (above) |
| `help` | the commands, then `OK` |
| anything else | `ERR unknown command, type 'help' for command list` |

An error is `ERR <reason>` (`ERR off` for the development channel's questions with `CONFIG_CARDENZA_HAL_USB_DEV`
off). The port also restarts the board into the ROM's download mode the way esptool and Arduino ask a serial
port to: esptool's reset sequences (DTR and RTS: ClassicReset, UnixTightReset; `--before default_reset`), and
Arduino's 1200 baud touch (opened at 1200 baud, then closed). The board then shows up as USB-Serial-JTAG, a new
port: esptool run on the firmware's port resets the board and then cannot follow it, so run it again on the new
port (203cli and the website do that themselves).

## Requests

JSON answers are one UTF-8 JSON object, split over the messages as it comes. Sizes are bytes; times
milliseconds. A field the firmware cannot know is `null`.

| op | name | args | answer |
| --- | --- | --- | --- |
| `0x00` | `HELLO` | | JSON `{"protocol": 4, "max_args": 52, "dev": true, "settings": true, "info": true, "serial": false, "webusb": true}` (`dev`: the development channel's questions are answered; `info`: INFO is, whatever `dev` says (4); `serial`, `webusb`: the board has that way too; 2 had neither field) |
| `0x01` | `INFO` | | JSON, below |
| `0x02` | `PARTITIONS` | | JSON, below |
| `0x03` | `MEMORY` | `[window]` (optional, 10 ms units, default 50) | JSON, below |
| `0x04` | `STORAGE` | | JSON, below |
| `0x05` | `STATUS` | | JSON, below |
| `0x06` | `I2C` | | JSON, below |
| `0x0E` | `ARG` | bytes | nothing (no answer) |
| `0x10` | `SCREEN` | | binary, below |
| `0x11` | `KEYS` | `[key, action, wait]…` | JSON `{"down": [keys still down]}` |
| `0x20` | `COREDUMP_INFO` | | JSON, below |
| `0x21` | `COREDUMP_READ` | | binary: the dump as it is in the partition |
| `0x22` | `COREDUMP_ERASE` | | JSON `{"erased": true}` |
| `0x30` | `LS` | path | JSON `{"path": "...", "entries": [{"name", "dir", "bytes", "modified"}]}` |
| `0x31` | `GET` | u32 offset, path | binary: u32 the file's size, then its bytes from `offset` |
| `0x32` | `PUT_BEGIN` | u32 size, path | JSON `{"path": "...", "bytes": size}` |
| `0x33` | `PUT_DATA` | u32 offset, bytes (at most 48) | binary: u32 bytes written so far |
| `0x34` | `PUT_END` | | JSON `{"path": "...", "bytes": n}` |
| `0x35` | `PUT_ABORT` | | JSON `{"aborted": true}` |
| `0x36` | `RM` | path | JSON `{"removed": "..."}` |
| `0x37` | `MKDIR` | path | JSON `{"made": "..."}` |
| `0x38` | `MV` | from, `0`, to | JSON `{"moved": "...", "to": "..."}` |
| `0x40` | `SETTINGS` | | JSON, below: every setting |
| `0x41` | `SETTING_GET` | key | JSON `{"setting": {…}, "saved": true}` |
| `0x42` | `SETTING_SET` | i32 value, key | JSON `{"asked": 80, "changed": ["volume"], "setting": {…}, "saved": false}` |
| `0x43` | `SETTINGS_RESET` | | JSON `{"reset": true, "settings": […], "saved": false}` |
| `0x7F` | `CANCEL` | | the cancelled answer's error |

Numbers in args are little-endian (`u32`: 4 bytes). Paths are on the SD card, from its root: `/` between the
names, no leading `/`, no `.` or `..`, at most 255 bytes (longer than a request: `ARG`); the empty path is the
root.

### INFO

```json
{"protocol": 4,
 "firmware": {"name": "CardenzaSynth", "version": "0.2.0", "code": "V6QK28", "product": "Cardenza",
              "idf": "v5.3.5", "built": "Nov 20 2026 10:00:00", "elf_sha256": "9f2c…", "secure_version": 0},
 "hal": {"commit": "acbc6bd", "dirty": false},
 "launcher": {"name": "203 Launcher", "version": "0.3.1"},
 "chip": {"model": "ESP32-S3", "revision": "0.2", "cores": 2, "cpu_mhz": 240, "mac": "F4:12:FA:00:11:22",
          "flash_bytes": 16777216, "psram_bytes": 2097152},
 "board": "Cardenza",
 "usb": {"vid": 515, "pid": 4288, "serial_number": "F4:12:FA:00:11:22", "started": true,
         "classes": ["midi", "audio", "hid", "webusb"]},
 "uptime_ms": 81234, "reset": "power-on", "wakeup": null,
 "hal_init": {"system": true, "keyboard": true, "audio": true, "display": true, "storage": true, "ir": true}}
```

`usb` is how the board shows up: its IDs, its serial number (the chip's MAC in USB-Serial-JTAG's form, the same as
`chip.mac` and as the ROM's download mode and the Launcher show: a computer matches the chip across them by it)
and the classes it presents (`midi`, `audio` (`audio-adaptive`: its playback without the feedback endpoint,
`audio-recording`: only the recording), `hid`, `webusb`, `serial`, `drive`, `camera`). Answered by every firmware
with protocol 4, also with the development channel off. `firmware` is the image's own description (`esp_app_get_description`: the project's name, `PROJECT_VER`, the
ELF's SHA-256, the one a backtrace is decoded with) and `code` the community code from the project's
`203.json`, read at build time. `hal.commit` is the HAL's Git commit at build time (`dirty`: built with local
changes). `launcher` is read from the Launcher's own image (`null` on a board without it). `reset` is
`esp_reset_reason`: `power-on`, `external`, `software`, `panic`, `interrupt-watchdog`, `task-watchdog`,
`watchdog`, `deep-sleep`, `brownout`, `sdio`, `usb`, `jtag`, `efuse`, `power-glitch`, `cpu-lockup` or
`unknown`; `wakeup` the deep-sleep wake cause after `deep-sleep`. `flash_bytes` is the flash size the firmware sees
(`esp_flash_get_size`): what the bootloader declares, so 8388608 on a board flashed with Launcher 0.3.3 or
earlier (every Cardenza has 16 MB).

### PARTITIONS

```json
{"flash_bytes": 16777216, "running": "ota_0",
 "partitions": [{"label": "nvs", "type": "data", "subtype": "nvs", "offset": 36864, "size": 20480,
                 "nvs": {"pages": 5, "entries": 630, "used": 211}},
                {"label": "ota_0", "type": "app", "subtype": "ota_0", "offset": 1507328, "size": 4194304,
                 "image": 1834208, "running": true}],
 "free_bytes": 7340032}
```

Every partition of the table on the board (the Launcher's, under the Launcher), in flash order. `image`: the
size of the application image in an `app` partition (and in the Launcher's slot), from its header (`null`
when there is none). `nvs` counts the NVS pages and their entries (126 a page; `used`: written ones), read from
the flash as it is, without the NVS library. `free_bytes`: flash after the last partition. Everything is read
through the cache (memory-mapped), never with the cache disabled.

### MEMORY

```json
{"internal": {"total": 391000, "free": 120000, "min_free": 90000, "largest": 60000},
 "psram": {"total": 2097152, "free": 1500000, "min_free": 1400000, "largest": 1400000},
 "dma": {"free": 110000, "largest": 60000},
 "tasks": [{"name": "main", "priority": 1, "core": 1, "stack_free": 2100, "state": "blocked", "cpu": 12.5}],
 "cpu_window_ms": 500}
```

`tasks` needs `CONFIG_FREERTOS_USE_TRACE_FACILITY` (`null` without it) and `core` needs
`CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID` (`null` without it; `-1` for a task on both cores); the board's debug
defaults (`board/sdkconfig.debug`) set both. `stack_free` is the least the task's stack has had free. `cpu` (percent of one
core over `cpu_window_ms`, idle tasks included) needs `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` (the board's
debug defaults set it); without it `cpu` is `null` and no window is waited for.

### STORAGE

```json
{"mounted": true, "path": "/sdcard", "card_bytes": 31914983424, "total": 31902400512, "free": 30000000000}
```

`total` and `free` from the file system (`null` while the card is not mounted).

### STATUS

```json
{"display": {"ready": true, "framebuffer": true, "brightness": 80},
 "keyboard": {"down": [1, 29], "led_brightness": 255},
 "menu": {"enabled": true, "open": false},
 "usb": {"mounted": true, "audio_in": false, "audio_out": true, "playback_restarts": 0},
 "presence": "connected"}
```

`presence`: the community presence service's state (`net/presence.h`), `null` in a firmware without it.

### I2C

```json
{"sda": 2, "scl": 1, "devices": [{"address": 8, "name": "ES8156 (codec)"}]}
```

Every 7-bit address from `0x08` to `0x77` probed on the shared bus (the expansion port's).

### SCREEN

Binary: a header of 8 bytes, then the pixels. Header: `'S'`, version `1`, width (u16), height (u16),
encoding (`0` raw, `1` run-length), source (`0` a frame the firmware presented after the request, `1` the
firmware's last frame: it presented none for 250 ms). Pixels are RGB565 as the firmware's framebuffer holds
them (u16 little-endian, red in bits 15–11), row by row. Run-length: `[count (1–255), pixel (u16)]` pairs.
A firmware that draws through the panel's handles itself (`Display::GetPanel`) has no framebuffer: the error
`no framebuffer`. While the system menu is open it is still the firmware's frame.

### KEYS

Each step is three bytes: the key (1–56: `y * 14 + x + 1`, the HAL's key numbers, 1 is ESC at the top left;
57 is HOME), the action (`1` down, `0` up) and how long to wait after it (10 ms units). The firmware sees these
keys as if they were pressed (`Keyboard::updateKeyList`, and HOME through `System::GetHomeButton`, which opens
the system menu as a real press does), on top of the real ones. Keys still down are released 10 s after the
last request, by `CANCEL`, and when the computer goes away.

### COREDUMP_INFO

```json
{"partition": "coredump", "size": 65536, "present": true, "bytes": 41236, "enabled": true,
 "summary": {"task": "main", "pc": "0x42012345", "backtrace": ["0x42012345", "0x42013456"], "corrupted": false}}
```

`enabled`: the firmware writes a core dump when it crashes (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`; the board's
debug defaults set it, in ELF format: about 3 KB of internal RAM, so the release defaults do not). `summary` only for an ELF dump this firmware can read (`null` otherwise);
decode its addresses with the firmware's ELF (`xtensa-esp32s3-elf-addr2line -pfiaC -e <elf> <addresses>`), or
the whole dump with ESP-IDF's `esp-coredump info_corefile -c <dump> <elf>`.

### SETTINGS

```json
{"settings": [{"key": "volume", "value": 64, "default": 70, "min": 0, "max": 90, "step": 2, "unit": "", "stored": true},
              {"key": "volume_limit", "value": 90, "default": 100, "min": 10, "max": 100, "step": 2, "unit": "", "stored": true},
              {"key": "screen_brightness", "value": 80, "default": 100, "min": 10, "max": 100, "step": 5, "unit": "%", "stored": true},
              {"key": "key_brightness", "value": 15, "default": 15, "min": 0, "max": 15, "step": 1, "unit": "", "stored": false},
              {"key": "language", "value": 1, "default": 0, "choices": [{"value": 0, "name": "en"}, {"value": 1, "name": "zh-Hans"},
               {"value": 3, "name": "ja"}, {"value": 4, "name": "id"}, {"value": 5, "name": "vi"}], "stored": true}],
 "saved": true}
```

The board's settings ([SETTINGS.md](SETTINGS.md)) in this order. `max` of `volume` is the volume limit now.
`step` is the system menu's step; any whole value in the range is taken. `stored`: the value came from the flash or
was set since the start (otherwise it is the default). `saved`: nothing waits to be written to the flash (a change is
written a second after the last one). The language's `value` is the 203 Launcher's code (its NVS `launcher/lang`).

`SETTING_SET`'s value is clamped to the range (the volume to the limit; a language that is not one of the choices
changes nothing): `setting.value` is what was set, `changed` every key whose value changed (lowering the limit
changes the volume too). It is applied at once (heard, seen) and written to the flash a second later. An unknown key
is the error `unknown setting`. `SETTINGS_RESET` puts every setting but the language back to its default.

