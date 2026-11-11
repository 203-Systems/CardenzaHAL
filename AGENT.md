# CardenzaHAL Agent Notes

- Sources are ESP-IDF-only. Do not add `ESP_PLATFORM` preprocessor guards or fallback typedefs.
- Keep `CardenzaHAL` lightweight and generic. Expose native ESP-IDF handles rather than app-level wrappers.
- `CardenzaHAL::Audio` exposes the TX `i2s` channel, volume, and `Init(i2s_slot_mode_t)` for mono/stereo configuration. Use native ESP-IDF types; keep PCM conversion and rendering in consumers.
- `CardenzaHAL::Mic` should expose the RX `i2s` channel only. Device-level input start/read/stop wrappers belong above the HAL.
- `CardenzaHAL::Display` should expose native panel and panel-IO handles.
- `CardenzaHAL::System::GetI2CBus()` belongs in the public `system.h` surface.
- Keep includes relative to this repository; never include through a consumer's path such as `Devices/Cardenza/cardenza/...`.
- Target the current board only: LED_EN GPIO47, clock GPIO48, data GPIO21. Do not add legacy pin maps or compatibility APIs.
- Keyboard LED APIs use 0–15. Keep per-key scanning at four bit planes / 16 duties and global hardware PWM at a fixed gamma-2 curve. Do not add alternative brightness modes.
- Keyboard OE uses LEDC timer 1 / channel 6; backlight uses timer 0 / channel 7. Both must use APB because ESP32-S3 low-speed LEDC timers share their clock source.
- `docs/developing.md` (and its Chinese twin) is what every firmware developer should know about the chip, the toolchain and the 203 Launcher; add to it when a firmware learns something the hard way, with the measurement. `examples/` are small ESP-IDF projects that build (`idf.py -C examples/<name> build`); they are not part of the component.
- `tools/` holds host tools for firmware developers (Python 3, standard library plus hidapi / sounddevice where noted): keep them generic (any firmware on the HAL), importable, and tested in `tests/` without a board. A firmware's own protocol on the app channel belongs in that firmware's tools.
- `board/` holds the board's defaults (`sdkconfig.defaults`, `sdkconfig.debug`, `partitions.csv`); none is enforced. `board/partitions.csv` keeps the 203 Launcher's fixed regions exactly as the Launcher has them (203-launcher `boards/cardenza/partitions.csv`: nvs, otadata, the Launcher's slot reserved as data, coredump) and puts the firmware from 0x170000; when the Launcher's layout changes, change it with it. A flash tool must never erase the Launcher without being told to (`tools/cardenza_flash.py --replace-launcher`).
- Keep test apps (firmware), experiments, firmware binaries, build directories, device dumps, and runtime logs outside this HAL repository.
- Host tests of the HAL's own code live in `tests/` (CMake, no ESP-IDF: pure calculations, and the driver sources against the fake ESP-IDF drivers in `tests/hal_faults/fakes`); CI runs them on every push and pull request. A change to a calculation or an error path comes with its test.
- Do not bring back copyleft code (the old GPL `keyboard/keymap.h` was removed on purpose).
- Do not start builds unless the user explicitly asks. Changes here affect every consumer (CardenzaSynth, Cardenza-MPE); bump the submodule in each one.
- USB (`usb/usb.h`): one composite device, TinyUSB configured only by `usb/config/tusb_config.h` (the component injects it into TinyUSB; never add `esp_tinyusb`). All four non-control IN endpoints of the ESP32-S3 are in use; a new interface needs one of them back. Keep Matrix OS's report IDs and command bytes (0xCB system, 0xFF app; 0x40 / 0x41 as in Matrix OS) so host tools work across 203 devices; new HAL commands take the next free byte after 0x44. Restarting out of USB always hands the PHY back to USB-Serial-JTAG first (`sw_hw_usb_phy_sel = 0`): the Launcher and esptool depend on it.

