#pragma once

#include <cstdint>

// The system menu: in every firmware on CardenzaHAL, a short press of the side key (HOME, GPIO0), once it is
// let go, opens the 203 Launcher's list (drawn by the Launcher's own code, system/menu/launcher/; black, white and grey only, at
// the Launcher's own backlight setting), over whatever the firmware shows. The firmware's picture goes dark
// and grey through a dither, and the menu comes through it (system/menu/logic.h). Its items:
//
//   Continue           back to the firmware (also ESC or del)
//   Settings           the board's settings (system/settings.h): volume, volume limit, screen brightness,
//                      key lights, language, upload screenshots, reset to defaults; live, written to the flash
//                      once when the menu closes
//   Back to Launcher   the Launcher's home screen, whatever its settings (the restart ticket,
//                      system/launcher.h)
//   Restart            this firmware again (under the Launcher: its boot ticket, no countdown)
//   Turn off           deep sleep; HOME turns the board on again and this firmware starts
//   About this device  the firmware, the Launcher's and the HAL's versions, the MAC, flash, PSRAM, SD card
//   Download mode      the ROM's download mode, to flash over USB (esptool)
//
// Keys, as in the Launcher: ; and , up (on a value: less), . and / down (more), ENTER or a tap on HOME
// choose, ESC or del go back a page; they are lit while the menu is open, every other key light is off. The
// words follow the board's language setting (the Launcher's NVS value launcher/lang).
//
// While the menu is open the firmware keeps running (its tasks, its sound), but the HAL holds back what
// would disturb the menu: Display::Present* send nothing (they return true; the frame after the menu
// closes goes whole, so the firmware's screen comes back by itself), Keyboard::updateKeyList reports no
// key (keys still down when it closes stay hidden until they are released), and the key lights show the
// menu's keys (setLED* keep the firmware's levels, restored when it closes). A firmware that draws through
// the panel handles itself (Display::GetPanel, Present() without a framebuffer) should stop while IsOpen().
//
// CardenzaHAL::Init starts it (CONFIG_CARDENZA_HAL_SYSTEM_MENU, on by default): a task of 5 KB that reads
// the side key and space every 10 ms (shared with the screenshots, screenshot/screenshot.h). The user,
// 2026-11-24: "把当前进入Home键，按下去进入改成松开进入Home键界面，而且是在短按没有截屏的情况下，才会进入": a press of
// 30 ms to 1 s that took no screenshot opens the menu once HOME is let go, 200 ms after the release (a space in
// that moment makes it a screenshot: HOME + space in any order is one, screenshot/logic.h HomeGesture); a longer
// press opens nothing; a HOME already down when the firmware starts, or still down when the menu closes, opens
// nothing until it has been let go. So with the menu on, HOME is the system's (System::GetHomeButton reads it
// released): a firmware that needs HOME for itself turns the menu off with Enable(false) (or the Kconfig
// option), and HOME + space is still the screenshot there.
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
