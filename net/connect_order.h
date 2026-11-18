#pragma once

#include <cstddef>
#include <cstdint>

#include "net/launcher_settings.h"

// In which order Net::Connect tries the Launcher's networks, as the Launcher's own start-up connection
// does (203-launcher src/onlineLauncher.cpp): the network it joined last first, on its channel and
// without a scan; then, after a scan, the other saved networks that are in range, strongest first.
// Pure (no ESP-IDF): host tests and simulators include it.
namespace CardenzaHAL::Net::ConnectOrder {

    // A network a scan found.
    struct Seen {
        char ssid[33] = {};
        int8_t rssi = 0;
        uint8_t channel = 0;
    };

    // One try: settings.networks[network] on `channel` (0: every channel).
    struct Attempt {
        uint8_t network = 0;
        uint8_t channel = 0;
    };

    // The last network on its channel, when the Launcher knows both and the network is still saved.
    bool LastNetwork(const LauncherSettings& settings, Attempt& attempt);

    // Whether a scan can still find something to try, after `triedLast` (LastNetwork's try) failed.
    bool WorthScanning(const LauncherSettings& settings, bool triedLast);

    // The saved networks among `seen`, each once on the channel of its strongest access point,
    // strongest first, leaving out `skip` (the index LastNetwork gave, or -1). Hidden networks (no
    // SSID) never match. Returns how many it wrote to `out` (at most `capacity`).
    std::size_t AfterScan(const LauncherSettings& settings, const Seen* seen, std::size_t seenCount, int skip,
                          Attempt* out, std::size_t capacity);

}  // namespace CardenzaHAL::Net::ConnectOrder
