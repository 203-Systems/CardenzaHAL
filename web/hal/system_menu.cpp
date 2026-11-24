// The system menu (system/menu.h) on the web target: a page has no side key and nothing to restart into
// (no Launcher, no deep sleep, no download mode), so the menu is absent. The API is here so that a
// firmware calling it builds for the web as it is.
#include "system/menu.h"

namespace CardenzaHAL::SystemMenu {

void Start() {}

void Enable(bool) {}

bool Enabled()
{
    return false;
}

bool IsOpen()
{
    return false;
}

void SetListener(Listener, void*) {}

void SetLeaveHook(LeaveHook, void*) {}

}  // namespace CardenzaHAL::SystemMenu

#include "system/internal/hooks.h"

// A crash loop has no Launcher to go back to on the web (the page restarts the firmware itself).
namespace CardenzaHAL::System::detail {
void CheckCrashLoop() {}
}  // namespace CardenzaHAL::System::detail
