#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/i2s_pdm.h>

#include "gain.h"

namespace CardenzaHAL::Mic {

    // PDM CLK shares GPIO43 with speaker WS. The consumer must standby the
    // codec before Init/InputChannel and restore TX GPIOs after stopping RX.
    bool Init();
    i2s_chan_handle_t InputChannel();
    // Reconnect PDM CLK after speaker use; RX must be disabled.
    bool RestoreInputPins();

    // The firmware's microphone gain, `gainDb` (held at kMinGainDb..kMaxGainDb, mic/gain.h: -12 to +36 dB),
    // applied in place to samples read from InputChannel() (16-bit mono): each times the gain, held at the int16
    // range (it saturates, never wraps). The chip's PDM receiver has no gain of its own, so a firmware that
    // records calls this on what it reads. The gain is the firmware's to keep, with its own settings (the user,
    // 2026-11-23: not a board setting). A multiply and a clamp a sample; nothing at 0 dB.
    void ApplyGain(int16_t* samples, std::size_t count, int gainDb);

}  // namespace CardenzaHAL::Mic
