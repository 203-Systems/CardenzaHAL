# The screen stream (the USB camera)

A firmware on CardenzaHAL can show its screen to a computer as a lossless stream (`usb/camera.h`), made for
recording and streaming tutorials: a web page shows and records it together with the device's USB audio, and a
desktop program can turn it into a virtual camera for OBS. This is the protocol a reader implements; the reference
reader is [`tools/web/screen-stream.js`](../tools/web/screen-stream.js) (a browser, or Node for the decoder alone),
and [`tools/web/screen-stream-demo.html`](../tools/web/screen-stream-demo.html) shows it working.

Exact pixels: the frames carry the screen as the firmware presents it, RGB565, with no lossy step anywhere. Only
what changed since the frame before is sent, as rectangles, raw or in a small run code that takes 30-45 % of the raw
bytes on the groovebox's screens. A frame is sent when something changed and the computer has read the one
before: a computer slower than the firmware gets fewer frames, each with everything that changed since the last
(never a backlog, never a torn picture). An idle screen sends nothing.

## Finding it

The device is any 203 device (USB vendor ID `0x0203`) whose firmware turned the camera on
(`CardenzaHAL::USB::Camera::Enable(true)`, or `USB::Config::camera`; Kconfig `CONFIG_CARDENZA_HAL_USB_CAMERA`). The
stream is on the interface named **"Cardenza WebUSB"**: class `0xFF` (vendor), subclass 0, protocol 0, with one
**bulk IN endpoint** (64-byte packets, full speed). Without the camera the same interface has no endpoint (it then
carries only the system channel, `docs/usb-dev.md`). Its interface number depends on the firmware's other USB
classes: find it by class and endpoint, never by a fixed number.

- **Browser (WebUSB, Chrome and Edge):** `navigator.usb.requestDevice({filters: [{vendorId: 0x0203}]})` once (the
  chooser), then `getDevices()`; `open()`, `selectConfiguration(1)` when none is selected, `claimInterface(n)` for
  the vendor interface with a bulk IN endpoint. Control requests below are `{requestType: 'vendor', recipient:
  'interface', request, value, index: n}`. A secure page (https or localhost).
- **macOS and Linux (libusb):** `libusb_open_device_with_vid_pid(ctx, 0x0203, pid)` or walk the device list for
  vendor `0x0203`; find the interface as above in `libusb_get_active_config_descriptor`; `libusb_claim_interface`.
  macOS: no driver owns a vendor interface, so a program claims it directly (no kernel extension, no entitlement).
  Linux: the user needs access to the device node (a udev rule for vendor `0203`, e.g.
  `SUBSYSTEM=="usb", ATTR{idVendor}=="0203", MODE="0660", TAG+="uaccess"`).
- **Windows (WinUSB):** the device's Microsoft OS 2.0 descriptors bind WinUSB to the interface with no driver to
  install, with the DeviceInterfaceGUID **`{3C0D4A20-0203-4C5A-B1E5-CA4DE2A0B0E1}`**: `SetupDiGetClassDevs` with that
  GUID, `CreateFile` on the path, `WinUsb_Initialize`; the control requests go with `WinUsb_ControlTransfer`
  (`RequestType` `0x41` out, `0xC1` in; `Index` the interface number), the stream with `WinUsb_ReadPipe`. libusb on
  Windows works through the same WinUSB binding.

## Control requests (endpoint 0)

Vendor requests to the interface (`bmRequestType` `0x41` host to device, `0xC1` device to host; `wIndex` the
interface number):

| bRequest | name | direction | wValue | data |
| --- | --- | --- | --- | --- |
| `0x20` | START | out | bit 0: the run code may be used | none. Begins (or begins again) with a keyframe. Fails (stall) when the device has no room for it (it needs 130 KB of PSRAM) |
| `0x21` | STOP | out | 0 | none. Capturing stops; nothing is sent |
| `0x22` | KEYFRAME | out | 0 | none. The next frame is a keyframe |
| `0x23` | INFO | in | 0 | 16 bytes (below) |
| `0x24` | STATS | in | 0 | 64 bytes (below) |

A reader sends START once it reads the endpoint, and KEYFRAME whenever it lost something (a gap in the sequence, a
broken frame, a reader that joined late). Nothing needs STOP: a reset, the cable going or the host reconfiguring
ends the stream, and a reader that stops reading for 2 s pauses it on the device (it captures nothing then; the
first frame read again is a keyframe). START again after a reader came back: it resets the frame state.

INFO: `u8 version` (1), `u8 format` (1: RGB565), `u16 width` (240), `u16 height` (135), `u8 state` (bit 0 started,
bit 1 paused: the reader stopped reading), `u8` 0, `u32` the next frame's sequence, `u32` the largest frame (64852).

STATS (what the device spent; `usb/camera.h` `Stats`): `u8 state` (bit 0 presented, 1 endpoint configured, 2
started, 3 paused), 3 bytes 0, then `u32` each: frames, keyframes, rectangles, the firmware's frames seen, of them
merged into a later frame, bytes sent, the last frame's bytes, the next sequence, capture time in µs (last, most:
the comparison inside the firmware's Present), write time in µs (last, most: building a frame), errors; 8 bytes 0.

## The stream (the bulk IN endpoint)

