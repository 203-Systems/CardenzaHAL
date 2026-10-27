#pragma once

#include <cstdint>
#include <driver/i2s_std.h>

namespace CardenzaHAL::Audio {
    // Initialize I2S and the codec before enabling TX. HAL supplies temporary
    // zeroed-DMA clocks for codec startup and leaves a new TX disabled.
    // Mono takes one sample per frame and copies
    // it to both wire slots; stereo takes interleaved L/R samples.
    // Switching modes requires a disabled channel. A failed codec can retry.
    bool Init(i2s_slot_mode_t mode = I2S_SLOT_MODE_MONO);
    i2s_chan_handle_t GetTxChannel();

    bool IsCodecReady();
    // Put the codec in standby before lending GPIO43 to the PDM microphone.
    bool StandbyCodec();
    // Reconnect TX GPIOs after PDM use. The native TX channel must be disabled.
    bool RestoreOutputPins();

    // Sets/caches volume only; never performs codec initialization. False if it
    // is not ready or the write failed. Init() applies the cached level on retry.
    bool SetVolume(uint8_t volume);
    uint8_t GetVolume();
}  // namespace CardenzaHAL::Audio
