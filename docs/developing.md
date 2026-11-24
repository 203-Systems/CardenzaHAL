# Developing a firmware for Cardenza

[简体中文](developing.zh-CN.md)

What the ESP32-S3 on this board does that is easy to miss, learnt the hard way in CardenzaSynth (each number below was measured on the board). The HAL's README covers its API; this covers the chip and the toolchain around it.

## Memory

- **Two RAMs.** Internal RAM (about 300 KB of heap) is fast, and it is the only RAM the SD card's DMA, interrupts and code running while the flash is written can use. PSRAM (2 MB, quad, on the flash bus) is slower: fine for data read per step, per screen or on demand (patterns, snapshots, samples, lists), wrong for anything touched per audio sample, by an interrupt or by DMA. An SD read into PSRAM falls back to one sector per command.
- **Keep malloc out of PSRAM** unless you mean it: [`examples/psram_heap`](../examples/psram_heap) maps the PSRAM and adds it to the heap with its own capability, so drivers and C libraries stay internal and your code asks for PSRAM where it wants it.
- **Measure internal RAM with `MALLOC_CAP_INTERNAL`**: `heap_caps_get_free_size(MALLOC_CAP_8BIT)` counts the PSRAM too.
- **A failed `new` reboots the board.** ESP-IDF builds without exceptions, and libstdc++'s `new (std::nothrow)` calls plain new, so it aborts too. Replace them (the example does): nothrow new returns null, both spill into PSRAM when internal RAM is full. Use nothrow new for anything large and say on screen when it fails. Allocate when used, give back when not; no large static buffers; nothing per audio block.
- **Atomics with PSRAM on.** ESP-IDF then builds everything with `-mdisable-hardware-atomics`: atomics become library calls, and `std::atomic_flag::test_and_set` becomes a plain load and store, not atomic at all (a spinlock built on it froze a board). Spin on `std::atomic<bool>::exchange`. 64-bit atomics are library calls behind one lock both cores share with interrupts off: keep the ones a real-time task uses 32-bit.

## Hot code

- **`std::memcpy` is a call into the ROM** even for one word (ESP-IDF builds with `-fno-builtin-memcpy` and `-memset`). In a loop use `__builtin_memcpy`; with `__builtin_assume_aligned` it is one load (hashing a 240 x 135 frame went from 3.6 ms to 0.4 ms).
- **The FPU has no division**: a float divide is a ROM call of some 80 cycles. Multiply by a reciprocal computed once; none per sample.
- **A function call per sample and voice costs more than the sound.** Render a block at a time.
- **Code from flash runs through a 32 KB instruction cache** (with `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`) shared by both cores. Put the hottest code in IRAM with a linker fragment (`.lf`), code only and only what runs per sample. Build C code full of `1.0` literals with `-fsingle-precision-constant`, or every one is a software double.
- **Flash in QIO at 120 MHz** ([`board/sdkconfig.defaults`](../board/sdkconfig.defaults)): in DIO, code from flash cost about 40% more.

## Flash writes

Writing the internal flash stops the cache on both cores for the length of the write (an NVS write took about 45 ms): code in flash and interrupts that are not in IRAM stop with it (the key LED scan stood still on one row, which lit up). Keep your data on the SD card. If you must use NVS, use your own namespace and never erase the partition: it holds the 203 Launcher's registry of installed software.

Reading the Launcher's Wi-Fi networks and community account needs no write access: [`Net::ReadLauncherSettings`](../net/net.h) opens the partition through a read-only partition entry and never repairs it. Wi-Fi writes to NVS unless told not to: Net starts it with RAM storage, and a firmware that uses Wi-Fi sets `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n` (the PHY otherwise stores its calibration in NVS). The README's "Network" section has the details.

## Files on the SD card

- **Your folder is `/sdcard/Apps/<AppName>/`** ([`storage/app_dir.h`](../storage/app_dir.h), [SD-CARD.md](SD-CARD.md)): nothing at the card's root, nothing in `/sdcard/Launcher/` (the 203 Launcher's). A firmware that moves its folder there calls `Storage::MigrateAppDir` once at the start: one rename, not a copy (FAT renames within the card in milliseconds whatever the folder holds).
- **Never read through an unbuffered `FILE`**: newlib then reads a byte at a time (a 2 KB block took 33 ms and held a core). Use `read()` on the file's descriptor, or give the `FILE` a buffer.
- FAT refuses to rename or delete an open file, and ignores case (`Song` and `SONG` are one file).
- A card read takes about 2 ms whatever its size: read in blocks of several KB, from a task below the UI, and never from the audio task.

## Screen and ticks

- **Present with `Display::PresentChanged` in a `Display::FramePacer` loop** ([DISPLAY.md](DISPLAY.md)): it sends only what changed, in the panel's scan order, at 80 MHz (not yet measured on a board: the HAL's model, `tests/display_bench.cpp`, gives about 0.5 ms for an unchanged screen and 8 ms for a whole one; `Display::LastFrame()` measures yours).
- **A task woken from an `esp_lcd` or SPI callback waits for the next tick** unless the callback asks for the switch (`portYIELD_FROM_ISR`): ESP-IDF's SPI panel IO ignores the callback's return value. The HAL's display waited so for each band of 8 rows until 2026-11-23: a whole screen took 17 ms at 1000 Hz and 170 ms at ESP-IDF's default of 100 Hz. Any driver of yours that waits on such a callback has the same problem.
- **The panel has no tearing-effect line**: what is written while it scans can show half old and half new for one refresh. Small steps per frame and pages changed in one frame hide it; [DISPLAY.md](DISPLAY.md) has the details.

