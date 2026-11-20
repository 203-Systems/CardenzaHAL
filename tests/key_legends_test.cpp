#include "keyboard/legends.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

namespace {

namespace Keyboard = CardenzaHAL::Keyboard;
using Keyboard::Arrow;

// The caps as the board prints them, row by row from the top (written out here, not taken from the
// header). In the character rows '~' stands for a key that types nothing: the board has no ~ (nor `).
constexpr const char* kNames[4][14] = {
    {"esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "del"},
    {"tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\"},
    {"fn", "shift", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "ok"},
    {"ctrl", "opt", "alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "space"},
};
constexpr const char* kPlain[4] = {"~1234567890-=~", "~qwertyuiop[]\\", "~~asdfghjkl;'~", "~~~zxcvbnm,./ "};
constexpr const char* kShifted[4] = {"~!@#$%^&*()_+~", "~QWERTYUIOP{}|", "~~ASDFGHJKL:\"~", "~~~ZXCVBNM<>? "};

// Usable in constant expressions.
static_assert(Keyboard::Legend(Keyboard::kKeyUp).arrow == Arrow::Up);
static_assert(Keyboard::Typed(Keyboard::kKeySpace, true) == ' ');
static_assert(Keyboard::KeyNumber({13, 3}) == 56 && Keyboard::KeyPosition(29).y == 2);

void TableTests()
{
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 14; ++x)
        {
            const int key = y * 14 + x + 1;
            const Keyboard::KeyLegend& legend = Keyboard::Legend(key);
            assert(&legend == &Keyboard::Legend(Keyboard::Point2D_t{x, y}));
            assert(std::strcmp(legend.name, kNames[y][x]) == 0);
            const char plain = kPlain[y][x] == '~' ? 0 : kPlain[y][x];
            const char shifted = kShifted[y][x] == '~' ? 0 : kShifted[y][x];
            assert(legend.character == plain && legend.shifted == shifted);
            assert(Keyboard::Typed(key, false) == plain && Keyboard::Typed(key, true) == shifted);
            assert(Keyboard::Typed(Keyboard::Point2D_t{x, y}, true) == shifted);
            // A key types something with shift exactly when it does without.
            assert((plain == 0) == (shifted == 0));
            // A character key is named by its character.
            if (plain != 0 && plain != ' ') assert(legend.name == std::string(1, plain));
        }

    // Everything printable on a US keyboard but ` and ~, each on one key.
    std::set<char> typed;
    int typing = 0;
    for (int key = 1; key <= Keyboard::KEY_COUNT; ++key)
    {
        const char plain = Keyboard::Typed(key, false);
        if (plain == 0) continue;
        ++typing;
        assert(typed.insert(plain).second);
        if (plain != ' ') assert(typed.insert(Keyboard::Typed(key, true)).second);
    }
    assert(typing == 47);
    for (char c = ' '; c <= '~'; ++c) assert(typed.count(c) == (c != '`' && c != '~'));
}

void NamedKeyTests()
{
    const struct
    {
        int key;
        const char* name;
        int x, y;
    } named[] = {
        {Keyboard::kKeyEsc, "esc", 0, 0},     {Keyboard::kKeyDel, "del", 13, 0},     {Keyboard::kKeyTab, "tab", 0, 1},
        {Keyboard::kKeyFn, "fn", 0, 2},       {Keyboard::kKeyShift, "shift", 1, 2},  {Keyboard::kKeyOk, "ok", 13, 2},
        {Keyboard::kKeyCtrl, "ctrl", 0, 3},   {Keyboard::kKeyOpt, "opt", 1, 3},      {Keyboard::kKeyAlt, "alt", 2, 3},
        {Keyboard::kKeySpace, "space", 13, 3}, {Keyboard::kKeyUp, ";", 11, 2},       {Keyboard::kKeyDown, ".", 11, 3},
        {Keyboard::kKeyLeft, ",", 10, 3},     {Keyboard::kKeyRight, "/", 12, 3},
    };
    for (const auto& n : named)
    {
        assert(std::strcmp(Keyboard::Legend(n.key).name, n.name) == 0);
        const Keyboard::Point2D_t position = Keyboard::KeyPosition(n.key);
        assert(position.x == n.x && position.y == n.y);
    }
    assert(Keyboard::kKeyEsc == 1 && Keyboard::kKeyDel == 14 && Keyboard::kKeyTab == 15 && Keyboard::kKeyFn == 29);
    assert(Keyboard::kKeyShift == 30 && Keyboard::kKeyOk == 42 && Keyboard::kKeyCtrl == 43 && Keyboard::kKeyOpt == 44);
    assert(Keyboard::kKeyAlt == 45 && Keyboard::kKeySpace == 56);
    assert(Keyboard::kKeyUp == 40 && Keyboard::kKeyDown == 54 && Keyboard::kKeyLeft == 53 && Keyboard::kKeyRight == 55);
    // Only space types among them.
    for (const auto& n : named)
    {
        const bool types = n.key == Keyboard::kKeySpace || Keyboard::Legend(n.key).arrow != Arrow::None;
        assert((Keyboard::Typed(n.key, false) != 0) == types);
    }
}

void ArrowTests()
{
    // Four keys carry an arrow: the yellow triangles on ; . , /, and each arrow is on one key.
    int arrows = 0;
    for (int key = 1; key <= Keyboard::KEY_COUNT; ++key)
    {
        const Arrow arrow = Keyboard::Legend(key).arrow;
        if (arrow == Arrow::None) continue;
        ++arrows;
        assert(Keyboard::ArrowKey(arrow) == key);
    }
    assert(arrows == 4);
    assert(Keyboard::Legend(Keyboard::kKeyUp).arrow == Arrow::Up && Keyboard::Typed(Keyboard::kKeyUp, false) == ';');
    assert(Keyboard::Legend(Keyboard::kKeyDown).arrow == Arrow::Down && Keyboard::Typed(Keyboard::kKeyDown, false) == '.');
    assert(Keyboard::Legend(Keyboard::kKeyLeft).arrow == Arrow::Left && Keyboard::Typed(Keyboard::kKeyLeft, false) == ',');
    assert(Keyboard::Legend(Keyboard::kKeyRight).arrow == Arrow::Right && Keyboard::Typed(Keyboard::kKeyRight, false) == '/');
    assert(Keyboard::ArrowKey(Arrow::None) == 0);
    // None of the invented layouts.
    for (const char* letter : {"h", "j", "k", "l", "i", "w", "a", "s", "d"})
        for (int key = 1; key <= Keyboard::KEY_COUNT; ++key)
            if (std::strcmp(Keyboard::Legend(key).name, letter) == 0) assert(Keyboard::Legend(key).arrow == Arrow::None);
}

void PositionTests()
{
    // Number <-> position, y * 14 + x + 1, both ways.
    for (int key = 1; key <= Keyboard::KEY_COUNT; ++key)
    {
        const Keyboard::Point2D_t position = Keyboard::KeyPosition(key);
        assert(position.x >= 0 && position.x < 14 && position.y >= 0 && position.y < 4);
        assert(position.y * 14 + position.x + 1 == key);
        assert(Keyboard::KeyNumber(position) == key);
    }
    // Off the keyboard: no number, no position, no legend.
    for (const Keyboard::Point2D_t off : {Keyboard::Point2D_t{-1, -1}, Keyboard::Point2D_t{-1, 0}, Keyboard::Point2D_t{0, -1},
                                          Keyboard::Point2D_t{14, 0}, Keyboard::Point2D_t{0, 4}, Keyboard::Point2D_t{13, 4}})
    {
        assert(Keyboard::KeyNumber(off) == 0);
        assert(&Keyboard::Legend(off) == &Keyboard::kNoKeyLegend);
        assert(Keyboard::Typed(off, false) == 0 && Keyboard::Typed(off, true) == 0);
    }
    for (const int key : {-1, 0, 57, 255, 256 + 40})
    {
        const Keyboard::Point2D_t position = Keyboard::KeyPosition(key);
        assert(position.x == -1 && position.y == -1);
        const Keyboard::KeyLegend& legend = Keyboard::Legend(key);
        assert(legend.name[0] == '\0' && legend.character == 0 && legend.shifted == 0 && legend.arrow == Arrow::None);
    }
}

}  // namespace

int main()
{
    TableTests();
    NamedKeyTests();
    ArrowTests();
    PositionTests();
    std::cout << "key legends ok\n";
    return 0;
}
