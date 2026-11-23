#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/i2s_pdm.h>

namespace CardenzaHAL::Mic {

    // PDM CLK shares GPIO43 with speaker WS. The consumer must standby the
    // codec before Init/InputChannel and restore TX GPIOs after stopping RX.
    bool Init();
    i2s_chan_handle_t InputChannel();
    // Reconnect PDM CLK after speaker use; RX must be disabled.
    bool RestoreInputPins();

    // The board's microphone gain (Settings::Id::MicGain, the user's in the system menu): applied in place to
    // samples read from InputChannel() (16-bit mono), each times the gain, held at the int16 range (it
    // saturates, never wraps). The chip's PDM receiver has no gain of its own, so a firmware that records
    // calls this on what it reads. A multiply and a clamp a sample; nothing at 0 dB.
    void ApplyGain(int16_t* samples, std::size_t count);

}  // namespace CardenzaHAL::Mic
