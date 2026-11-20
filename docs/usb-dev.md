# The development channel over USB

Every firmware on CardenzaHAL that starts USB (`USB::Init`) answers a computer's questions about itself over
the HAL's system channel, without a line of the firmware's own: what it is, its partitions, its memory and
tasks, the SD card, the I2C bus, its screen, and its last crash; and it takes keys pressed from the computer and
files for the card. `203c board` is the computer's side (203-CLI); a web page reaches it over Web MIDI. The
template does not start USB (its console stays on the serial port, which the community's boot check reads): add
`CardenzaHAL::USB::Init();` after `CardenzaHAL::Init()` to use it.

It costs a firmware nothing until a computer asks: no task, no buffer and no polling. A question starts a
worker task of priority 1 (6 KB of internal RAM for its stack, created then and deleted 10 s after the last
question); a screenshot copies one frame at the firmware's next `Present`; nothing else in the firmware's paths
changes. `CONFIG_CARDENZA_HAL_USB_DEV` (Kconfig, on by default) leaves it out altogether: every question is then
answered with the error `off`.

What it never does: write NVS or anything of the Launcher's, send the device token, or reach files outside the
SD card. The only writes are the ones asked for: files on the card (`PUT`, `RM`, `MKDIR`, `MV`) and erasing the
core dump (`COREDUMP_ERASE`).

## Transport

The system command `0x45` (`USB::kCommandDev`), next to the other system commands (`usb/usb.h`), over either way
the system channel goes; the answer comes back the same way:

- **HID**, report `0xCB`, 63 bytes: request `[0x45, id, op, length, args…]`, args raw (`length` of them, at
  most 52); answer
  `[0xC5, id, seq_lo, seq_hi, flags, length, payload…]` (payload at most 57 bytes; `length` says how many).
- **SysEx**: request `F0 23 45 id op <args, packed> F7`; answer `F0 24 45 id seq_lo seq_hi flags <payload,
  packed> F7` (payload at most 48 bytes before packing). Packing (7-bit): each group of up to 7 bytes becomes a
  byte of their top bits (bit *i* = the top bit of byte *i* of the group) followed by the 7 bytes' low 7 bits.
  A request's args are at most 52 bytes after unpacking (the SysEx the HAL collects is 63 bytes).

`id` (0–127) is the computer's, chosen per request; every answer to it carries it. `seq` counts the answer's
messages from 0, 14 bits (`seq_lo` the low 7, `seq_hi` the next 7), wrapping. `flags`: bit 0 `LAST` (the
answer's final message), bit 1 `ERROR` (this message's payload is the reason, UTF-8, and it is the last). A
computer that finds a gap in `seq` has lost part of the answer: it asks again.

Requests are queued (8 at most) and answered one after the other. A request that finds the queue full is
answered at once with the error `busy`. `ARG` (`0x0E`) lengthens a request beyond 52 bytes: its args are
appended to that id's argument buffer (512 bytes), and the next other request with the same id gets that buffer
in front of its own args. `CANCEL` (`0x7F`, any id) stops the answer being sent (it ends with the error
`cancelled`), drops what is queued and releases keys pressed from the computer.

The protocol version in `IDENTIFY`'s answer (`0x44`) is 2 from this channel on.

## Requests

JSON answers are one UTF-8 JSON object, split over the messages as it comes. Sizes are bytes; times
milliseconds. A field the firmware cannot know is `null`.

| op | name | args | answer |
| --- | --- | --- | --- |
| `0x00` | `HELLO` | | JSON `{"protocol": 2, "max_args": 52}` |
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
| `0x7F` | `CANCEL` | | the cancelled answer's error |

Numbers in args are little-endian (`u32`: 4 bytes). Paths are on the SD card, from its root: `/` between the
names, no leading `/`, no `.` or `..`, at most 255 bytes (longer than a request: `ARG`); the empty path is the
root.

### INFO

```json
{"protocol": 2,
 "firmware": {"name": "CardenzaSynth", "version": "0.2.0", "code": "V6QK28", "product": "Cardenza",
              "idf": "v5.3.5", "built": "Nov 20 2026 10:00:00", "elf_sha256": "9f2c…", "secure_version": 0},
 "hal": {"commit": "acbc6bd", "dirty": false},
 "launcher": {"name": "203 Launcher", "version": "0.3.1"},
 "chip": {"model": "ESP32-S3", "revision": "0.2", "cores": 2, "cpu_mhz": 240, "mac": "F4:12:FA:00:11:22",
          "flash_bytes": 16777216, "psram_bytes": 2097152},
 "uptime_ms": 81234, "reset": "power-on", "wakeup": null,
 "hal_init": {"system": true, "keyboard": true, "audio": true, "display": true, "storage": true, "ir": true}}
```

`firmware` is the image's own description (`esp_app_get_description`: the project's name, `PROJECT_VER`, the
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
