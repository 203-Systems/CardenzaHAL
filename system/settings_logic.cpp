// The system settings' decisions (system/settings_logic.h). Pure: built into the HAL, the web target and the
// host tests.
#include "system/settings_logic.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CardenzaHAL::Settings::detail {

// The menu's steps: 2 for the volumes (50 presses end to end), the Launcher's 5 for the backlight, every
// level of the key lights, a decibel of gain.
const Info kInfo[kCount] = {
    {"volume", 0, 100, 2, 70, ""},
    {"volume_limit", 10, 100, 2, 100, ""},
    {"screen_brightness", 10, 100, 5, 100, "%"},
    {"key_brightness", 0, 15, 1, 15, ""},
    {"mic_gain", -12, 36, 1, 0, "dB"},
    {"language", 0, 5, 1, 0, ""},
};

// The screen's brightness and the language are the Launcher's own keys (its src/settings.cpp: bright with
// lnvs::setInt, an i32; lang a u8), so the Launcher, the menu and every firmware show the same.
const Stored kStored[kCount] = {
    {kNamespace, "volume", kTypeU8},
    {kNamespace, "volume_max", kTypeU8},
    {kLauncherNamespace, "bright", kTypeI32},
    {kNamespace, "key_light", kTypeU8},
    {kNamespace, "mic_gain", kTypeI32},
    {kLauncherNamespace, "lang", kTypeU8},
};

Values Defaults()
{
    Values values{};
    for (int i = 0; i < kCount; ++i) values[i] = kInfo[i].defaultValue;
    return values;
}

bool Find(const char* key, std::size_t length, Id& id)
{
    for (int i = 0; i < kCount; ++i)
        if (std::strlen(kInfo[i].key) == length && std::memcmp(kInfo[i].key, key, length) == 0)
        {
            id = IdAt(i);
            return true;
        }
    return false;
}

int LanguageFromStored(int code)
{
    if (code == 2) return 1;  // Traditional Chinese until 2026-11: the Launcher reads it as Simplified
    return LanguageIndex(code) >= 0 ? code : 0;
}

int LanguageIndex(int code)
{
    for (int i = 0; i < kLanguageCount; ++i)
        if (kLanguageCodes[i] == code) return i;
    return -1;
}

int Clamp(Id id, int value, const Values& now)
{
    const Info& info = kInfo[Index(id)];
    switch (id)
    {
        case Id::Language:
            if (value == 2) value = 1;
            return LanguageIndex(value) >= 0 ? value : now[Index(Id::Language)];
        case Id::Volume:
            return std::clamp(value, info.min, std::min(info.max, now[Index(Id::VolumeLimit)]));
        default:
            return std::clamp(value, info.min, info.max);
    }
}

uint32_t Apply(Values& values, Id id, int value)
{
    const Values before = values;
    values[Index(id)] = Clamp(id, value, values);
    if (id == Id::VolumeLimit)
        values[Index(Id::Volume)] = std::min(values[Index(Id::Volume)], values[Index(Id::VolumeLimit)]);
    uint32_t changed = 0;
    for (int i = 0; i < kCount; ++i)
        if (values[i] != before[i]) changed |= 1u << i;
    return changed;
}

uint32_t ApplyReset(Values& values)
{
    const Values before = values;
    const int language = values[Index(Id::Language)];
    values = Defaults();
    values[Index(Id::Language)] = language;
    uint32_t changed = 0;
    for (int i = 0; i < kCount; ++i)
        if (values[i] != before[i]) changed |= 1u << i;
    return changed;
}

float VolumeDb(int volume)
{
    volume = std::clamp(volume, 0, 100);
    if (volume == 0) return -INFINITY;
    return -60.0f * std::pow(1.0f - static_cast<float>(volume) / 100.0f, 1.5f);
}

uint8_t CodecRegister(int volume)
{
    if (volume <= 0) return 0;
    const long reg = std::lround(191.0f + 2.0f * VolumeDb(volume));
    return static_cast<uint8_t>(std::clamp(reg, 1L, 191L));
}

uint8_t CodecPercent(int volume)
{
    // The register's percent, as CardenzaSynth's curve had it until 2026-11-23: a level of d dB is the HAL's
    // percent 100 + d * 200 / 191 (Audio::SetVolume maps percent to the register linearly).
    if (volume <= 0) return 0;
    const long percent = std::lround(100.0f + VolumeDb(volume) * 200.0f / 191.0f);
    return static_cast<uint8_t>(std::clamp(percent, 1L, 100L));
}

int32_t MicGainQ12(int db)
{
    db = std::clamp(db, kInfo[Index(Id::MicGain)].min, kInfo[Index(Id::MicGain)].max);
    return static_cast<int32_t>(std::lround(4096.0 * std::pow(10.0, db / 20.0)));
}

void ApplyMicGain(int16_t* samples, std::size_t count, int32_t gainQ12)
{
    if (gainQ12 == 4096) return;
    for (std::size_t i = 0; i < count; ++i)
    {
        const int32_t scaled = (static_cast<int32_t>(samples[i]) * gainQ12 + 2048) >> 12;
        samples[i] = static_cast<int16_t>(std::clamp<int32_t>(scaled, INT16_MIN, INT16_MAX));
    }
}

int FromStored(Id id, int32_t stored)
{
    const Info& info = kInfo[Index(id)];
    switch (id)
    {
        case Id::ScreenBrightness:
            // The Launcher's savedBrightness: never dark by a stored 0, never under 10.
            return stored == 0 ? 100 : std::clamp<int32_t>(stored, info.min, info.max);
        case Id::Language:
            return LanguageFromStored(static_cast<int>(stored));
        default:
            return stored >= info.min && stored <= info.max ? static_cast<int>(stored) : info.defaultValue;
    }
}

void WritePlan::Loaded(const Values& values, uint32_t found)
{
    flash_ = values;
    found_ = found;
    stored_ = found;
    changed_ = false;
}

void WritePlan::Changed(uint32_t nowMs)
{
    changed_ = true;
    changedAt_ = nowMs;
}

uint32_t WritePlan::Dirty(const Values& values) const
{
    uint32_t dirty = 0;
    for (int i = 0; i < kCount; ++i)
        if (values[i] != flash_[i]) dirty |= 1u << i;
    return dirty;
}

bool WritePlan::Due(const Values& values, uint32_t nowMs, bool menuOpen, bool flush) const
{
    if (Dirty(values) == 0) return false;
    if (flush) return true;
    if (menuOpen) return false;
    return !changed_ || nowMs - changedAt_ >= kSettleMs;
}

void WritePlan::Written(const Values& values, uint32_t written)
{
    for (int i = 0; i < kCount; ++i)
        if ((written >> i) & 1)
        {
            flash_[i] = values[i];
            found_ |= 1u << i;
            stored_ |= 1u << i;
        }
    if (Dirty(values) == 0) changed_ = false;
}

}  // namespace CardenzaHAL::Settings::detail
