#pragma once

#include <cstddef>
#include <cstdint>

// Screenshots, in every firmware on CardenzaHAL (the user, 2026-11-24: "给HAL加入一个截图功能。会把内容保存到SD卡
// 里面的固定位置/screenshot，并且可以在社区的个人主页里面访问。" and "按住Home键加右下角空格键截屏 … 确保所有固件是
// 统一的"). docs/SCREENSHOTS.md is the whole story; in short:
//
// - Taken with HOME + space (the bottom-right key) in any order, together, or one just after the other (within
//   200 ms: Logic::kComboWindowMs); over USB (the development channel's SCREENSHOT, `203cli board screenshot`);
//   or by the firmware (Take). The system menu now opens when HOME is let go after a short press that took no
//   screenshot (screenshot/logic.h, system/menu.h).
// - What is taken: the screen's 240 x 135 pixels as the firmware last presented them (Display::Present*, the
//   frame presenter, DoubleBuffer), or as it drew them through the panel's handles (the HAL keeps a copy of
//   those draws in PSRAM from the first one), never the HAL's own menu or effect.
// - The file: /sdcard/screenshot/<date>_<time>_<Firmware>.png (UTC; shot-NNNN_<Firmware>.png while the board's
//   clock is not set), a PNG of the exact pixels (screenshot/png.h), written to a temporary name and renamed.
// - The effect: a white flash that fades, then a card at the bottom in the Launcher's look with the shot small
//   and what happened, for 1.6 s, the same in every firmware and in the Launcher. The firmware keeps running
//   (its sound, its frames: the card is drawn over its latest ones).
// - The upload: with the board setting screenshot_upload on (the default; HOME > Settings, `203cli board
//   settings`), each shot is queued (/sdcard/screenshot/.upload-queue) and goes up to the community account
//   the Launcher bound the board to, private, by the presence service when it is online (net/presence.h): one
//   file at a time at priority 1, after its inbox requests, never while the firmware's activity or audio says
//   no. A shot the community refuses is skipped; the rest wait for the next time.
//
// Cost: nothing at rest but the keyboard's look at space (a few instructions per updateKeyList). A shot takes a
// copy of the frame (65 KB of PSRAM) and the encoder's 32 KB (PSRAM when there is some) for the moment it is
// encoded and written, on a task of priority 1 (6 KB of internal RAM, gone 10 s after the last shot).
namespace CardenzaHAL::Screenshot {

    inline constexpr char kFolder[] = "/sdcard/screenshot";

    enum class Status : uint8_t {
        Saved,        // on the card
        Taken,        // taken, not asked to be saved (USB --no-save)
        NoCard,       // no SD card mounted: not saved
        WriteFailed,  // the card refused the write
        NoFrame,      // this firmware has shown nothing the HAL can capture
        NoMemory,     // no memory for the frame or the encoder now
        Busy,         // another screenshot is being taken
    };
    const char* StatusName(Status status);  // "saved", "no card", ...

    struct Shot {
        Status status = Status::NoFrame;
        char name[84] = "";     // the file's name in kFolder (empty when not saved)
        uint32_t bytes = 0;     // the PNG's size
    };

    struct Options {
        bool save = true;    // write it to the card (and queue its upload when screenshot_upload is on)
        bool effect = true;  // the flash and the card on the screen
    };

    // Started by CardenzaHAL::Init with the display (CONFIG_CARDENZA_HAL_SCREENSHOT, on by default): HOME + space
    // from then on. Once, however often it is called.
    void Start();

    // A screenshot now, as HOME + space takes one: the frame the firmware presents next (or its last one when it
    // presents none within 250 ms), saved and shown as the options say. Returns at once (the work is done on the
    // HAL's task); false when one is being taken already or screenshots are not started. Any task.
    bool Take(const Options& options = {});

    // Told when a screenshot is done, on the HAL's task: keep it short. One listener (nullptr: none).
    using Listener = void (*)(const Shot& shot, void* context);
    void SetListener(Listener listener, void* context);

    // The effect is on the screen (1.6 s after a shot). A firmware that draws through the panel's handles itself
    // holds its drawing meanwhile, as it does while the system menu is open (SystemMenu::IsOpen).
    bool IsShowing();

    // Space (the bottom-right key) and HOME make a screenshot in either order. Space pressed first reaches the
    // firmware at once, as always (a music firmware's PLAY must not wait); when HOME follows within the window
    // the screenshot is taken all the same, after the firmware saw space. A firmware where space is not
    // time-critical may ask for space to be held back for that window (200 ms, Logic::kComboWindowMs): then a
    // space that becomes a screenshot never reaches it, and every other press of space reaches it 200 ms late.
    // Off by default.
    void SetSpaceWait(bool wait);
    bool SpaceWait();

}  // namespace CardenzaHAL::Screenshot
