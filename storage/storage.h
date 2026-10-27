#pragma once

#include <cstdint>

namespace CardenzaHAL::Storage {
    bool Init();
    bool IsMounted();
    // Call after filesystem I/O fails, with the file closed. A healthy card
    // stays mounted (e.g. disk full); a lost card is retired on the next Init.
    bool CheckAfterIoError();
    uint64_t GetCapacity();
}
