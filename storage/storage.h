#pragma once

#include <cstdint>

namespace CardenzaHAL::Storage {
    bool Init();
    bool IsMounted();
    uint64_t GetCapacity();
}
