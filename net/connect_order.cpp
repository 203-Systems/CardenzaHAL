// Pure C++ (no ESP-IDF): the host tests compile it too.
#include "net/connect_order.h"

#include <cstring>

namespace CardenzaHAL::Net::ConnectOrder {

namespace {

bool SameSsid(const char* a, const char* b)
{
    return a[0] != 0 && std::strncmp(a, b, 32) == 0;
}

std::size_t Count(const LauncherSettings& settings)
{
    return settings.networkCount < kMaxSavedNetworks ? settings.networkCount : kMaxSavedNetworks;
}

}  // namespace

bool LastNetwork(const LauncherSettings& settings, Attempt& attempt)
{
    attempt = Attempt{};
    // Channels 1..14 (2.4 GHz); the Launcher stores 0 when it does not know.
    if (settings.lastSsid[0] == 0 || settings.lastChannel == 0 || settings.lastChannel > 14) return false;
    for (std::size_t i = 0; i < Count(settings); ++i)
    {
        if (SameSsid(settings.networks[i].ssid, settings.lastSsid))
        {
            attempt.network = static_cast<uint8_t>(i);
            attempt.channel = settings.lastChannel;
            return true;
        }
    }
    return false;
}

bool WorthScanning(const LauncherSettings& settings, bool triedLast)
{
    return Count(settings) > (triedLast ? 1u : 0u);
}

std::size_t AfterScan(const LauncherSettings& settings, const Seen* seen, std::size_t seenCount, int skip,
                      Attempt* out, std::size_t capacity)
{
    if (seen == nullptr || out == nullptr) return 0;
    // For each saved network, its strongest access point in the scan.
    int strongest[kMaxSavedNetworks];
    for (int& index : strongest) index = -1;
    for (std::size_t s = 0; s < seenCount; ++s)
    {
        for (std::size_t n = 0; n < Count(settings); ++n)
        {
            if (static_cast<int>(n) == skip || !SameSsid(seen[s].ssid, settings.networks[n].ssid)) continue;
            if (strongest[n] < 0 || seen[s].rssi > seen[strongest[n]].rssi) strongest[n] = static_cast<int>(s);
        }
    }
    // Strongest first; equal signals keep the order of the saved list.
    std::size_t written = 0;
    bool used[kMaxSavedNetworks] = {};
    while (written < capacity)
    {
        int best = -1;
        for (std::size_t n = 0; n < Count(settings); ++n)
        {
            if (used[n] || strongest[n] < 0) continue;
            if (best < 0 || seen[strongest[n]].rssi > seen[strongest[best]].rssi) best = static_cast<int>(n);
        }
        if (best < 0) break;
        used[best] = true;
        out[written].network = static_cast<uint8_t>(best);
        const uint8_t channel = seen[strongest[best]].channel;
        out[written].channel = channel <= 14 ? channel : 0;
        ++written;
    }
    return written;
}

}  // namespace CardenzaHAL::Net::ConnectOrder
