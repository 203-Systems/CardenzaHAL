#pragma once

#include <cstdint>

// The board's system settings (the user's request 2026-11-23: "像是亮度、输出音量等等，这些应该作为全局的系统设置 ...
// 所有使用HAL的固件都通用，能够读取。放置在Flash里面"): the same values in every firmware on CardenzaHAL, kept in
// the board's flash by the HAL, shown and changed in the system menu (HOME > Settings, system/menu.h) and over
// USB (docs/usb-dev.md, `203cli board settings`).
//
//   Volume            the output volume, 0 to 100 by ear (0 silent): -60 (1 - v / 100)^1.5 dB at the codec,
//                     about -59 dB at 1, -21 dB at 50, -10 dB at 70 (the default), 0 dB at 100
//   VolumeLimit       the most Volume may be, 10 to 100 (default 100): every Set keeps Volume under it
//   ScreenBrightness  the backlight, 10 to 100 % (default 100): the 203 Launcher's own setting, the same
//                     light in the Launcher, the menu and every firmware
//   KeyBrightness     the key lights' master level, 0 to 15 (default 15; Keyboard::setLEDGlobalBrightness)
//   Language         the 203 Launcher's language (its NVS value launcher/lang): 0 English, 1 简体中文,
//                     3 日本語, 4 Bahasa Indonesia, 5 Tiếng Việt (2 reads as 1)
//
// CardenzaHAL::Init reads them (without writing anything) and applies them before the firmware's first
// frame: the codec's volume, the backlight, the key lights' master level. A firmware reads them with Get and
// may offer its own shortcuts with Set. A firmware that sets those itself (Audio::SetVolume,
// Display::SetBrightness, Keyboard::setLEDGlobalBrightness) takes that part over from the setting until the
// setting changes again: a firmware made before these settings keeps working as it did.
//
// The microphone's gain is not one of them (the user, 2026-11-23: a firmware's, kept and given by the firmware):
// a firmware that records keeps its own and passes it to Mic::ApplyGain (mic/mic.h).
//
// Flash: written by the HAL only, in the NVS partition the 203 Launcher shares (docs/SETTINGS.md): the
// screen's brightness and the language as the Launcher's own keys (launcher/bright, launcher/lang), the rest
// in the namespace hal_system. Writing the flash stalls both cores' caches, so it is never done per change:
// once a second after the last change (or when the system menu closes, or at Flush), only what changed, on a
// task of the HAL's own, with the speaker faded out around it and the key lights dark (their scan is not in
// IRAM). A board whose NVS cannot be opened without erasing it keeps the settings for this run only.
namespace CardenzaHAL::Settings {

    enum class Id : uint8_t {
        Volume,
        VolumeLimit,
        ScreenBrightness,
        KeyBrightness,
        Language,
    };
    inline constexpr int kCount = 5;

    struct Info {
        const char* key;   // its name over USB and in logs: volume, volume_limit, screen_brightness,
                           // key_brightness, language
        int min, max;
        int step;          // the system menu's step
        int defaultValue;
        const char* unit;  // "" or "%"
    };
    const Info& Describe(Id id);

    int Get(Id id);
    // Sets a value (clamped to its range; Volume to VolumeLimit too, and VolumeLimit lowers Volume; Language
    // to a language there is, else unchanged) and applies it at once. Returns the value now set. Written to
    // the flash about a second after the last change. Any task.
    int Set(Id id, int value);
    // Every setting to its default but the language (as the Launcher's own reset keeps it).
    void Reset();
    // The value came from the flash or was set since the start (not only the default).
    bool Stored(Id id);
    // What changed is written now; waits for it (at most about a second). For a firmware that restarts the
    // board itself right after a Set (the system menu's restarts, and esp_restart's shutdown, do it already).
    bool Flush();
    // Nothing waits to be written.
    bool Saved();

    // Told after a value changed (by the menu, USB, a Set or Reset), on the task that changed it: keep it
    // short. One listener (nullptr: none).
    using Listener = void (*)(Id id, int value, void* context);
    void SetListener(Listener listener, void* context);

}  // namespace CardenzaHAL::Settings
