#pragma once

#include <driver/i2c_master.h>

namespace CardenzaHAL::System {

    bool Init();
    void EnterDeepSleep();
    bool GetFuncButton();
    i2c_master_bus_handle_t GetI2CBus();

}  // namespace CardenzaHAL::System
