#pragma once

#include <cstdint>

namespace CardenzaHAL::IR {

    bool Init();
    void Send(uint8_t addr, uint8_t cmd);

}  // namespace CardenzaHAL::IR
