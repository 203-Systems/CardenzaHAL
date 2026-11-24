#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "system/settings.h"

// The system settings' decisions (system/settings.h), pure: no ESP-IDF, so the host tests check them
// (tests/settings_test.cpp). system/settings.cpp runs them on the device, web/hal/settings.cpp in a browser.
namespace CardenzaHAL::Settings::detail {

    using Values = std::array<int, kCount>;

    inline constexpr int Index(Id id) { return static_cast<int>(id); }
    inline constexpr Id IdAt(int index) { return static_cast<Id>(index); }

    // kInfo[i] describes Id i.
    extern const Info kInfo[kCount];
    Values Defaults();
    // The id named `key` (Info::key); false if none is.
    bool Find(const char* key, std::size_t length, Id& id);

    // ---- Languages: the 203 Launcher's (its NVS value launcher/lang; docs/SHARED_SETTINGS.md there) -----
    inline constexpr int kLanguageCount = 5;
    // In the Launcher's order of the picker: English, Simplified Chinese, Japanese, Indonesian, Vietnamese.
    inline constexpr uint8_t kLanguageCodes[kLanguageCount] = {0, 1, 3, 4, 5};
    inline constexpr const char* kLanguageTags[kLanguageCount] = {"en", "zh-Hans", "ja", "id", "vi"};
    // A stored code as the Launcher reads it: 2 (Traditional Chinese until 2026-11) is 1, anything unknown 0.
    int LanguageFromStored(int code);
    // The code's place in kLanguageCodes; -1 for a code that is none (2 included: Set maps it first).
    int LanguageIndex(int code);

    // ---- Values ------------------------------------------------------------------------------------------
    // `value` for `id` as Set takes it, given the others in `now`: in its range; Volume under VolumeLimit;
    // a Language that is none keeps the one in `now`.
    int Clamp(Id id, int value, const Values& now);
    // Sets `id` in `values` (Clamp), and VolumeLimit pulls Volume down with it. Returns a bit per id that
    // changed (1 << Index).
    uint32_t Apply(Values& values, Id id, int value);
    // Every id to its default but Language; the ids that changed.
    uint32_t ApplyReset(Values& values);

    // ---- The output volume ---------------------------------------------------------------------------
    // The level of volume `v` in dB: -60 (1 - v / 100)^1.5 (0 at 100, about -59 at 1); 0 is silent.
    float VolumeDb(int volume);
    // The codec's volume register (ES8156, 0.5 dB a step from -95.5 dB at 0 to 0 dB at 191) for `v`:
    // 0 (silent) at 0, at least 1 above.
    uint8_t CodecRegister(int volume);
    // The same as Audio::SetVolume's percent (linear in dB over the codec's range: 191 registers in 100).
    uint8_t CodecPercent(int volume);

    // ---- In the flash (docs/SETTINGS.md) --------------------------------------------------------------
    // Where each setting is in the NVS partition: namespace, key and type (NVS's: 0x01 u8, 0x14 i32).
    struct Stored {
        const char* nameSpace;
        const char* key;
        uint8_t type;
    };
    inline constexpr char kNamespace[] = "hal_system";
    inline constexpr char kLauncherNamespace[] = "launcher";
    inline constexpr uint8_t kTypeU8 = 0x01;
    inline constexpr uint8_t kTypeI32 = 0x14;
    extern const Stored kStored[kCount];
    // A value read from the flash as the setting (the Launcher's brightness never under 10 and 0 meaning
    // 100, its language codes; out of range: the default).
    int FromStored(Id id, int32_t stored);

    // When to write. Changes are written once they have rested kSettleMs, never while the system menu is
    // open (it writes when it closes: Flush), and only what differs from what the flash holds.
    inline constexpr uint32_t kSettleMs = 1000;
    class WritePlan {
      public:
        // The values the flash holds (read at the start; `found`: a bit per id that was there).
        void Loaded(const Values& values, uint32_t found);
        // A change at `nowMs`.
        void Changed(uint32_t nowMs);
        // The ids that differ from the flash.
        uint32_t Dirty(const Values& values) const;
        // Time to write: something differs, it has rested, the menu is closed (or a flush was asked).
        bool Due(const Values& values, uint32_t nowMs, bool menuOpen, bool flush) const;
        // `written` (a bit per id) is in the flash now, as in `values`.
        void Written(const Values& values, uint32_t written);
        // The value is in the flash, or was set this run.
        bool Stored(Id id) const { return (stored_ >> Index(id)) & 1; }
        void MarkSet(Id id) { stored_ |= 1u << Index(id); }

      private:
        Values flash_{};
        uint32_t found_ = 0;   // in the flash
        uint32_t stored_ = 0;  // found_ and set since
        uint32_t changedAt_ = 0;
        bool changed_ = false;
    };

}  // namespace CardenzaHAL::Settings::detail
