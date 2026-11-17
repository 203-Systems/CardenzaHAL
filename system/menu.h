#pragma once

#include <cstdint>

// The system menu: in every firmware on CardenzaHAL, holding the side key (HOME, GPIO0) for a second
// opens a menu drawn like the 203 Launcher's lists, over whatever the firmware shows:
//
//   Continue           back to the firmware (also ESC or del)
//   Back to Launcher   the Launcher's home screen, whatever its settings (the restart ticket,
//                      system/launcher.h)
//   Restart            this firmware again (under the Launcher: its boot ticket, no countdown)
//   Turn off           deep sleep; HOME turns the board on again (under the Launcher: through it, as
//                      after a power-on, as its own Turn off does)
//   Download mode      the ROM's download mode, to flash over USB (esptool)
//
// Keys, as in the Launcher: ; and , up, . and / down, ENTER or a tap on HOME choose, ESC or del go back;
// they are lit while the menu is open, every other key light is off. The words follow the Launcher's
// language (its NVS value launcher/lang, read without writing anything).
//
// While the menu is open the firmware keeps running (its tasks, its sound), but the HAL holds back what
// would disturb the menu: Display::Present* send nothing (they return true; the frame after the menu
// closes goes whole, so the firmware's screen comes back by itself), Keyboard::updateKeyList reports no
// key (keys still down when it closes stay hidden until they are released), and the key lights show the
// menu's keys (setLED* keep the firmware's levels, restored when it closes). A firmware that draws through
// the panel handles itself (Display::GetPanel, Present() without a framebuffer) should stop while IsOpen().
//
// CardenzaHAL::Init starts it (CONFIG_CARDENZA_HAL_SYSTEM_MENU, on by default): a task of 2.5 KB that
// reads the side key every 20 ms. A firmware that needs HOME's long press for itself turns it off with
// Enable(false) (or the Kconfig option); short presses of HOME are always the firmware's.
namespace CardenzaHAL::SystemMenu {

    enum class Action : uint8_t { Launcher, Restart, PowerOff, Download };

    // Called by CardenzaHAL::Init; once, however often it is called.
    void Start();
    void Enable(bool enabled);
    bool Enabled();
    // The menu is on the screen.
    bool IsOpen();

    // Told when the menu opens (true) and closes (false), on the menu's task: keep it short (set a flag).
    // A firmware that draws only what changed can redraw everything after it closes.
    using Listener = void (*)(bool open, void* context);
    void SetListener(Listener listener, void* context);

    // Called on the menu's task just before the board restarts, sleeps or enters the download mode: save
    // what must survive (an open file, settings). The board leaves as soon as it returns.
    using LeaveHook = void (*)(Action action, void* context);
    void SetLeaveHook(LeaveHook hook, void* context);

}  // namespace CardenzaHAL::SystemMenu
