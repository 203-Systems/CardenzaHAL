#pragma once

// The side key's task (system/menu.cpp): it reads HOME (and space) every 10 ms, opens the system menu on a short
// press, takes screenshots on HOME + space (screenshot/screenshot.h) and draws their effect. Internal to the HAL.
namespace CardenzaHAL::System::detail {

    // Starts the task (once): SystemMenu::Start (the menu) and Screenshot::Start (HOME + space) both call it; the
    // menu opens only when SystemMenu::Start ran and SystemMenu::Enable has not turned it off.
    void StartKeyTask();

    // HOME is the system's (the menu is on): System::GetHomeButton reports it released to the firmware.
    void SetHomeForSystem(bool system);

}  // namespace CardenzaHAL::System::detail
