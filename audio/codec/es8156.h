#pragma once

#include <cstdint>

namespace CardenzaHAL::Audio::Codec::ES8156 {

bool Init();
bool SetEnabled(bool enabled);
bool IsEnabled();

bool SetVolume(uint8_t volume);
uint8_t GetVolume();

}  // namespace CardenzaHAL::Audio::Codec::ES8156