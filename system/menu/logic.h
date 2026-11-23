#pragma once

#include <cstddef>
#include <cstdint>

#include "system/menu/words.inc"
#include "system/settings_logic.h"

// The system menu's decisions and drawing (system/menu.h), pure: no ESP-IDF, so the host tests check them
// (tests/system_menu_test.cpp). system/menu.cpp runs them on the device.
namespace CardenzaHAL::SystemMenu::detail {

    // A press of the side key (HOME, GPIO0) opens the menu (the user, 2026-11-18: no hold, no animation while
    // it is held: "直接点按"): when it has read as down this long, which a contact's bounce or a glitch on the
    // pin does not.
    inline constexpr uint32_t kPressMs = 30;

    // When the side key has been down kPressMs: once per press. A press that was already down when watching
    // began (the firmware started with it held: the ROM's download strap, the Launcher's entry key; or the
    // menu closed while it was held) does not count until it has been released once.
    class PressDetector {
      public:
        void Reset(bool down)
        {
            armed_ = !down;
            fired_ = false;
            downSince_ = 0;
            wasDown_ = down;
        }
        // True once per press, kPressMs after it went down.
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
            if (!armed_ || fired_ || nowMs - downSince_ < kPressMs) return false;
            fired_ = true;
            return true;
        }
        // HOME is down on a press that is about to open the menu.
        bool Pending() const { return armed_ && wasDown_ && !fired_; }

