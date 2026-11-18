#pragma once

#include <cstddef>
#include <cstdint>

// The community presence service: the device stays reachable from the 203 community while any firmware
// on the HAL runs, not only in the 203 Launcher's menus. Firmware sent from a firmware's page on the
// website ("install" or "install and start"), and the Launcher's own update ("update_launcher"), then
// reach the device whatever it is running, and the community can show what runs.
//
// CardenzaHAL::Init starts it when CONFIG_CARDENZA_HAL_PRESENCE is on (menuconfig: CardenzaHAL; on by
// default), CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY seconds later. It does something only when the
// Launcher left what it needs: a community binding (NVS l_account) for one of the community's hosts, a
// saved Wi-Fi network, and Settings > Account > Receive firmware on. Then a task of priority 1 on core 0
// keeps Wi-Fi up (through Net, sharing it with the firmware) and asks the community's inbox
// (GET <api>/launcher/inbox?from=firmware, with the device token, the firmware's name, version and image
// digest) every 15 seconds, or as the answer says. That request is the device's presence: the website
// lets someone send firmware only while it comes.
//
// A command that comes is handed over to the Launcher: the firmware's hook may save its work (or refuse),
// the command is written to NVS l_pending (the only thing the HAL ever writes there), the community is
// told ("handover") and the device restarts into the Launcher, which downloads, checks, installs and
// starts it, reporting each step (203-launcher docs/SHARED_SETTINGS.md).
//
// It yields (net/presence_logic.h, Decide): Wi-Fi comes up only while the firmware's activity is Idle and
// internal RAM leaves the firmware's floor (SetRamFloor) free beside what Wi-Fi takes; it is let go when
// the firmware says Quiet or its free internal RAM falls below the floor. No request starts while the
// speaker's DMA reports underruns, without enough RAM, or while the firmware's own Net request is under
// way; a request during which the audio ran dry pauses it (60 s, doubling to 5 minutes). After a failure
// it waits 5 s, doubling up to 5 minutes. Its TLS session is resumed from one request to the next (no
// certificates, no key exchange) when the firmware enables CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS. The
// device token goes only over https to the community's own hosts (presence_logic.h), and a token the
// community refuses stops it until the next start.
//
// Cost while it runs (README.md, "Community presence", has the measured numbers): an 8 KB task stack and
// Wi-Fi connected (internal RAM: what GetReport().wifiBytes says; modem sleep between beacons), and per
// request one TLS connection (resumed: about 2 KB on the air; a full handshake about 6 KB and 30-50 KB of
// TLS memory for a moment, PSRAM with CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC) and a 16 KB answer buffer (PSRAM
// when there is some), given back after it. With the option on, Net is linked into the firmware. With it
// off, nothing of it is linked and these functions do nothing.
//
// The web target has no network: there the service is absent (GetState() is Off).
namespace CardenzaHAL::Presence {

    enum class State : uint8_t {
        Off,          // not started (yet), or switched off
        Idle,         // nothing to do in this run: not bound, no saved network, Receive firmware off in
                      // the Launcher, a binding for a host it does not send the token to, or NVS unreadable
        Connecting,   // joining Wi-Fi
        Online,       // asking the inbox
        Waiting,      // after a failure, or giving way to the firmware (GetReport().waiting says why)
        SignedOut,    // the community refused the device token: no more requests until the next start
        HandingOver,  // restarting into the Launcher with a command
    };

    // Starts the service (once; CardenzaHAL::Init calls it with the Kconfig option on). The first look at
    // the Launcher's settings comes after CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY seconds.
    void Start();

    // Switches the service on or off for this run (on by default). Off stops asking and lets go of
    // Wi-Fi (it comes down unless the firmware holds it through Net): call it before a firmware runs
    // Wi-Fi itself. Switching off returns once the service has stopped (within a network operation's
    // timeout, 10 s, at worst; at once from the hook): call it from a task that may wait, never from the
    // audio or UI task (SetActivity(Quiet) is the call that returns at once). Before Init, Enable(false)
    // keeps the service from starting at all.
    void Enable(bool enabled);
    bool Enabled();