## USB

`CardenzaHAL::Init` starts USB (`CONFIG_CARDENZA_HAL_USB_AUTOSTART`), which takes the PHY from USB-Serial-JTAG: the firmware has no USB-Serial-JTAG port any more, only the classes it chose (MIDI, audio, HID, WebUSB by default; a CDC serial port if you turn it on, which also answers esptool's reset). Read its console with `203cli log` (or [`tools/cardenza_usb.py`](../tools/cardenza_usb.py) `console`), and flash it with `203cli flash`, or after `command download` (ROM download mode) or `command launcher`. The ESP32-S3 has four IN endpoints besides EP0 and the default classes take them all: the build stops with what to turn off when yours need more ([README](../README.md#usb-device-usage)). For JTAG debugging on USB-Serial-JTAG, build with `CONFIG_CARDENZA_HAL_USB_AUTOSTART` off and do not call `USB::Init`.

## When it hangs or crashes

- Debug builds: add [`board/sdkconfig.debug`](../board/sdkconfig.debug); a hung task then restarts the board with a backtrace.
- Print the version, the Git commit and the ELF's hash (`esp_app_get_elf_sha256`) first at every start ([`cmake/firmware_info.cmake`](../cmake/firmware_info.cmake)), and keep the ELF of every build you give anyone: `xtensa-esp32s3-elf-addr2line -pfiaC -e <firmware>.elf <addresses>`.
- Keep the log where a crash cannot take it: RTC slow memory survives a panic and a software restart (not a power cut). CardenzaSynth's `Devices/Cardenza/DeviceLog.cpp` keeps such a ring and copies it to the SD card.

## Running under the 203 Launcher

A Cardenza normally runs the 203 Launcher, which installs and starts firmwares. A firmware it starts:

- is installed into a slot the Launcher makes: it never changes the partition table, the otadata, or the Launcher's slot. From a computer, [`tools/uploader`](../tools/uploader/README.md) sends a build to the Launcher (`serial_flasher.py`) and registers its name, UUID and version (`register_app.py`). A board is flashed directly only when it has no Launcher ([`board/partitions.csv`](../board/partitions.csv) keeps the Launcher's regions even then, and [`tools/cardenza_flash.py`](../tools/cardenza_flash.py) refuses to erase a Launcher);
- keeps its data on the SD card, in its own folder `/sdcard/Apps/<AppName>/` (the Launcher's are in `/sdcard/Launcher/`), and leaves the Launcher's NVS records alone (above); it may read the Wi-Fi networks and community account the Launcher saved, through `net/net.h`;
- runs with the Launcher's bootloader, which sets the flash to QIO: build it so. Its header also sets the flash size the firmware sees (`esp_flash_get_size`): 16 MB, or 8 MB on a board flashed with Launcher 0.3.3 or earlier (until its bootloader is written again over USB), although every Cardenza has 16 MB. Build for 16 MB ([`board/sdkconfig.defaults`](../board/sdkconfig.defaults)), leave `CONFIG_SPI_FLASH_SIZE_OVERRIDE` off and never touch flash outside your own partitions;
- carries its version in its app description: `PROJECT_VER` (`cardenza_read_version`) is the version the Launcher shows under it. Its name in the list is not the project name: it is the name it was registered or installed under, and a directly flashed firmware shows its partition label;
- goes back to the Launcher with [`System::RestartIntoLauncher()`](../system/launcher.h) (the Launcher's restart ticket: its home screen even when "Resume after reset" is on, where a plain `esp_restart()` starts the firmware again; `command launcher` over USB does the same from a computer; by hand, the system menu's Back to Launcher, a press of HOME away, or FN held while the board starts);
- stays reachable from the 203 community while it runs: the presence service ([`net/presence.h`](../net/presence.h), on by default) keeps the device online and hands firmware sent from the website (and the Launcher's own update) over to the Launcher. Give it a hook to save your work first (or refuse); tell it what you do (`Presence::SetActivity`: `RealTime` while sound plays, `Quiet` when you need Wi-Fi's RAM and time back) and how much internal RAM you keep (`SetRamFloor`); switch it off (`Presence::Enable(false)`, or Kconfig) before running Wi-Fi yourself;
- is released on the 203 community, like every firmware including 203 Systems' own: from CI with [`.github/actions/community-publish`](../.github/actions/community-publish/action.yml) and trusted publishing (dev for a push to main, stable for a tag `vX.Y.Z` equal to its version, beta for `vX.Y.Z-beta.N`), the first version by hand with `203cli` ([FIRMWARE-PROJECT.md](FIRMWARE-PROJECT.md#8-publish): the files a project needs, what the community checks, versions and channels). The old Launcher hub ([`launcher-publish`](../.github/actions/launcher-publish/action.yml)) only receives a mirror of 203's official releases while Launchers 0.2.0 are in use.

The Launcher's own documentation has the full rules (registration, metadata, installation).
