#pragma once

#include <array>
#include <cstdint>

#include "types.h"

// What the Cardenza's key caps say, by key number. Pure: a firmware, its simulator and its tests can all
// use it. Use these legends, never a layout of your own: the arrows are the yellow triangles on ; . , /
// (not HJKL, IJKL or WASD), ok confirms and esc goes back.
//
// Key number n = y * 14 + x + 1 (getKeyNum), y = 0 the top row, x = 0 the left column. Black legend:
// the key's own character or name; yellow: its shifted character; four keys also carry a yellow arrow.
//
//   y   keys   x=0   1      2    3    4    5    6    7    8    9    10        11        12         13
//   0   1..14  esc   1 !    2 @  3 #  4 $  5 %  6 ^  7 &  8 *  9 (  0 )       - _       = +        del
//   1  15..28  tab   q      w    e    r    t    y    u    i    o    p         [ {       ] }        \ |
//   2  29..42  fn    shift  a    s    d    f    g    h    j    k    l         ; : up    ' "        ok
//   3  43..56  ctrl  opt    alt  z    x    c    v    b    n    m    , < left  . > down  / ? right  space
//
// up, down, left, right: the yellow arrows. del is printed as a left arrow (backspace), shift as an
// outlined arrow, space as an open box; ok is enter.
//
// HOME, the side key left of the screen, is not in the matrix (System::GetHomeButton): in every firmware
// on the HAL a press opens the system menu.
//
// Conventions:
// - Text input: every key types its character (Typed), shift its yellow one; the arrows are then
//   fn + ; . , /. del erases the character before the cursor, ok ends the input.
// - A screen that types nothing (a list, a menu, a game) may take ; . , / alone as the arrows, as the
//   203 Launcher and the system menu do.
// - ok confirms, esc (or del) goes back.
namespace CardenzaHAL::Keyboard {

enum class Arrow : uint8_t
{
    None,
    Up,
    Down,
    Left,
    Right,
};

struct KeyLegend
{
    const char* name;  // the black legend in lower case: "esc", "1", "q", ";", "ok", "space"
    char character;    // what it types: printable ASCII (space ' '); 0 for a key that types no character
    char shifted;      // what it types with shift (letters: upper case); 0 likewise
    Arrow arrow;       // the yellow arrow printed on it, if any
};

inline constexpr uint8_t KEY_COUNT = 56;

// The named keys. esc, del, tab, ok and the modifiers type no character (0): find them by number.
inline constexpr uint8_t kKeyEsc = 1;
inline constexpr uint8_t kKeyDel = 14;    // backspace
inline constexpr uint8_t kKeyTab = 15;
inline constexpr uint8_t kKeyFn = 29;     // the left column's third key (not the bottom one: that is ctrl)
inline constexpr uint8_t kKeyShift = 30;
inline constexpr uint8_t kKeyOk = 42;     // enter
inline constexpr uint8_t kKeyCtrl = 43;   // bottom left
inline constexpr uint8_t kKeyOpt = 44;
inline constexpr uint8_t kKeyAlt = 45;
inline constexpr uint8_t kKeySpace = 56;  // types ' '
// The arrows: the yellow triangles.
inline constexpr uint8_t kKeyUp = 40;     // ;
inline constexpr uint8_t kKeyDown = 54;   // .
inline constexpr uint8_t kKeyLeft = 53;   // ,
inline constexpr uint8_t kKeyRight = 55;  // /

// Index key number - 1.
inline constexpr std::array<KeyLegend, KEY_COUNT> kKeyLegends{{
    {"esc", 0, 0, Arrow::None},
    {"1", '1', '!', Arrow::None},
    {"2", '2', '@', Arrow::None},
    {"3", '3', '#', Arrow::None},
    {"4", '4', '$', Arrow::None},
    {"5", '5', '%', Arrow::None},
    {"6", '6', '^', Arrow::None},
    {"7", '7', '&', Arrow::None},
    {"8", '8', '*', Arrow::None},
    {"9", '9', '(', Arrow::None},
    {"0", '0', ')', Arrow::None},
    {"-", '-', '_', Arrow::None},
    {"=", '=', '+', Arrow::None},
    {"del", 0, 0, Arrow::None},

    {"tab", 0, 0, Arrow::None},
    {"q", 'q', 'Q', Arrow::None},
    {"w", 'w', 'W', Arrow::None},
    {"e", 'e', 'E', Arrow::None},
    {"r", 'r', 'R', Arrow::None},
    {"t", 't', 'T', Arrow::None},
    {"y", 'y', 'Y', Arrow::None},
    {"u", 'u', 'U', Arrow::None},
    {"i", 'i', 'I', Arrow::None},
    {"o", 'o', 'O', Arrow::None},
    {"p", 'p', 'P', Arrow::None},
    {"[", '[', '{', Arrow::None},
    {"]", ']', '}', Arrow::None},
    {"\\", '\\', '|', Arrow::None},

    {"fn", 0, 0, Arrow::None},
    {"shift", 0, 0, Arrow::None},
    {"a", 'a', 'A', Arrow::None},
    {"s", 's', 'S', Arrow::None},
    {"d", 'd', 'D', Arrow::None},
    {"f", 'f', 'F', Arrow::None},
    {"g", 'g', 'G', Arrow::None},
    {"h", 'h', 'H', Arrow::None},
    {"j", 'j', 'J', Arrow::None},
    {"k", 'k', 'K', Arrow::None},
    {"l", 'l', 'L', Arrow::None},
    {";", ';', ':', Arrow::Up},
    {"'", '\'', '"', Arrow::None},
    {"ok", 0, 0, Arrow::None},

    {"ctrl", 0, 0, Arrow::None},
    {"opt", 0, 0, Arrow::None},
    {"alt", 0, 0, Arrow::None},
    {"z", 'z', 'Z', Arrow::None},
    {"x", 'x', 'X', Arrow::None},
    {"c", 'c', 'C', Arrow::None},
    {"v", 'v', 'V', Arrow::None},
    {"b", 'b', 'B', Arrow::None},
    {"n", 'n', 'N', Arrow::None},
    {"m", 'm', 'M', Arrow::None},
    {",", ',', '<', Arrow::Left},
    {".", '.', '>', Arrow::Down},
    {"/", '/', '?', Arrow::Right},
    {"space", ' ', ' ', Arrow::None},
}};

// The legend of no key (a number outside 1..56, a position off the keyboard): name "", nothing typed.
inline constexpr KeyLegend kNoKeyLegend{"", 0, 0, Arrow::None};

// The key number at a position, 0 off the keyboard.
constexpr uint8_t KeyNumber(Point2D_t position)
{
    if (position.x < 0 || position.x >= 14 || position.y < 0 || position.y >= 4) return 0;
    return static_cast<uint8_t>(position.y * 14 + position.x + 1);
}

// The position of a key number, {-1, -1} for a number outside 1..56.
constexpr Point2D_t KeyPosition(int key)
{
    if (key < 1 || key > KEY_COUNT) return {-1, -1};
    return {(key - 1) % 14, (key - 1) / 14};
}

constexpr const KeyLegend& Legend(int key)
{
    return key >= 1 && key <= KEY_COUNT ? kKeyLegends[key - 1] : kNoKeyLegend;
}

constexpr const KeyLegend& Legend(Point2D_t position)
{
    return Legend(KeyNumber(position));
}

// The character a key types, with or without shift; 0 when it types none.
constexpr char Typed(int key, bool shift)
{
    return shift ? Legend(key).shifted : Legend(key).character;
}

constexpr char Typed(Point2D_t position, bool shift)
{
    return Typed(KeyNumber(position), shift);
}

// The key that carries this arrow, 0 for Arrow::None.
constexpr uint8_t ArrowKey(Arrow arrow)
{
    switch (arrow)
    {
    case Arrow::Up: return kKeyUp;
    case Arrow::Down: return kKeyDown;
    case Arrow::Left: return kKeyLeft;
    case Arrow::Right: return kKeyRight;
    default: return 0;
    }
}

}  // namespace CardenzaHAL::Keyboard
