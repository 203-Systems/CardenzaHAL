#pragma once

#include <cstddef>
#include <cstdint>

// The screenshot's decisions (screenshot/screenshot.h), pure: no ESP-IDF, so the host tests check them
// (tests/screenshot_test.cpp) and the 203 Launcher carries the same file. screenshot/screenshot.cpp runs them on
// the device.
namespace CardenzaHAL::Screenshot::Logic {

    // ---- HOME + space ------------------------------------------------------------------------------------
    // The user, 2026-11-24: "按住Home键加右下角空格键截屏 … 支持按住空格键，再按Home键截屏。也支持两个快速按下截屏。也支持两个
    // 没有同时在按，但是它们相差的时间非常短 … 这样也算截屏", and the system menu opens when HOME is let go, only after a
    // short press that took no screenshot.
    //
    // A screenshot: space (the bottom-right key, 56) goes down while HOME is down or within kComboWindowMs after
    // HOME was let go; or HOME goes down while space is down or within kComboWindowMs after space was let go. Each
    // such press of space while HOME stays down takes one more. A HOME press that took a screenshot opens nothing.
    // The system menu: HOME let go after a press of kPressMs to kShortPressMs that took no screenshot, and no space
    // within kComboWindowMs after (the menu opens then: a stiff space pressed a little late still makes a
    // screenshot). A longer press opens nothing. A press of HOME during that wait is the same press going on (a
    // bounce, or a double tap: one menu). A HOME already down when watching starts (the firmware started with it
    // held: the ROM's download strap, the Launcher's entry key) or when the menu closes counts for nothing until
    // it has been let go.
    inline constexpr int kSpaceX = 13, kSpaceY = 3;  // the bottom-right key (keyboard/legends.h: kKeySpace, 56)
    inline constexpr uint32_t kComboWindowMs = 200;
    inline constexpr uint32_t kPressMs = 30;          // shorter: a glitch on the pin, not a press
    inline constexpr uint32_t kShortPressMs = 1000;   // longer: no menu

    struct GestureEvents {
        bool screenshot = false;  // take one now
        bool openMenu = false;    // open the system menu now
        bool homeDown = false;    // HOME just went down on a press that counts (the firmware's frames are kept
                                  // from now: the menu's backdrop and a screenshot are made from them)
    };

    class HomeGesture {
      public:
        // Watching starts (or starts again after the menu closed): a HOME down now counts only once let go.
        void Reset(bool homeDown, bool spaceDown, uint32_t nowMs);
        // The keys as they are now (debounced matrix, HOME's pin), every 10 ms or so; `spacePressed`: a press of
        // space came and went since the last call (the keyboard saw it claimed: one too short for the poll).
        GestureEvents Update(bool homeDown, bool spaceDown, bool spacePressed, uint32_t nowMs);
        // Space goes to the system now, not to the firmware: HOME (that counts) is down, or was let go less than
        // kComboWindowMs ago. `homeDownNow`: the pin read right now (fresher than the last Update).
        bool SpaceIsSystems(bool homeDownNow, uint32_t nowMs) const { return SpaceIsSystems(GetWindow(), homeDownNow, nowMs); }
        // The same from a copy of the gesture's state (the keyboard reads it on the firmware's task).
        struct Window {
            bool armed = false, home = false, upValid = false;
            uint32_t upAt = 0;
        };
        Window GetWindow() const { return Window{armed_, home_, homeUpValid_, homeUpAt_}; }
        static bool SpaceIsSystems(const Window& window, bool homeDownNow, uint32_t nowMs);
        // A menu is waiting for the window after HOME's release.
        bool MenuPending() const { return menuAt_ != 0; }

      private:
        bool armed_ = false;      // HOME counts (it was let go since watching started)
        bool home_ = false;
        bool space_ = false;
        uint32_t homeDownAt_ = 0;
        uint32_t homeUpAt_ = 0;   // 0: not within a window
        bool homeUpValid_ = false;
        uint32_t spaceUpAt_ = 0;
        bool spaceUpValid_ = false;
        bool pressShot_ = false;  // this HOME press (with its window) took a screenshot
        uint32_t pressStart_ = 0; // when the press (with its bounces) began
        uint32_t menuAt_ = 0;     // the menu opens then (0: none waiting)
    };

