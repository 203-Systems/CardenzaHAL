// CardenzaHAL::USB::Network on the web target: no USB cable, so no USB network (the page is the computer).
#include "usb/network.h"

namespace CardenzaHAL::USB::Network {

bool Start(Mode, const Options&) { return false; }
void Stop() {}
bool Started() { return false; }
State GetState() { return State{}; }
esp_netif_obj* Netif() { return nullptr; }
void SetListener(Listener) {}

const char* LinkName(Link link)
{
    switch (link)
    {
        case Link::Off: return "off";
        case Link::Waiting: return "waiting for the computer";
        case Link::Up: return "up";
        case Link::Ready: return "ready";
    }
    return "?";
}

}  // namespace CardenzaHAL::USB::Network