    // Whether the service goes online in this run: on, and the Launcher's settings have a binding to the
    // community the token may go to, a saved network and Receive firmware on. The first call reads the
    // settings (a few milliseconds of NVS reads, read-only); the answer is kept. A firmware may ask at its
    // start, to keep room for Wi-Fi. Without the service (option off, web): false.
    bool Planned();

    // What the firmware is doing, so the service picks its moments. Any task (not an interrupt), any
    // time: it returns at once (the service acts on its own task, a moment later). A firmware that never
    // calls it is Idle.
    enum class Activity : uint8_t {
        Idle,      // nothing time-critical: Wi-Fi may come up (the first time in a run its radio calibrates,
                   // which keeps core 0 busy for a while) and requests go on
        RealTime,  // time-critical work under way (sound): Wi-Fi does not come up; a connection that is up
                   // keeps asking (a few milliseconds of the network tasks per poll), giving way while the
                   // speaker's DMA runs late
        Quiet,     // the firmware wants its RAM and time back: the service lets go of Wi-Fi (its internal
                   // RAM returns, a request under way is cancelled) and asks nothing until the activity
                   // changes. The website shows the device offline 40 s after its last request.
    };
    // While the activity keeps Wi-Fi down (Quiet, or RealTime before it came up) the service's task ends and
    // its 8 KB stack returns to the firmware; the next change of the activity starts it again (it then
    // looks at once: no start delay, what it learnt kept).
    void SetActivity(Activity activity);
    Activity GetActivity();

    // The internal RAM the firmware keeps for itself (default 24 KB): Wi-Fi comes up only while that much
    // stays free beside what Wi-Fi takes (learnt when it first comes up; until then 40 KB, 32 KB with its own
    // allocations in PSRAM) and a request, and is let go
    // when free internal RAM falls below it. Any task, any time.
    void SetRamFloor(std::size_t bytes);

    // Why the service is waiting (State::Waiting).
    enum class Wait : uint8_t {
        None,
        Activity,         // the firmware's activity (Quiet, or RealTime while Wi-Fi is down)
        AudioLate,        // the speaker's DMA ran late
        Ram,              // not enough internal RAM above the firmware's floor
        FirmwareRequest,  // the firmware's own Net request is under way
        Failure,          // after a failed connection or request (backoff)
    };
    struct Report {
        State state = State::Off;
        Wait waiting = Wait::None;
        uint32_t asked = 0;      // inbox requests the community answered
        uint32_t failed = 0;     // connections and requests that failed
        uint32_t joins = 0;      // times Wi-Fi came up
        uint32_t letGo = 0;      // times Wi-Fi was let go for the firmware (Quiet, or its RAM floor)
        uint32_t resumed = 0;    // requests whose TLS session was resumed (no full handshake)
        uint32_t wifiBytes = 0;  // internal RAM Wi-Fi took when it last came up (0: not yet)
        uint32_t lastAskMs = 0;  // how long the last inbox request took
        uint32_t freeInternal = 0;  // internal RAM free at the service's last look
    };
    // A snapshot for a firmware's diagnostics (a stress test, a status page). Any task.
    Report GetReport();

    // A command about to be handed over. The strings live until the hook returns.
    struct Handover {
        const char* command;       // the command's id
        const char* action;        // "install", "install_start" or "update_launcher"
        bool start;                // install and start (install_start)
        bool launcherUpdate;       // the 203 Launcher updates itself (update_launcher)
        const char* firmwareName;  // what is sent, for a message on the screen (may be empty)
        const char* version;
    };
    // Called on the service's task (priority 1, 8 KB of stack) before the device restarts into the
    // Launcher: save what must not be lost, stop the audio. Return false to refuse (the website is told
    // "Refused by the running firmware"). Without a hook the device restarts at once.
    using HandoverHook = bool (*)(const Handover& handover, void* context);
    void SetHandoverHook(HandoverHook hook, void* context);

}  // namespace CardenzaHAL::Presence
