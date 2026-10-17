#pragma once

#include <driver/i2s_pdm.h>

namespace CardenzaHAL::Mic {

    bool Init();
    i2s_chan_handle_t InputChannel();

}  // namespace CardenzaHAL::Mic
