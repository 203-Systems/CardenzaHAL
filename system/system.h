#pragma once

#include <driver/i2c_master.h>

namespace CardenzaHAL::System {

    bool Init();
    // Releases the I2C bus (and the HAL's codec on it) right before the caller enters deep sleep. A
    // firmware removes its own devices from GetI2CBus() first, or the bus stays.
    void EnterDeepSleep();
    // HOME, the side key (GPIO0, active low): true while it is down, except while the system menu has it
    // (system/menu.h). The side key is called HOME everywhere (2026-11-18); fn is the keyboard's left column's third key (29; ctrl is the bottom-left one).
    bool GetFuncButton();  // its old name, still the one defined (firmware and their host fakes use it)
    inline bool GetHomeButton() { return GetFuncButton(); }
    i2c_master_bus_handle_t GetI2CBus();

}  // namespace CardenzaHAL::System
