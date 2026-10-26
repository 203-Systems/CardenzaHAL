# CardenzaHAL Agent Notes

- Sources are ESP-IDF-only. Do not add `ESP_PLATFORM` preprocessor guards or fallback typedefs.
- Keep `CardenzaHAL` lightweight and generic. Expose native ESP-IDF handles rather than app-level wrappers.
- `CardenzaHAL::Audio` exposes the TX `i2s` channel, volume, and `Init(i2s_slot_mode_t)` for mono/stereo configuration. Use native ESP-IDF types; keep PCM conversion and rendering in consumers.
- `CardenzaHAL::Mic` should expose the RX `i2s` channel only. Device-level input start/read/stop wrappers belong above the HAL.
- `CardenzaHAL::Display` should expose native panel and panel-IO handles.
- `CardenzaHAL::System::GetI2CBus()` belongs in the public `system.h` surface.
- Keep includes relative to this repository; never include through a consumer's path such as `Devices/Cardenza/cardenza/...`.
- Do not bring back copyleft code (the old GPL `keyboard/keymap.h` was removed on purpose).
- Do not start builds unless the user explicitly asks. Changes here affect every consumer (CardenzaSynth, Cardenza-MPE); bump the submodule in each one.
