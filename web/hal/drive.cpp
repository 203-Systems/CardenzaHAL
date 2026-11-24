// CardenzaHAL::USB::Drive (usb/drive.h) on the web target: the page has no computer to lend the card to, so a
// share fails (NoHost) and the card stays the firmware's.
#include "usb/drive.h"

namespace CardenzaHAL::USB::Drive {

namespace {
Status g_status;
Listener g_listener = nullptr;
}  // namespace

bool Share(uint32_t)
{
    g_status.state = State::Failed;
    g_status.reason = Reason::NoHost;
    if (g_listener != nullptr) g_listener(g_status);
    return false;
}

bool Stop(uint32_t) { return true; }
Status GetStatus() { return g_status; }
void SetListener(Listener listener) { g_listener = listener; }

}  // namespace CardenzaHAL::USB::Drive
