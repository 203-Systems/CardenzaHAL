#pragma once

#include <driver/i2s_pdm.h>

namespace CardenzaHAL::Mic {

    // PDM CLK shares GPIO43 with speaker WS. The consumer must standby the
    // codec before Init/InputChannel and restore TX GPIOs after stopping RX.
    bool Init();
    i2s_chan_handle_t InputChannel();
    // Reconnect PDM CLK after speaker use; RX must be disabled.
    bool RestoreInputPins();

}  // namespace CardenzaHAL::Mic
