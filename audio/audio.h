#pragma once

#include <cstdint>
#include <driver/i2s_std.h>

namespace CardenzaHAL::Audio {
    // Configure before enabling TX. Mono takes one sample per frame and copies
    // it to both wire slots; stereo takes interleaved L/R samples.
    // Switching modes requires a disabled channel. Repeating the same mode is safe.
    bool Init(i2s_slot_mode_t mode = I2S_SLOT_MODE_MONO);
    i2s_chan_handle_t GetTxChannel();

    bool SetVolume(uint8_t volume);
    uint8_t GetVolume();
}  // namespace CardenzaHAL::Audio