    // What the firmware sees of space (updateKeyList), given the scan, the gesture and whether space is held back
    // (SetSpaceWait). Not held back (the default): a press is the firmware's at once, unless the gesture has
    // space then (claimed: hidden until it is let go). Held back: a press waits kComboWindowMs; HOME in that time
    // claims it, otherwise the firmware gets it late (down while still held, or a short tap of kTapMs when it was
    // already let go).
    class SpaceGate {
      public:
        static constexpr uint32_t kTapMs = 30;
        // True: the firmware sees space down now.
        bool Update(bool spaceDown, bool systemHas, bool wait, uint32_t nowMs);
        // A press was claimed since the last call (once).
        bool TakeClaimed();

      private:
        enum class State : uint8_t { Up, Passed, Claimed, Waiting, Tap };
        State state_ = State::Up;
        uint32_t since_ = 0;
        bool released_ = false;  // let go while Waiting
        bool claimed_ = false;
    };

    // ---- Names ---------------------------------------------------------------------------------------------
    // /screenshot/YYYY-MM-DD_HHMMSS_<Firmware>.png when the clock is set (UTC; a second one in the same second
    // YYYY-MM-DD_HHMMSS-2_<Firmware>.png …), else shot-NNNN_<Firmware>.png (one more than the highest number in
    // the folder). Names are ASCII [A-Za-z0-9._-], at most kNameMax characters.
    inline constexpr std::size_t kNameMax = 80;
    inline constexpr std::size_t kTagMax = 32;
    // The firmware's part: characters outside [A-Za-z0-9-] become '-', at most kTagMax; "Firmware" when empty.
    void FirmwareTag(const char* projectName, char* out, std::size_t capacity);
    // The clock has been set (by the community's answers, the firmware or the Launcher): 2025 or later.
    inline constexpr int64_t kClockSetAfter = 1735689600;  // 2025-01-01T00:00:00Z
    inline bool ClockSet(int64_t unixSeconds) { return unixSeconds >= kClockSetAfter; }
    // "2026-11-24_153012" (UTC; 18 bytes with the terminating zero).
    void Stamp(int64_t unixSeconds, char* out, std::size_t capacity);
    // A dated name (`sequence` 1 for the first in that second) or a numbered one. False when it does not fit.
    bool DatedName(int64_t unixSeconds, int sequence, const char* tag, char* out, std::size_t capacity);
    bool NumberedName(uint32_t number, const char* tag, char* out, std::size_t capacity);
    // The number of a "shot-NNNN_..." name (0 for any other name).
    uint32_t ShotNumber(const char* name);
    // A name the folder and the community take: [A-Za-z0-9._-], 5 to kNameMax characters, ".png" at the end, not
    // starting with '.'.
    bool ValidName(const char* name);
    // Where a shot is written before it is renamed to its name: ".<name>.tmp".
    bool TempName(const char* name, char* out, std::size_t capacity);

    // ---- The upload queue ----------------------------------------------------------------------------------
    // /screenshot/.upload-queue, a journal appended to: "Q <name>" (taken while auto-upload is on), "U <name>"
    // (uploaded, or the community had it or its owner deleted it), "S <name>" (skipped for good). Pending: a Q
    // without a later U or S, oldest first. A torn last line (no '\n') is ignored. The 203 Launcher reads and
    // writes the same file.
    namespace Queue {
        inline constexpr char kQueued = 'Q', kUploaded = 'U', kSkipped = 'S';
        inline constexpr char kFileName[] = ".upload-queue";
        // "Q <name>\n" into `out`; its length, 0 when it does not fit or the name is not valid.
        std::size_t Line(char kind, const char* name, char* out, std::size_t capacity);
        // The oldest pending name (copied into `out`) and how many are pending; false when none is.
        bool FirstPending(const char* text, std::size_t length, char* out, std::size_t capacity, std::size_t* pending);
        // The pending lines alone, in order (the journal rewritten when it has grown): its length (fits in `length`).
        std::size_t Compact(const char* text, std::size_t length, char* out, std::size_t capacity);
    }

