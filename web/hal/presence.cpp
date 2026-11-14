// The community presence service (net/presence.h) on the web target: a page in a browser is no device
// the community can reach (no Wi-Fi, no Launcher, no device token), so the service is absent. The API is
// here so that a firmware calling it builds for the web as it is; it reports State::Off, as on a device
// built with CONFIG_CARDENZA_HAL_PRESENCE off.
#include "net/presence.h"

namespace CardenzaHAL::Presence {

void Start() {}

void Enable(bool) {}

bool Enabled()
{
    return false;
}

State GetState()
{
    return State::Off;
}

void SetHandoverHook(HandoverHook, void*) {}

}  // namespace CardenzaHAL::Presence
