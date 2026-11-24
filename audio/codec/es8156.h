#pragma once

#include <cstdint>

namespace CardenzaHAL::Audio::Codec::ES8156 {

bool Init();
bool SetEnabled(bool enabled);
bool IsEnabled();

bool SetVolume(uint8_t volume);
uint8_t GetVolume();

// Removes the codec from the I2C bus, so the bus can be deleted (System::EnterDeepSleep). Init (or any
// later access) binds it again.
void Release();

}  // namespace CardenzaHAL::Audio::Codec::ES8156