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
    // While HOME is held the menu is pulled out from the left like a curtain: after a tap's length it comes
    // out (the Launcher's hold-to-act easing, power_state.h there), a little further than it will stay, and
    // settles back to the rail and the rows' start (kClothWidth) by the time the hold is complete; then the
    // rest fades in (kPopMs). Let go before that, it slides back out of sight (the Launcher's return: 300 ms,
    // eased both ways). The panel has no tearing-effect line: only the narrow curtain moves (and, in the
    // menu, the selection bar's edges, a few pixels a frame: the Launcher's ListSlide), what covers the
    // screen fades.
    inline constexpr uint32_t kPullDelayMs = 120;  // a tap on HOME shows nothing
    inline constexpr int kClothWidth = 44;         // the Launcher's rail (24) and 20 pixels of its rows
    inline constexpr int kPullPeak = 56;           // how far it comes out before settling
    inline constexpr float kPullPeakAt = 0.7f;     // of the hold
    inline constexpr uint32_t kPopMs = 180;        // the rest of the menu fades in
    inline constexpr uint32_t kCloseMs = 150;      // Continue: the firmware's picture fades back
    // The curtain's edge (pixels from the left) when HOME has been held `heldMs`.
    float PullEdge(uint32_t heldMs);
    // The edge `sinceMs` after HOME was let go with the edge at `from`.
    float ReturnEdge(float from, uint32_t sinceMs);
    // While the curtain is out, the menu's column c is at x = c + ClothOffset(edge): it slides with its edge.
    constexpr int ClothOffset(int edge) { return edge - kClothWidth; }

    // A grey as the panel takes it: RGB565 with its bytes swapped (the Launcher's grey to RGB565).
    uint16_t PanelGrey(uint8_t grey);
    // Between two pixels as the panel takes them: weight 0 is a, 256 is b.
    uint16_t PanelMix(uint16_t a, uint16_t b, int weight);

    // Reading one u8 value from the NVS partition without the NVS library (nothing is written). One page
    // (4096 bytes) at a time: first every page for the namespace's index, then every page for the value.
    // ESP-IDF 5.3's layout (components/nvs_flash/src/nvs_types.hpp, nvs_page.hpp).
    inline constexpr std::size_t kNvsPageBytes = 4096;
    // The index of namespace `name` if this page defines it (0: not here).
    uint8_t NvsNamespaceIndex(const uint8_t* page, const char* name);
    // The u8 `key` of namespace `index` if this page holds it.
    bool NvsFindU8(const uint8_t* page, uint8_t index, const char* key, uint8_t& value);

}  // namespace CardenzaHAL::SystemMenu::detail
