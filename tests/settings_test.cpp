// system/settings_logic.h: the board's settings (system/settings.h). Their ranges and defaults, the volume
// limit, the volume's curve and the codec's level, the values as the flash holds them
// (the Launcher's own keys for the screen and the language), and when changes are written.
#include <cassert>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "display/backlight_brightness.h"
#include "system/settings_logic.h"

using namespace CardenzaHAL::Settings;
using namespace CardenzaHAL::Settings::detail;

namespace {

void TestDescribe()
{
    const char* const keys[] = {"volume", "volume_limit", "screen_brightness", "key_brightness", "language", "webusb_landing", "webusb_seen",
                                "screenshot_upload"};
    static_assert(sizeof(keys) / sizeof(keys[0]) == kCount, "a key for every setting");
    for (int i = 0; i < kCount; ++i)
    {
        assert(std::strcmp(kInfo[i].key, keys[i]) == 0);
        assert(kInfo[i].min <= kInfo[i].defaultValue && kInfo[i].defaultValue <= kInfo[i].max && kInfo[i].step > 0);
        Id id;
        assert(Find(keys[i], std::strlen(keys[i]), id) && id == IdAt(i));
    }
    Id id;
    assert(!Find("volumes", 7, id) && !Find("volume", 5, id) && !Find("", 0, id));
    const Values defaults = Defaults();
    assert(defaults[Index(Id::Volume)] == 70 && defaults[Index(Id::VolumeLimit)] == 100);
    assert(defaults[Index(Id::ScreenBrightness)] == 100 && defaults[Index(Id::KeyBrightness)] == 15);
    assert(defaults[Index(Id::Language)] == 0);
    // The WebUSB landing page: the first time only, not seen yet.
    assert(defaults[Index(Id::WebUsbLanding)] == kLandingFirstTime && defaults[Index(Id::WebUsbSeen)] == 0);
    // The microphone's gain is the firmware's (mic/gain.h), not a setting (2026-11-23).
    assert(!Find("mic_gain", 8, id));
    // The Launcher's brightness is never under 10 (its savedBrightness).
    assert(kInfo[Index(Id::ScreenBrightness)].min == 10 && kInfo[Index(Id::ScreenBrightness)].step == 5);
}

void TestClampAndLimit()
{
    Values v = Defaults();
    assert(Apply(v, Id::Volume, 150) == (1u << Index(Id::Volume)) && v[Index(Id::Volume)] == 100);
    assert(Apply(v, Id::Volume, -3) != 0 && v[Index(Id::Volume)] == 0);
    assert(Apply(v, Id::Volume, 0) == 0);  // nothing changed
    // The limit pulls the volume down with it, and the volume stays under it.
    v[Index(Id::Volume)] = 80;
    const uint32_t both = Apply(v, Id::VolumeLimit, 50);
    assert(both == ((1u << Index(Id::VolumeLimit)) | (1u << Index(Id::Volume))));
    assert(v[Index(Id::Volume)] == 50 && v[Index(Id::VolumeLimit)] == 50);
    Apply(v, Id::Volume, 90);
    assert(v[Index(Id::Volume)] == 50);
    assert(Apply(v, Id::VolumeLimit, 100) == (1u << Index(Id::VolumeLimit)) && v[Index(Id::Volume)] == 50);
    Apply(v, Id::VolumeLimit, 2);
    assert(v[Index(Id::VolumeLimit)] == 10 && v[Index(Id::Volume)] == 10);
    // Ranges.
    Apply(v, Id::ScreenBrightness, 3);
    assert(v[Index(Id::ScreenBrightness)] == 10);
    Apply(v, Id::KeyBrightness, 99);
    assert(v[Index(Id::KeyBrightness)] == 15);
    // Languages: the Launcher's codes; 2 (Traditional Chinese until 2026-11) is 1; anything else changes nothing.
    Apply(v, Id::Language, 3);
    assert(v[Index(Id::Language)] == 3);
    Apply(v, Id::Language, 2);
    assert(v[Index(Id::Language)] == 1);
    assert(Apply(v, Id::Language, 7) == 0 && v[Index(Id::Language)] == 1);
    assert(Apply(v, Id::Language, -1) == 0);
    for (int i = 0; i < kLanguageCount; ++i) assert(LanguageIndex(kLanguageCodes[i]) == i);
    assert(LanguageIndex(2) == -1 && LanguageFromStored(2) == 1 && LanguageFromStored(9) == 0 && LanguageFromStored(5) == 5);
    // Reset: every one to its default but the language.
    const uint32_t reset = ApplyReset(v);
    assert(!(reset & (1u << Index(Id::Language))) && v[Index(Id::Language)] == 1);
    for (int i = 0; i < kCount; ++i) assert(i == Index(Id::Language) || v[i] == kInfo[i].defaultValue);
    // Screenshots go up by default (screenshot/screenshot.h); off is 0, and nothing else.
    assert(v[Index(Id::ScreenshotUpload)] == 1);
    assert(Apply(v, Id::ScreenshotUpload, 0) != 0 && v[Index(Id::ScreenshotUpload)] == 0);
    assert(Apply(v, Id::ScreenshotUpload, 7) != 0 && v[Index(Id::ScreenshotUpload)] == 1);
    assert(ApplyReset(v) == 0);
    assert(ApplyReset(v) == 0);
}

void TestVolumeCurve()
{
    // -60 (1 - v / 100)^1.5 dB: CardenzaSynth's curve until 2026-11-23, now every firmware's.
    assert(std::isinf(VolumeDb(0)) && VolumeDb(0) < 0);
    assert(VolumeDb(100) == 0.0f);
    assert(std::fabs(VolumeDb(50) + 21.21f) < 0.05f);
    assert(std::fabs(VolumeDb(70) + 9.86f) < 0.05f);
    assert(std::fabs(VolumeDb(1) + 59.1f) < 0.1f);
    for (int v = 1; v < 100; ++v) assert(VolumeDb(v) < VolumeDb(v + 1));
    // The codec's register: 0 silent, 191 0 dB, half a decibel a step; never silent above 0.
    assert(CodecRegister(0) == 0 && CodecRegister(100) == 191);
    assert(CodecRegister(1) >= 1);
    assert(CodecRegister(70) == 171);  // -9.86 dB: 191 - 19.7
    for (int v = 1; v < 100; ++v) assert(CodecRegister(v) <= CodecRegister(v + 1));
    // Audio::SetVolume's percent (linear in dB over the codec's range), as the synth computed it.
    assert(CodecPercent(0) == 0 && CodecPercent(100) == 100);
    assert(CodecPercent(70) == 90);  // 100 - 9.86 * 200 / 191
    assert(CodecPercent(1) >= 1 && CodecPercent(50) == 78);
}

void TestBacklight()
{
    // The Launcher's duty curve (display/backlight_brightness.h): 20 % of the time at 10, always on at 100.
    using CardenzaHAL::Display::detail::LauncherBacklightDuty;
    // (Rounded here: the HAL's 12 bits; the Launcher's 10 truncate, 203.)
    assert(std::abs(LauncherBacklightDuty(10, 1023) - 203) <= 1 && LauncherBacklightDuty(100, 1023) == 1023 &&
           LauncherBacklightDuty(0, 1023) == 0);
    assert(LauncherBacklightDuty(100, 4095) == 4095);
    for (int p = 10; p < 100; ++p) assert(LauncherBacklightDuty(p, 4095) < LauncherBacklightDuty(p + 1, 4095));
}

void TestStored()
{
    // The screen's brightness and the language are the Launcher's own keys; the rest are the HAL's.
    assert(std::strcmp(kStored[Index(Id::ScreenBrightness)].nameSpace, "launcher") == 0 &&
           std::strcmp(kStored[Index(Id::ScreenBrightness)].key, "bright") == 0 &&
           kStored[Index(Id::ScreenBrightness)].type == kTypeI32);
    assert(std::strcmp(kStored[Index(Id::Language)].nameSpace, "launcher") == 0 &&
           std::strcmp(kStored[Index(Id::Language)].key, "lang") == 0 && kStored[Index(Id::Language)].type == kTypeU8);
    for (Id id : {Id::Volume, Id::VolumeLimit, Id::KeyBrightness, Id::ScreenshotUpload})
    {
        assert(std::strcmp(kStored[Index(id)].nameSpace, "hal_system") == 0);
        assert(std::strlen(kStored[Index(id)].key) <= 15);
    }
    assert(std::strlen(kNamespace) <= 15);
    // Read as the Launcher reads its own: brightness 0 is 100, never under 10.
    assert(FromStored(Id::ScreenBrightness, 0) == 100 && FromStored(Id::ScreenBrightness, 3) == 10 &&
           FromStored(Id::ScreenBrightness, 400) == 100 && FromStored(Id::ScreenBrightness, 55) == 55);
    assert(FromStored(Id::Language, 2) == 1 && FromStored(Id::Language, 77) == 0);
    // Anything else out of its range: the default.
    assert(FromStored(Id::Volume, 120) == 70 && FromStored(Id::Volume, 30) == 30);
    assert(FromStored(Id::KeyBrightness, 16) == 15 && FromStored(Id::KeyBrightness, 4) == 4);
}

void TestWritePlan()
{
    WritePlan plan;
    Values flash = Defaults();
    flash[Index(Id::Volume)] = 40;
    plan.Loaded(flash, 1u << Index(Id::Volume));
    assert(plan.Stored(Id::Volume) && !plan.Stored(Id::VolumeLimit));
    Values v = flash;
    assert(plan.Dirty(v) == 0 && !plan.Due(v, 0, false, true));  // nothing to write, even when asked
    // A change: written once it has rested a second, never while the menu is open, at once when asked.
    v[Index(Id::Volume)] = 50;
    plan.Changed(1000);
    assert(plan.Dirty(v) == (1u << Index(Id::Volume)));
    assert(!plan.Due(v, 1000 + kSettleMs - 1, false, false));
    assert(plan.Due(v, 1000 + kSettleMs, false, false));
    assert(!plan.Due(v, 5000, true, false));
    assert(plan.Due(v, 1001, true, true));
    // Another change starts the wait again.
    v[Index(Id::Volume)] = 52;
    plan.Changed(1900);
    assert(!plan.Due(v, 2500, false, false) && plan.Due(v, 2900, false, false));
    // Back to what the flash holds: nothing to write.
    v[Index(Id::Volume)] = 40;
    assert(plan.Dirty(v) == 0 && !plan.Due(v, 9000, false, true));
    // Written: only what was written is the flash's now.
    v[Index(Id::Volume)] = 60;
    v[Index(Id::VolumeLimit)] = 80;
    plan.Changed(10000);
    plan.Written(v, 1u << Index(Id::Volume));
    assert(plan.Dirty(v) == (1u << Index(Id::VolumeLimit)) && plan.Stored(Id::VolumeLimit) == false);
    plan.Written(v, 1u << Index(Id::VolumeLimit));
    assert(plan.Dirty(v) == 0 && plan.Stored(Id::VolumeLimit));
    // Time wraps (a uint32_t of milliseconds).
    v[Index(Id::KeyBrightness)] = 3;
    plan.Changed(0xFFFFFF00u);
    assert(!plan.Due(v, 0xFFFFFF00u + 10, false, false) && plan.Due(v, 0xFFFFFF00u + kSettleMs, false, false));
    plan.MarkSet(Id::Language);
    assert(plan.Stored(Id::Language));
}

}  // namespace

int main()
{
    TestDescribe();
    TestClampAndLimit();
    TestVolumeCurve();
    TestBacklight();
    TestStored();
    TestWritePlan();
    std::puts("settings: ok");
    return 0;
}
