#pragma once

#include <cstdint>

// The community presence service: the device stays reachable from the 203 community while any firmware
// on the HAL runs, not only in the 203 Launcher's menus. Firmware sent from a firmware's page on the
// website ("install" or "install and start") then reaches the device whatever it is running.
//
// CardenzaHAL::Init starts it when CONFIG_CARDENZA_HAL_PRESENCE is on (menuconfig: CardenzaHAL; on by
// default), CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY seconds later. It does something only when the
// Launcher left what it needs: a community binding (NVS l_account) for one of the community's hosts, a
// saved Wi-Fi network, and Settings > Account > Receive firmware on. Then a task of priority 1 on core 0
// keeps Wi-Fi up (through Net, sharing it with the firmware) and asks the community's inbox
// (GET <api>/launcher/inbox?from=firmware, with the device token) every 15 seconds, or as the answer says.
// That request is the device's presence: the website lets someone send firmware only while it comes.
//
// A command that comes is handed over to the Launcher: the firmware's hook may save its work (or refuse),
// the command is written to NVS l_pending (the only thing the HAL ever writes there), the community is
// told ("handover") and the device restarts into the Launcher, which downloads, checks, installs and
// starts it, reporting each step (203-launcher docs/SHARED_SETTINGS.md).
//
// It yields: after a failure it waits 5 s, doubling up to 5 minutes; it does not start a request while
// the speaker's DMA reports underruns, or without enough internal RAM, or while the firmware's own Net
// request is under way; a request during which the audio ran dry pauses it (60 s, doubling to 5
// minutes). The device token goes only over https to the community's own hosts (presence_logic.h), and
// a token the community refuses stops it until the next start.
//
// Cost while it runs: an 8 KB task stack and Wi-Fi connected (some 40-60 KB of internal RAM, Net's
// numbers; modem sleep between beacons), and per request one TLS connection (a handshake of some 5 KB
// on the air, 30-50 KB of TLS memory for a moment, PSRAM with CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC) and a
// 16 KB answer buffer (PSRAM when there is some), both given back after it. With the option on, Net is
// linked into the firmware (README.md: about 36 KB of static internal RAM and 610 KB of flash). With it
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
        Waiting,      // after a failure, or giving way to the audio or the RAM
        SignedOut,    // the community refused the device token: no more requests until the next start
        HandingOver,  // restarting into the Launcher with a command
    };

    // Starts the service (once; CardenzaHAL::Init calls it with the Kconfig option on). The first look at
    // the Launcher's settings comes after CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY seconds.
    void Start();

    // Switches the service on or off for this run (on by default). Off stops asking and lets go of
    // Wi-Fi (it comes down unless the firmware holds it through Net): call it before a firmware runs
    // Wi-Fi itself, or for a stretch where it wants every cycle. Switching off returns once the service
    // has stopped (within a network operation's timeout, 10 s, at worst; at once from the hook). Before
    // Init, Enable(false) keeps the service from starting at all.
    void Enable(bool enabled);
    bool Enabled();

    State GetState();

    // A command about to be handed over. The strings live until the hook returns.
    struct Handover {
        const char* command;       // the command's id
        bool start;                // install and start (else install only)
        const char* firmwareName;  // what is sent, for a message on the screen (may be empty)
        const char* version;
    };
    // Called on the service's task (priority 1, 8 KB of stack) before the device restarts into the
    // Launcher: save what must not be lost, stop the audio. Return false to refuse (the website is told
    // "Refused by the running firmware"). Without a hook the device restarts at once.
    using HandoverHook = bool (*)(const Handover& handover, void* context);
    void SetHandoverHook(HandoverHook hook, void* context);

}  // namespace CardenzaHAL::Presence
