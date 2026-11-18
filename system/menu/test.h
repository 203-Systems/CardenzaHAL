#pragma once

#include "system/menu/logic.h"

// For on-board tests of the system menu (system/menu.h) only: a key as if pressed once while the menu is
// open (the next poll takes it), and what the menu is doing. The side key itself is tested by driving GPIO0
// low, as the button does.
namespace CardenzaHAL::SystemMenu::detail {

    void InjectKey(Key key);
    // Whether the menu has the firmware's picture (its backdrop is made from it), and the menu task's stack
    // never used so far (bytes).
    bool HasFirmwarePicture();
    uint32_t StackFree();

}  // namespace CardenzaHAL::SystemMenu::detail