      private:
        bool armed_ = false;
        bool fired_ = false;
        bool wasDown_ = false;
        uint32_t downSince_ = 0;
    };

    // The main list (system/menu.h): Continue, Settings, Back to Launcher, Restart, Turn off, About this
    // device, Download mode.
    enum class Item : uint8_t { Continue, Settings, Launcher, Restart, PowerOff, About, Download };
    inline constexpr int kItemCount = 7;
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

    // The pages (the user's request 2026-11-23: the board's settings and what it is, in this menu):
    //   Main      the list above
    //   Settings  a row per setting with its value on the right (the Launcher's Display page), and Reset
    //   Value     one setting being changed: its value large over a track (the Launcher's Backlight page);
    //             ; , less, . / more (as the Launcher's: up and previous less), live; ENTER keeps, ESC puts
    //             back what it was
    //   Language  the Launcher's languages, each in its own script (its Language card); ENTER takes one
    //   Reset     the Launcher's question with two buttons, Cancel focused (its uiConfirm)
    //   About     what the board runs and has (the rows of the Launcher's About)
    // ESC (or del) goes back a page; on the main page it is Continue.
    enum class Page : uint8_t { Main, Settings, Value, Language, Reset, About };

    // The Settings page's rows.
    enum class Row : uint8_t { Volume, VolumeLimit, ScreenBrightness, KeyBrightness, MicGain, Language, Reset };
    inline constexpr int kRowCount = 7;
    // The setting of a row (Reset: none, Volume's).
    Settings::Id RowSetting(Row row);

    struct ListState {
        int selected = 0;
        int scroll = 0;
    };
    struct State {
        Page page = Page::Main;
        ListState main, settings, language;
        int button = 1;                            // Reset's focused button: 0 Reset, 1 Cancel
        Settings::Id editing = Settings::Id::Volume;  // the Value page's setting
        int original = 0;                          // its value when the page opened
        Settings::detail::Values values = Settings::detail::Defaults();  // as the menu shows them
    };
    struct Outcome {
        bool close = false;        // back to the firmware (Continue, or ESC on the main page)
        bool leave = false;        // Back to Launcher, Restart, Turn off or Download mode: `item`
        Item item = Item::Continue;
        bool changed = false;      // repaint
        bool page = false;         // another page: drawn at once, a new selection bar
        bool set = false;          // set `id` to `value` now (already in State::values)
        Settings::Id id = Settings::Id::Volume;
        int value = 0;
        bool reset = false;        // every setting to its default but the language (Settings::Reset)
    };
    // Up and Down stop at the ends (no wrap, as the Launcher's lists); the view scrolls to keep the
    // selection in sight. A list of `count` rows.
    bool Move(ListState& list, int delta, int count);
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

    // What the About page says (system/menu.cpp gathers it as the page opens). ASCII; empty: not known.
    struct AboutInfo {
        char firmware[40] = "";   // the firmware's name
        char version[40] = "";    // its version
        char launcher[24] = "";   // the 203 Launcher's version (empty: no Launcher on this board)
        char hal[24] = "";        // the HAL's commit at build time ("+": with local changes)
        char mac[20] = "";        // as the Launcher's About shows it
        uint32_t flashBytes = 0;  // the flash the firmware sees
        uint32_t psramBytes = 0;  // 0: none
        bool card = false;        // an SD card is mounted
        uint64_t cardBytes = 0, cardFree = 0;
    };

    // What the screen shows.
    struct View {
        int language = 0;            // index into kTexts
        State state;
        const char* firmware = "";   // Continue's detail: the firmware's name and version (ASCII)
        bool releasePrompt = false;  // "Release HOME" under the chosen row
        bool backHeld = false;       // ESC or del down: the rail's back arrow dims, as in the Launcher
        const AboutInfo* about = nullptr;
    };

    // A setting's value as the menu writes it: "70", "80%", "+6 dB" (ASCII; at most 15 bytes). The
    // language's is its name (kLanguageNames).
    void ValueText(Settings::Id id, int value, char* out, std::size_t size);
    // A size as the About page writes it: "16 MB", "29.7 GB".
    void SizeText(uint64_t bytes, char* out, std::size_t size);

    inline constexpr int kWidth = 240;
    inline constexpr int kHeight = 135;

    // ---- How it comes and goes (system/menu.cpp) ---------------------------------------------------------
    // The user, 2026-11-18: "直接点按，就直接一个 dither 变暗变低饱和屏幕，然后再让它这个界面显示出来" and
    // "必须要让它只保持黑白灰". A press of HOME: the firmware's picture (its last frame) goes dark and grey
    // through an ordered dither (the backdrop, one frame), and after kBackdropMs the menu comes through it,
    // pixel by pixel in the dither's order (kDissolveSteps frames: every pixel is the backdrop's or the
    // menu's, never a mix, so nothing on the screen has a colour). The panel has no tearing-effect line: a
    // dissolve can be written across its scan without a seam, a slide of the whole screen cannot. Continue:
    // the backdrop again (the firmware's latest frame) for kCloseMs, then the firmware's picture.
    inline constexpr uint32_t kBackdropMs = 70;
    inline constexpr int kDissolveSteps = 4;
    inline constexpr uint32_t kCloseMs = 60;

    // The 4x4 ordered dither's threshold at a pixel: 0 to 15.
    int Bayer(int x, int y);
    // The backdrop at (x, y) over the firmware's pixel there (RGB565 as the panel takes it): its luma
    // (Rec. 709), darkened to two fifths and dithered to the greys 0, 32, 64, 96 and 128.
    uint8_t BackdropGrey(uint16_t firmwarePixel, int x, int y);
    // Without the firmware's picture: a dark dithered field.
    uint8_t BackdropField(int x, int y);
    // At dissolve step `step` (0: the backdrop alone, kDissolveSteps: the menu alone) the pixel is the menu's.
    bool Dissolved(int x, int y, int step);
    // The grey the screen shows at (x, y) at dissolve step `step`: the menu's (`menuGrey`) or the backdrop's
    // over `firmwarePixel` (null: no picture). Everything the menu puts on the panel is PanelGrey of this.
    uint8_t SceneGrey(int step, uint8_t menuGrey, const uint16_t* firmwarePixel, int x, int y);

    // A grey as the HAL's panel takes it: RGB565 in the processor's byte order (the HAL sets the panel to
    // little-endian data; the Launcher sends its own setup of the panel the bytes swapped, and its swapped
    // pixels were other colours here until 2026-11-18), and neutral: red and blue the grey's five bits, green
    // those five bits as the panel widens them to six (the top bit again at the bottom). The Launcher's own
    // Lcd::rgb takes green's sixth bit from the grey instead, which is a step of green more or less than the
    // red and blue beside it on half the greys (20, the selection bar: a little green; 170, the details: a
    // little magenta): the menu shows black, white and grey only.
    uint16_t PanelGrey(uint8_t grey);
    // The pixel is a neutral grey (PanelGrey's rule).
    bool PanelNeutral(uint16_t pixel);

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
