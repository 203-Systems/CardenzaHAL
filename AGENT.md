# CardenzaHAL Agent Notes

- Sources are ESP-IDF-only. Do not add `ESP_PLATFORM` preprocessor guards or fallback typedefs.
- Keep `CardenzaHAL` lightweight and generic. Expose native ESP-IDF handles rather than app-level wrappers.
- `CardenzaHAL::Audio` exposes the TX `i2s` channel, volume, and `Init(i2s_slot_mode_t)` for mono/stereo configuration. Use native ESP-IDF types; keep PCM conversion and rendering in consumers.
- `CardenzaHAL::Mic` should expose the RX `i2s` channel only. Device-level input start/read/stop wrappers belong above the HAL.
- `CardenzaHAL::Display` should expose native panel and panel-IO handles.
- `CardenzaHAL::System::GetI2CBus()` belongs in the public `system.h` surface.
- Keep includes relative to this repository; never include through a consumer's path such as `Devices/Cardenza/cardenza/...`.
- Keyboard LED APIs use 0–15. Keep per-key scanning at exactly four bit planes / 16 electrical duties; gamma compensation belongs in the independent global hardware PWM.
- Keyboard OE uses LEDC timer 1 / channel 6; backlight uses timer 0 / channel 7. Both must use APB because ESP32-S3 low-speed LEDC timers share their clock source.
- Keep test apps, experiments, firmware binaries, build directories, device dumps, and runtime logs outside this HAL repository.
- Do not bring back copyleft code (the old GPL `keyboard/keymap.h` was removed on purpose).
- Do not start builds unless the user explicitly asks. Changes here affect every consumer (CardenzaSynth, Cardenza-MPE); bump the submodule in each one.
