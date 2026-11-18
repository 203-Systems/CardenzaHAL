#pragma once

#include <cstddef>
#include <cstdint>

#include "system/menu/words.inc"

// The system menu's decisions and drawing (system/menu.h), pure: no ESP-IDF, so the host tests check them
// (tests/system_menu_test.cpp). system/menu.cpp runs them on the device.
namespace CardenzaHAL::SystemMenu::detail {

    // The side key (HOME, GPIO0) held this long opens the menu.
    inline constexpr uint32_t kHoldMs = 1000;

    // When the side key has been held kHoldMs. A press that was already down when watching began (the
    // firmware started with it held: the ROM's download strap, the Launcher's entry key) does not count
    // until it has been released once.
    class HoldDetector {
      public:
        void Reset(bool down)
        {
            armed_ = !down;
            fired_ = false;
            downSince_ = 0;
            wasDown_ = down;
        }
        // True once per press, kHoldMs after it went down.
        bool Update(bool down, uint32_t nowMs)
        {
            if (!down)
            {
                armed_ = true;
                fired_ = false;
                wasDown_ = false;
                return false;
            }
            if (!wasDown_) downSince_ = nowMs;
            wasDown_ = true;
            if (!armed_ || fired_ || nowMs - downSince_ < kHoldMs) return false;
            fired_ = true;
            return true;
        }
        // HOME is down on a press that can still open the menu (the curtain follows it), held this long.
        bool Pressing() const { return armed_ && wasDown_ && !fired_; }
        uint32_t HeldMs(uint32_t nowMs) const { return nowMs - downSince_; }

      private:
        bool armed_ = false;
        bool fired_ = false;
        bool wasDown_ = false;
        uint32_t downSince_ = 0;
    };

    enum class Item : uint8_t { Continue, Launcher, Restart, PowerOff, Download };
    inline constexpr int kItemCount = 5;
    inline constexpr int kVisibleRows = 4;  // as the Launcher's lists on the Cardenza

    enum class Key : uint8_t { None, Up, Down, Select, Back };

    // The Launcher's navigation keys on the Cardenza (203-launcher src/platform/board.cpp): ; up, . down,
    // , and / previous and next (here up and down: the list is vertical), ENTER select, ESC and del
    // back; the side key selects too. Matrix positions as CardenzaHAL::Keyboard reports them (x the
    // column 0-13, y the row 0-3 from the top).
    struct NavigationKey {
        int8_t x, y;
        Key key;
    };
    inline constexpr NavigationKey kNavigationKeys[] = {
        {11, 2, Key::Up},      // ;
        {11, 3, Key::Down},    // .
        {10, 3, Key::Up},      // ,
        {12, 3, Key::Down},    // /
        {13, 2, Key::Select},  // ENTER
        {0, 0, Key::Back},     // ESC (`)
        {13, 0, Key::Back},    // del
    };
    Key KeyAt(int x, int y);

    struct State {
        int selected = 0;
        int scroll = 0;
    };
    struct Outcome {
        bool chosen = false;  // the menu closes: `item` (Back chooses Continue)
        Item item = Item::Continue;
        bool changed = false;  // repaint
    };
    // Up and Down stop at the ends (no wrap, as the Launcher's lists); the view scrolls to keep the
    // selection in sight.
    Outcome Apply(State& state, Key key);

    // A key held down, as the menu sees it: the first press at once, then repeats for Up and Down only (a
    // held Select or Back must not run on into what comes after it), when the Launcher's lists repeat
    // (its uiKeyRepeatDue: after 400 ms every 110 ms, after 1.5 s every 50 ms).
    class KeyRepeat {
      public:
        // `held` is the key down now (None: none). Returns the key to act on at this moment, or None.
        Key Update(Key held, uint32_t nowMs);

      private:
        Key held_ = Key::None;
        uint32_t since_ = 0;
        uint32_t last_ = 0;
    };

    // The Launcher's NVS value launcher/lang (0 English, 1 Simplified Chinese, 2 read as 1, 3 Japanese,
    // 4 Indonesian, 5 Vietnamese; anything else English) as an index into kTexts.
    int LanguageIndex(int launcherLang);

    // What the screen shows.
    struct View {
        int language = 0;            // index into kTexts
        State state;
        const char* firmware = "";   // Continue's detail: the firmware's name and version (ASCII)
        bool releasePrompt = false;  // "Release HOME" under the chosen row
        bool backHeld = false;       // ESC or del down: the rail's back arrow dims, as in the Launcher
    };

    inline constexpr int kWidth = 240;
    inline constexpr int kHeight = 135;