A byte stream of **frames**, each one bulk transfer: the device ends every frame with a short packet, or with a
zero-length packet when its length is a multiple of 64, so a reader that asks for 65,536 bytes at a time
(`transferIn(ep, 65536)`, `libusb_bulk_transfer`, `WinUsb_ReadPipe`) gets exactly one frame per read. A reader that
reads in smaller pieces parses the bytes as a stream: every frame says its length. All numbers little-endian.

### Frame

| offset | size | field |
| --- | --- | --- |
| 0 | 4 | magic `43 44 5A 53` ("CDZS") |
| 4 | 1 | version: 1 |
| 5 | 1 | flags: bit 0 keyframe (the frame paints the whole screen; nothing before it is needed) |
| 6 | 2 | header bytes: 32 |
| 8 | 4 | frame bytes: the whole frame, header and trailer included (at most 64852) |
| 12 | 4 | sequence: counts the frames sent, from where the device was; wraps at 2^32 |
| 16 | 2 | width: 240 |
| 18 | 2 | height: 135 |
| 20 | 1 | pixel format: 1 = RGB565 (bits 15-11 red, 10-5 green, 4-0 blue), each pixel 2 bytes little-endian |
| 21 | 1 | 0 |
| 22 | 2 | rectangles in the frame |
| 24 | 4 | the device's time in ms when the newest frame in it was presented |
| 28 | 2 | the firmware's frames merged into this one (1: none skipped) |
| 30 | 2 | 0 |
| 32 | ... | the rectangles |
| end - 4 | 4 | trailer `43 44 5A 45` ("CDZE") |

### Rectangle

A 16-byte header, then its data, padded with zeros to a multiple of 4 bytes (the padding is not in its length):

| offset | size | field |
| --- | --- | --- |
| 0 | 2 | x |
| 2 | 2 | y |
| 4 | 2 | width |
| 6 | 2 | height |
| 8 | 1 | encoding: 0 raw, 1 the run code |
| 9 | 3 | 0 |
| 12 | 4 | data bytes (before the padding) |

Raw: `width × height` pixels, rows top to bottom, each left to right, 2 bytes each. Rectangles never overlap; x and
width are multiples of 8. A keyframe is one rectangle over the whole screen.

### The run code

The rectangle's pixels, rows top to bottom and each left to right, as one sequence, in chunks. Each chunk starts with
a `u16` control: its top two bits say what follows, its low 14 bits count pixels `n` (1 to 16383):

| top bits | chunk | then |
| --- | --- | --- |
| `00` | literal | `n` pixels |
| `10` | run | one pixel, repeated `n` times |
| `01` | pair | two pixels A, B: then A B A B ... for `n` pixels (the 2-pixel dithers and stripes of a UI) |
| `11` | reserved | (a reader treats the frame as broken) |

The chunks fill exactly `width × height` pixels.

### Reading it

1. Find the magic; read 12 bytes; check frame bytes (32 + 4 to 64852); wait for the whole frame. Anything that is not
   a frame (a reader that joined mid-frame, a leftover of an earlier reader's transfer) is skipped a byte at a time
   up to the next magic.
2. Check version, header bytes, width, height, format and the trailer; check every rectangle fits the screen and the
   frame before painting any (a broken frame paints nothing; ask for a keyframe).
3. A frame that is not a keyframe paints over the screen of the frame before: before the first keyframe, skip it and
   ask for one (KEYFRAME). A sequence that is not the last one + 1 means a frame is missing: draw on and ask for a
   keyframe.
4. Paint the rectangles into a 240 × 135 RGB565 screen. To 8-bit colour: repeat each channel's top bits into its low
   ones (`r8 = r5 << 3 | r5 >> 2`, `g8 = g6 << 2 | g6 >> 4`, `b8 = b5 << 3 | b5 >> 2`). Scale only by whole
   numbers with nearest neighbour (a canvas with `image-rendering: pixelated`) to keep every pixel exact; 240 × 135
   is 16:9, so a virtual camera at 1920 × 1080 is exactly 8 × 8 device pixels each (1280 × 720: not whole, 5.33 ×).

## What it costs the device

- No reader (or the camera off): nothing. The display's present paths test one pointer.
- Reading: memory, 64.8 KB for the copy of the screen and 64.9 KB for the frame being sent, in PSRAM, while a reader
  has started it; a task of priority 1 with a 3.5 KB stack in internal RAM; about 1 KB of static internal RAM.
  Time: in the firmware's `Present`, the rows it presented compared with the copy, a 16-byte segment at a time (for
  `PresentChanged`, the rows it found changed and its refresh band); in the stream's task, the changed rectangles
  written into the frame (the run code is one pass over them).
- The bus: one bulk IN endpoint, full speed (12 Mbit/s), beside the device's other classes (audio's isochronous
  streams are served first). A computer that cannot take every frame gets the changes merged.

On recorded and synthetic screens (the host test `tests/camera_test.cpp`, `CardenzaCameraTests` prints them): a
keyframe of the groovebox's pages is 8-27 KB (64.8 KB raw); a 2-pixel playhead walking over a sequencer grid about
14 KB/s; CardenzaSynth's recorded screen tours (docs/groovebox/motion in CardenzaSynth: page and scene
changes, animations) 65-245 KB/s at 33 frames a second, 28-39 % of the raw bytes; a full-screen animation of noise
is raw, 64.8 KB a frame, so the bus sets its frame rate.
