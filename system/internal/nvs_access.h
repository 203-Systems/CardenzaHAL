#pragma once

#include <mutex>

// The NVS partition opened read-write by the HAL (system/settings.cpp, Net's handover in net/net.cpp): one
// at a time, since each may initialise the partition and deinitialise it after. Internal to the HAL.
namespace CardenzaHAL::System::detail {

    std::mutex& NvsWriteMutex();

}  // namespace CardenzaHAL::System::detail
