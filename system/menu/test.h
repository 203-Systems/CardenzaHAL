#pragma once

#include "system/menu/logic.h"

// For on-board tests of the system menu (system/menu.h) only: a key as if pressed once while the menu is
// open (the next poll takes it). The side key itself is tested by driving GPIO0 low, as the button does.
namespace CardenzaHAL::SystemMenu::detail {

    void InjectKey(Key key);

}  // namespace CardenzaHAL::SystemMenu::detail