    // ---- What the community's answer means for the shot (POST <api>/device/screenshots) --------------------
    enum class Upload : uint8_t {
        Done,       // 200 or 201: uploaded (or it had it, or its owner deleted it): "U"
        Skip,       // 400 or 413: refused for good: "S"
        StopRun,    // 404 (an older community), 409 quota_full: nothing more this run, the queue kept
        Later,      // 429 or anything else (5xx, no answer): again after a while
        SignedOut,  // 401 signin_required, 403 banned: the presence service stops using the token
    };
    Upload Classify(int httpStatus, const char* errorCode);

    // ---- The effect ----------------------------------------------------------------------------------------
    // The user, 2026-11-24: "截屏的时候要有一个画面效果，确保所有固件是统一的". Drawn by the HAL over any firmware (and
    // by the 203 Launcher the same): a white flash that fades out through the 4x4 ordered dither (fades only),
    // and a card at the bottom in the Launcher's look (white, black words) with the shot at a quarter of its size
    // and what happened ("Screenshot saved", "No SD card", …), which comes and goes through the same dither.
    // Nothing is mixed: each pixel is the firmware's, the flash's white or the card's.
    namespace Effect {
        inline constexpr uint32_t kFlashMs = 180;      // white fades out
        inline constexpr uint32_t kCardInAtMs = 90;    // the card comes in, in kDissolveMs
        inline constexpr uint32_t kDissolveMs = 80;
        inline constexpr uint32_t kCardOutAtMs = 1500; // and goes
        inline constexpr uint32_t kDoneMs = kCardOutAtMs + kDissolveMs + 20;
        struct Box {
            int x, y, w, h;
            bool Contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
        };
        inline constexpr Box kCard{8, 135 - 8 - 46, 224, 46};
        // The shot at a quarter of its size (60 x 34), its frame a pixel around it.
        inline constexpr Box kThumb{kCard.x + 6, kCard.y + 6, 60, 34};
        inline constexpr int kTextX = kThumb.x + kThumb.w + 9;
        // The 4x4 ordered dither's threshold at a pixel: 0 to 15 (the system menu's, system/menu/logic.h).
        int Bayer(int x, int y);
        // The flash covers (x, y) at `t` ms after the shot.
        bool Flash(uint32_t t, int x, int y);
        // The card shows at (x, y) at `t` (inside kCard).
        bool Card(uint32_t t, int x, int y);
        // The effect still changes the screen after `t` (frames to send).
        inline bool Moving(uint32_t t)
        {
            return t < kFlashMs || (t >= kCardInAtMs && t < kCardInAtMs + kDissolveMs) || t >= kCardOutAtMs;
        }
        // The thumbnail's pixel (tx, ty) from a 240 x 135 frame: the pixel at the centre of its 4 x 4 block.
        inline int ThumbSourceX(int tx) { return tx * 4 + 2; }
        inline int ThumbSourceY(int ty) { return ty * 4 + 2 < 135 ? ty * 4 + 2 : 134; }
    }

    // ---- Over USB (docs/usb-dev.md, SCREENSHOT 0x12) -------------------------------------------------------
    namespace Usb {
        // kJson: answer one JSON object instead (the name, how saving went, the size; no image): the serial
        // port's word `screenshot`.
        inline constexpr uint8_t kSave = 0x01, kReturn = 0x02, kRaw = 0x04, kQuiet = 0x08, kJson = 0x10;
        inline constexpr uint8_t kDefaultFlags = kSave | kReturn;
        enum class Format : uint8_t { Png = 0, Raw = 1 };
        // How saving went: 0 not asked, 1 saved, 2 no SD card, 3 the card refused the write.
        enum class Saved : uint8_t { NotAsked = 0, Saved = 1, NoCard = 2, Failed = 3 };
        // The answer's header: 'K', version 1, format, saved, width, height (u16 little-endian), the name's length
        // and the name, then the data's length (u32). Returns its length (at most 8 + 1 + kNameMax + 4).
        std::size_t Header(Format format, Saved saved, int width, int height, const char* name, uint32_t dataBytes,
                           uint8_t* out, std::size_t capacity);
    }

}  // namespace CardenzaHAL::Screenshot::Logic
