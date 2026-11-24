#pragma once

#include <cstdint>

struct esp_netif_obj;  // ESP-IDF's esp_netif_t

// The board as a USB network adapter (CDC-NCM; Kconfig CONFIG_CARDENZA_HAL_USB_NETWORK, opt-in). The computer
// sees a wired network interface, with no driver to install on macOS, Linux and Windows 11 (Windows 10 has no NCM
// driver). Two modes:
//
//   Device: the board answers at 192.168.203.1, hands the computer an address from its own DHCP server
//     (192.168.203.2 onwards, no router and no DNS offered, so the computer's own internet and names are never
//     touched), and answers mDNS for <hostname>.local (cardenza.local by default). A firmware serves what it likes
//     there (HTTP, WebSocket, file transfer; ESP-IDF's esp_http_server works as on Wi-Fi) with no setup on the
//     computer.
//   Uplink: the board is a DHCP client on the link and uses the computer's shared internet (macOS Internet
//     Sharing, Windows ICS, a Linux NAT) when the person turned it on. It sits beside the HAL's Wi-Fi (net/net.h):
//     Wi-Fi stays the default route while it is up (unless Options::preferOverWifi), the USB link takes over when
//     it is not, and the DNS servers are always the default interface's (the Kconfig option selects ESP-IDF's
//     CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF).
//
// Start presents the class (the computer finds the device again: MIDI ports and audio streams of the host start
// over; its two IN endpoints come from what gives way, usb/usb.h), Stop removes it. The link follows the cable,
// the host's sleep and the classes changing: the listener hears it, and the log says each change and the address
// the computer leased ("usb-net: ..." lines). Costs while built: about 7 KB of static internal RAM (the NCM
// transfer blocks); while started: a task (3 KB stack), 6 KB of transmit slots (PSRAM when there is some), lwIP's
// pbufs per packet, and lwIP itself when the firmware had none.
namespace CardenzaHAL::USB::Network {

    enum class Mode : uint8_t { Device, Uplink };

    struct Options {
        // mDNS (<hostname>.local) and the DHCP client's name: lower case letters, digits and '-'.
        const char* hostname = "cardenza";
        // Uplink: the USB link is the default route even while Wi-Fi is up.
        bool preferOverWifi = false;
    };

    enum class Link : uint8_t {
        Off,      // not started, or the class could not be presented
        Waiting,  // presented, no computer has it (unplugged, asleep, not configured)
        Up,       // the computer has the link; addresses may still be coming (uplink: DHCP)
        Ready,    // the link has its addresses
    };

    struct State {
        Link link = Link::Off;
        Mode mode = Mode::Device;
        // IPv4 addresses, most significant byte first; 0.0.0.0 when unknown.
        uint8_t device[4] = {};   // the board's
        uint8_t netmask[4] = {};
        uint8_t gateway[4] = {};  // uplink: the computer's; device mode: none
        uint8_t dns[4] = {};      // uplink: the one the computer offered
        uint8_t host[4] = {};     // device mode: what the computer leased
        char hostname[33] = {};
        bool presented = false;   // the USB class is there
    };

    // From the firmware's task (it re-enumerates the device and waits for it): true when the class is presented
    // and the interface made. Start again switches the mode (or the options).
    bool Start(Mode mode = Mode::Device, const Options& options = Options());
    void Stop();
    bool Started();
    State GetState();
    // ESP-IDF's interface, for what the firmware does on it (esp_netif_get_ip_info, a socket bound to it); null
    // when not started.
    esp_netif_obj* Netif();
    // Called from the network's task at every change of the link or its addresses; keep it short, and do not
    // call Start or Stop from it (they wait for that task).
    using Listener = void (*)(const State& state);
    void SetListener(Listener listener);
    // "off", "waiting for the computer", "up", "ready".
    const char* LinkName(Link link);

}  // namespace CardenzaHAL::USB::Network
