#pragma once

#include <cstdint>

#include "net/net.h"

// Net inside the HAL: the firmware and the presence service (net/presence.h) share one Wi-Fi connection.
// Each says who it is; Wi-Fi comes down when neither holds it any more. The public functions in
// net/net.h are the firmware's (User::Firmware).
namespace CardenzaHAL::Net::Internal {

    enum class User : uint8_t { Firmware = 1, Presence = 2 };

    // As Net::Connect, for `user`. Connected already (for the other user): joins in at once. Wi-Fi saves
    // power between beacons (modem sleep) while only the presence service holds it, and runs at full
    // power while the firmware does. `quiet`: its routine lines (trying, joined, heap) at debug level.
    ConnectStatus Connect(User user, const LauncherSettings& settings, ConnectReport& report, uint32_t timeoutMs,
                          CancelCheck cancelled, void* context, bool quiet);
    // As ReadLauncherSettings; `quiet`: its routine line at debug level.
    SettingsResult ReadSettings(LauncherSettings& settings, bool quiet);
    // `user` lets go; Wi-Fi comes down when nobody holds it. The presence service's kept client (its TLS
    // session, Send) goes with it.
    void Disconnect(User user);
    bool Holds(User user);
    // As Net::Send, counted per user. The presence service's successes are logged at debug level only, and
    // its requests go through one kept client whose TLS session is resumed from one request to the next
    // (with CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS).
    SendStatus Send(User user, const Request& request, Response& response, Progress progress, CancelCheck cancelled,
                    void* context);
    // A request of `user` is under way.
    bool Sending(User user);

    // The Launcher's account binding and Settings > Account > Receive firmware, read as
    // ReadLauncherSettings reads them (read-only, nothing written), without the saved networks.
    SettingsResult ReadAccount(LauncherAccount& account, bool& receiveFirmware);

    // The handover: a command the presence service took, written for the Launcher to carry on (NVS
    // namespace l_pending, 203-launcher docs/SHARED_SETTINGS.md). The only write Net ever makes to the
    // shared NVS, right before restarting into the Launcher. The namespace is replaced as a whole. False
    // when NVS cannot be opened read-write without erasing it (never erased) or a write fails.
    struct PendingCommand {
        const char* command = nullptr;  // the command's id
        const char* action = nullptr;   // "install" or "install_start"
        const char* unlock = nullptr;   // "cfu_..." or empty
        const char* api = nullptr;      // the community the command came from
    };
    bool WritePending(const PendingCommand& pending);
    // Takes it back (the community said the command is gone after all).
    bool ClearPending();

}  // namespace CardenzaHAL::Net::Internal
