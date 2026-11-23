#pragma once

// The test's steps: where the person stands and what sound they make. Pure (no ESP-IDF): the firmware, the
// host test and the screenshots share it.

#include <cstdint>

namespace MicCheck {

enum class Sound : uint8_t
{
    Quiet,   // nobody makes a sound: the noise floor
    Soft,    // counting softly, as in a whisper
    Normal,  // counting at a conversation's level
    Loud,    // counting as if calling someone across the room
    Clap,    // a clap a second
    Music,   // music from a phone's speaker, at the level one listens at
};

struct Step
{
    const char* distance;  // as the log and the files say it: "10cm", "30cm", "1m", "2m", "-" (no distance)
    Sound sound;
};

inline constexpr Step kPlan[] = {
    {"-", Sound::Quiet},
    {"10cm", Sound::Soft},  {"10cm", Sound::Normal}, {"10cm", Sound::Loud},
    {"30cm", Sound::Soft},  {"30cm", Sound::Normal}, {"30cm", Sound::Loud},
    {"1m", Sound::Soft},    {"1m", Sound::Normal},   {"1m", Sound::Loud},
    {"2m", Sound::Soft},    {"2m", Sound::Normal},   {"2m", Sound::Loud},
    {"30cm", Sound::Clap},  {"1m", Sound::Clap},
    {"30cm", Sound::Music},
};
inline constexpr int kStepCount = static_cast<int>(sizeof(kPlan) / sizeof(kPlan[0]));

// The log's and the files' name of a sound.
inline const char* SoundKey(Sound sound)
{
    switch (sound)
    {
    case Sound::Quiet: return "quiet";
    case Sound::Soft: return "soft";
    case Sound::Normal: return "normal";
    case Sound::Loud: return "loud";
    case Sound::Clap: return "clap";
    case Sound::Music: return "music";
    }
    return "?";
}

inline bool IsSpeech(Sound sound) { return sound == Sound::Soft || sound == Sound::Normal || sound == Sound::Loud; }

}  // namespace MicCheck