    // ---- How it comes and goes (system/menu.cpp) ---------------------------------------------------------
    // The menu is the Launcher, pulled out from the left like a curtain (the user, 2026-11-18: "从左侧慢慢拉
    // 出来一点，又慢慢缩回去一点点，直到达到弹出时间的时候，快速渐变动画弹出来 ... 如果中途放手了，也会慢慢的用
    // 一定的非线性速度回去", and again that day after a straight pull was tried: "出来一些，然后缩回去一点点，
    // 超过触发时间就弹出来 ... 保持收回去，动画连贯性好").
    //
    // HOME held past a tap's length (kPullDelayMs): the menu's left part is uncovered up to an edge that
    // goes out to kPullOutPixels (enough to read what it is: the Launcher's black rail with the page's name,
    // the selection bar and the first words of the rows), then eases back kPullBackPixels, both on the
    // Launcher's hold-to-act curve (powerHoldFill: a cubic in and out, at rest at each end), and is at rest
    // when the hold is complete (kHoldMs). Then it pops: the rest of the menu fades in over the firmware's
    // picture (kPopMs, the Launcher's ease) while the backlight goes to the Launcher's.
    // Let go before that: the edge goes back to the left from where it is (kReturnMs, the Launcher's
    // kPowerReturnMs), on the cubic the Launcher's lists move their selection bar's edges with
    // (ListSlide::Edge: from where it is, at the speed it has when that is already toward the left, from rest
    // when it was still going out). A new press while it goes back adds its pull to what is left of the way
    // back: the edge never jumps. Continue closes the same way round: the right part fades back to the
    // firmware's picture (kCloseMs), then the curtain goes back to the left.
    //
    // The menu stays in place and is uncovered, never slid: the panel has no tearing-effect line, so a frame
    // sends only the columns the edge passed since the last one (a strip a few columns wide, one write along
    // the panel's scan), and what covers a large area fades.
    inline constexpr uint32_t kPullDelayMs = 120;  // a tap on HOME shows nothing
    inline constexpr uint32_t kPullOutMs = 620;    // held this long, the edge is furthest out
    inline constexpr int kPullOutPixels = 112;
    inline constexpr int kPullBackPixels = 16;     // and back by this at kHoldMs
    inline constexpr int kPullRestPixels = kPullOutPixels - kPullBackPixels;
    inline constexpr uint32_t kReturnMs = 300;     // let go: back to the left
    inline constexpr uint32_t kPopMs = 180;        // the rest of the menu fades in, the backlight to the Launcher's
    inline constexpr uint32_t kCloseMs = 150;      // Continue: the right part fades back to the firmware's picture
    // The curtain's edge (pixels from the left) when HOME has been held `heldMs`: 0 until kPullDelayMs, out to
    // kPullOutPixels at kPullOutMs, back to kPullRestPixels at kHoldMs (and there after it).
    float PullEdge(uint32_t heldMs);
    // Its speed there (pixels a millisecond; negative: going back).
    float PullSpeed(uint32_t heldMs);

    // The curtain's edge over time: the pull while HOME is held on a press that can open the menu, plus
    // what is left of a way back to the left (after HOME was let go, or after the menu closed).
    class Curtain {
      public:
        // Every frame. `pulling`: HOME is down on a press that can still open the menu, held `heldMs`.
        // Returns the edge: 0 to kWidth.
        float Update(bool pulling, uint32_t heldMs, uint32_t nowMs);
        // The curtain is at `edge` at rest and goes back to the left from now (the menu closed).
        void ReturnFrom(float edge, uint32_t nowMs);
        // It is out or on its way: draw frames.
        bool Moving(uint32_t nowMs) const { return pulling_ || nowMs - backSince_ < backMs_; }
        void Reset() { *this = Curtain(); }

      private:
        float Back(uint32_t nowMs) const;
        float BackSpeed(uint32_t nowMs) const;
        bool pulling_ = false;
        uint32_t heldMs_ = 0;
        // The way back: from backFrom_ at backSpeed_ (pixels a millisecond) to 0, at rest after backMs_.
        float backFrom_ = 0, backSpeed_ = 0;
        uint32_t backSince_ = 0, backMs_ = 0;
    };

    // A grey as the HAL's panel takes it: the Launcher's grey to RGB565 (its Lcd::rgb) in the processor's
    // byte order. The Launcher swaps the two bytes for its own panel setup (big-endian data, ESP-IDF's
    // default); the HAL sets the same panel to little-endian data (display/display.cpp), so the swapped
    // pixel was another colour here (the selection bar's grey 20 a dull pink, the details' 170 green) until
    // 2026-11-18. Black and white are the same either way, which is how it went unseen in the tests.
    uint16_t PanelGrey(uint8_t grey);
    // Between two pixels as the panel takes them: weight 0 is a, 256 is b.
    uint16_t PanelMix(uint16_t a, uint16_t b, int weight);

    // The backlight as the Launcher sets it on the Cardenza (its src/platform/board.cpp backlightDuty): the
    // duty of its brightness setting `percent` (its NVS value launcher/bright: 10 to 100, 0 or missing 100)
    // out of `full`. The menu shows at this duty and at the Launcher's PWM frequency, whatever the firmware's
    // brightness: the same white as the Launcher's.
    int LauncherBrightness(bool found, int32_t stored);
    int LauncherBacklightDuty(int percent, int full);

    // Reading one value from the NVS partition without the NVS library (nothing is written). One page
    // (4096 bytes) at a time: first every page for the namespace's index, then every page for the value.
    // ESP-IDF 5.3's layout (components/nvs_flash/src/nvs_types.hpp, nvs_page.hpp).
    inline constexpr std::size_t kNvsPageBytes = 4096;
    // The index of namespace `name` if this page defines it (0: not here).
    uint8_t NvsNamespaceIndex(const uint8_t* page, const char* name);
    // The u8 `key` of namespace `index` if this page holds it.
    bool NvsFindU8(const uint8_t* page, uint8_t index, const char* key, uint8_t& value);
    // The i32 `key` of namespace `index` if this page holds it.
    bool NvsFindI32(const uint8_t* page, uint8_t index, const char* key, int32_t& value);

}  // namespace CardenzaHAL::SystemMenu::detail
