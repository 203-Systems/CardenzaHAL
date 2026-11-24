#pragma once

#include <cstdint>

// What the rest of the HAL needs from the system settings (system/settings.h). Internal to the HAL.
namespace CardenzaHAL::Settings::detail {

    // CardenzaHAL::Init: the values from the flash (nothing is written) and what can be set before the
    // drivers start (the codec's level, the backlight); once.
    void Load();
    // After the drivers started: everything applied (the key lights' master level among them).
    void ApplyAfterInit();
    // The system menu closed: what changed while it was open is written now.
    void MenuClosed();

}  // namespace CardenzaHAL::Settings::detail
