#pragma once

#include <cstdint>
#include <driver/i2s_std.h>

namespace CardenzaHAL::Audio {
    bool Init();
    i2s_chan_handle_t GetTxChannel();

    bool SetVolume(uint8_t volume);
    uint8_t GetVolume();
}  // namespace CardenzaHAL::Audio
