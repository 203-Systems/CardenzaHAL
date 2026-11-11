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

## Files on the SD card

- **Never read through an unbuffered `FILE`**: newlib then reads a byte at a time (a 2 KB block took 33 ms and held a core). Use `read()` on the file's descriptor, or give the `FILE` a buffer.
- FAT refuses to rename or delete an open file, and ignores case (`Song` and `SONG` are one file).
- A card read takes about 2 ms whatever its size: read in blocks of several KB, from a task below the UI, and never from the audio task.

## Screen and ticks

`Display::Present` sends 8-row bands and waits for each: keep FreeRTOS at 1000 Hz (at 100 Hz a whole screen takes 170 ms), and send only the rectangles that changed (a whole screen is 17 ms of SPI).

## USB

A firmware that starts USB (`USB::Init`) takes the PHY from USB-Serial-JTAG: it has no serial port any more. Read its console with [`tools/cardenza_usb.py`](../tools/cardenza_usb.py) `console`, and flash it after `command download` (ROM download mode) or `command launcher`.

## When it hangs or crashes

- Debug builds: add [`board/sdkconfig.debug`](../board/sdkconfig.debug); a hung task then restarts the board with a backtrace.
- Print the version, the Git commit and the ELF's hash (`esp_app_get_elf_sha256`) first at every start ([`cmake/firmware_info.cmake`](../cmake/firmware_info.cmake)), and keep the ELF of every build you give anyone: `xtensa-esp32s3-elf-addr2line -pfiaC -e <firmware>.elf <addresses>`.
- Keep the log where a crash cannot take it: RTC slow memory survives a panic and a software restart (not a power cut). CardenzaSynth's `Devices/Cardenza/DeviceLog.cpp` keeps such a ring and copies it to the SD card.

## Running under the 203 Launcher

A Cardenza normally runs the 203 Launcher, which installs and starts firmwares. A firmware it starts:

- is installed into a slot the Launcher makes: it never changes the partition table, the otadata, or the Launcher's slot. A board is flashed directly only when it has no Launcher ([`board/partitions.csv`](../board/partitions.csv) keeps the Launcher's regions even then, and [`tools/cardenza_flash.py`](../tools/cardenza_flash.py) refuses to erase a Launcher);
- keeps its data on the SD card, in its own folder, and leaves the Launcher's NVS records alone (above);
- runs with the Launcher's bootloader, which sets the flash to QIO: build it so;
- names itself in its app description: `PROJECT_VER` is the version the Launcher shows (`cardenza_read_version`), the project name what it shows for a firmware it does not know;
- goes back to the Launcher with a plain restart (`esp_restart()`; `command launcher` over USB does it from a computer);
- is published to the Launcher hub with [`.github/actions/launcher-publish`](../.github/actions/launcher-publish/action.yml): a Nightly for a push to main, a release for a tag `vX.Y.Z` equal to its version.

The Launcher's own documentation has the full rules (registration, metadata, installation).
